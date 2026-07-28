#include "web_ui.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <uri/UriBraces.h>

#include "bus.h"
#include "devices.h"
#include "generated/web_assets.h"
#include "inputs.h"
#include "json_utils.h"
#include "metrics.h"
#include "mqtt_bridge.h"
#include "net_manager.h"
#include "rtc.h"
#include "schedules.h"
#include "sequences.h"
#include "settings.h"

namespace webserver {

namespace {

WebServer g_server(80);
bool g_restartPending = false;
uint32_t g_restartAt = 0;
bool g_otaError = false;
String g_otaMessage;

struct Session {
  String token;
  uint32_t expiresAt = 0;
};

Session g_sessions[4];
Session g_systemSessions[2];  // accesso alla sezione Sistema (password dedicata)

// Print che spedisce la risposta a blocchi: evita di tenere in RAM l'intero JSON di stato.
class ChunkedPrint : public Print {
 public:
  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *buffer, size_t size) override {
    size_t written = 0;
    while (written < size) {
      const size_t space = sizeof(buffer_) - used_;
      const size_t chunk = min(space, size - written);
      memcpy(buffer_ + used_, buffer + written, chunk);
      used_ += chunk;
      written += chunk;
      if (used_ == sizeof(buffer_)) flushBuffer();
    }
    return written;
  }

  void finish() {
    flushBuffer();
    g_server.sendContent("");
  }

 private:
  void flushBuffer() {
    if (used_ == 0) return;
    g_server.sendContent(reinterpret_cast<const char *>(buffer_), used_);
    used_ = 0;
  }

  uint8_t buffer_[1024];
  size_t used_ = 0;
};

String randomToken() {
  char buffer[33];
  for (size_t i = 0; i < 32; i += 8) {
    snprintf(buffer + i, 9, "%08x", static_cast<unsigned>(esp_random()));
  }
  buffer[32] = '\0';
  return String(buffer);
}

String issueToken(Session *sessions, size_t count, uint32_t ttlMs) {
  const String token = randomToken();
  size_t slot = 0;
  for (size_t i = 0; i < count; i++) {
    if (!sessions[i].token.length() ||
        static_cast<int32_t>(millis() - sessions[i].expiresAt) > 0) {
      slot = i;
      break;
    }
    if (sessions[i].expiresAt < sessions[slot].expiresAt) slot = i;
  }
  sessions[slot].token = token;
  sessions[slot].expiresAt = millis() + ttlMs;
  return token;
}

bool tokenValidIn(Session *sessions, size_t count, const String &token) {
  if (!token.length()) return false;
  for (size_t i = 0; i < count; i++) {
    if (sessions[i].token == token &&
        static_cast<int32_t>(millis() - sessions[i].expiresAt) < 0) {
      return true;
    }
  }
  return false;
}

String issueToken() { return issueToken(g_sessions, 4, 12UL * 60UL * 60UL * 1000UL); }

bool tokenValid(const String &token) { return tokenValidIn(g_sessions, 4, token); }

String requestToken() {
  if (g_server.hasHeader("Authorization")) {
    String value = g_server.header("Authorization");
    if (value.startsWith("Bearer ")) return value.substring(7);
  }
  if (g_server.hasArg("token")) return g_server.arg("token");
  if (g_server.hasHeader("Cookie")) {
    const String cookie = g_server.header("Cookie");
    const int index = cookie.indexOf("sheltr_token=");
    if (index >= 0) {
      const int start = index + 13;
      int end = cookie.indexOf(';', start);
      if (end < 0) end = cookie.length();
      return cookie.substring(start, end);
    }
  }
  if (g_server.hasArg("plain")) {
    JsonDocument doc(&SpiRamAllocator::instance());
    if (!deserializeJson(doc, g_server.arg("plain"))) {
      const char *token = doc["token"];
      if (token != nullptr) return String(token);
    }
  }
  return String();
}

void sendJson(int code, JsonDocument &doc) {
  String payload;
  serializeJson(doc, payload);
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(code, "application/json", payload);
}

void sendError(int code, const String &message) {
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = false;
  doc["error"] = message;
  sendJson(code, doc);
}

void sendOk() {
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  sendJson(200, doc);
}

bool requireAuth() {
  if (!cfg::config().auth.enabled) return true;
  if (tokenValid(requestToken())) return true;
  sendError(401, F("Autenticazione richiesta"));
  return false;
}

String systemToken() {
  if (g_server.hasHeader("X-Sheltr-System")) return g_server.header("X-Sheltr-System");
  if (g_server.hasArg("systemToken")) return g_server.arg("systemToken");
  return String();
}

bool systemUnlocked() { return tokenValidIn(g_systemSessions, 2, systemToken()); }

// La sezione Sistema (bus, rete, OTA, manutenzione, frame grezzi) è sempre protetta
// dalla password dedicata, anche quando il login generale è disattivato.
bool requireSystem() {
  if (!requireAuth()) return false;
  if (systemUnlocked()) return true;
  sendError(403, F("Sezione Sistema protetta: inserisci la password"));
  return false;
}

bool readBody(JsonDocument &doc) {
  if (!g_server.hasArg("plain")) return true;  // corpo vuoto ammesso
  const DeserializationError error = deserializeJson(doc, g_server.arg("plain"));
  if (error) {
    sendError(400, String(F("JSON non valido: ")) + error.c_str());
    return false;
  }
  return true;
}

String bodyString(JsonDocument &doc, const char *key, const String &fallback = String()) {
  if (doc[key].is<const char *>()) {
    String value = doc[key].as<const char *>();
    value.trim();
    return value;
  }
  if (g_server.hasArg(key)) return g_server.arg(key);
  return fallback;
}

void applyRuntimeConfig() {
  const cfg::BusCfg &bus = cfg::config().bus;
  Bus.reconfigure(bus.rx, bus.tx, bus.de, bus.baud, bus.timeoutMs);
  rtc::reconfigure();
  inputs::reconfigure();
  mqtt::reload();
}

// ---------------------------------------------------------------- pagine web

void sendIndex() {
  g_server.sendHeader("Content-Encoding", "gzip");
  g_server.sendHeader("Cache-Control", "no-cache");
  g_server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<PGM_P>(WEB_INDEX_GZ),
                  WEB_INDEX_GZ_LEN);
}

void handleNotFound() {
  if (g_server.uri().startsWith("/api/")) {
    sendError(404, F("Endpoint non trovato"));
    return;
  }
  if (net::apActive()) {
    // Captive portal: qualunque URL porta alla pagina di configurazione.
    g_server.sendHeader("Location", String("http://") + net::apIp() + "/", true);
    g_server.send(302, "text/plain", "");
    return;
  }
  sendIndex();  // SPA: tutte le rotte servono la stessa pagina
}

// ------------------------------------------------------------------- stato

void writeStatus(bool refresh, const std::vector<uint8_t> *addresses) {
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  devices::statusJson(root, refresh, addresses);
  sequences::statusJson(root["sequencer"].to<JsonObject>());
  inputs::statusJson(root["inputs"].to<JsonArray>());
  if (refresh) mqtt::publishStates();

  g_server.sendHeader("Cache-Control", "no-store");
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.send(200, "application/json", "");
  ChunkedPrint out;
  serializeJson(doc, out);
  out.finish();
}

void handleStatus() {
  if (!requireAuth()) return;
  const bool refresh = g_server.hasArg("refresh") && g_server.arg("refresh") != "0" &&
                       g_server.arg("refresh") != "false";
  writeStatus(refresh, nullptr);
}

void handlePoll() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;

  std::vector<uint8_t> addresses;
  if (body["address"].is<int>()) {
    addresses.push_back(static_cast<uint8_t>(body["address"].as<int>()));
  } else {
    const String channelId = bodyString(body, "channelId");
    if (channelId.length()) {
      devices::Entity entity;
      if (!devices::entityById(channelId, entity)) {
        sendError(404, F("Dispositivo non trovato per il polling"));
        return;
      }
      addresses.push_back(entity.address);
    }
  }
  if (addresses.empty()) addresses = cfg::allAddresses();
  writeStatus(true, &addresses);
}

void sendCommandResult(const devices::CommandResult &result) {
  if (!result.ok) {
    JsonDocument doc(&SpiRamAllocator::instance());
    doc["ok"] = false;
    doc["error"] = result.error;
    doc["frame"] = result.requestHex;
    if (result.responseHex.length()) doc["response"] = result.responseHex;
    sendJson(502, doc);
    return;
  }
  mqtt::publishStates();
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["frame"] = result.requestHex;
  if (result.responseHex.length()) doc["response"] = result.responseHex;
  JsonObject verification = doc["verification"].to<JsonObject>();
  verification["acknowledged"] = true;
  verification["pollVerified"] = result.pollVerified;
  sendJson(200, doc);
}

String targetId(JsonDocument &body, const char *idKey) {
  String id = bodyString(body, idKey);
  if (!id.length()) id = bodyString(body, "channelId");
  if (!id.length()) id = bodyString(body, "id");
  if (id.length()) return id;

  // Selezione per indirizzo + canale, come nel portale.
  if (body["address"].is<int>() && body["channel"].is<int>()) {
    const uint8_t address = body["address"].as<int>();
    const uint8_t channel = body["channel"].as<int>();
    for (const cfg::Board &board : cfg::config().boards) {
      if (board.address != address) continue;
      for (const cfg::Channel &item : board.channels) {
        if (item.channel == channel) return cfg::entityId(board.id, item.channel);
      }
    }
  }
  return String();
}

void handleLightCommand() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = targetId(body, "lightId");
  if (!id.length()) {
    sendError(400, F("Specifica lightId (o address + channel)"));
    return;
  }
  sendCommandResult(devices::commandLight(id, bodyString(body, "action", "on")));
}

void handleDimmerCommand() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = targetId(body, "dimmerId");
  if (!id.length()) {
    sendError(400, F("Specifica dimmerId (o address + channel)"));
    return;
  }
  int level = -1;
  if (body["level"].is<int>() || body["level"].is<float>()) level = body["level"].as<int>();
  String action = bodyString(body, "action");
  if (!action.length()) action = level >= 0 ? "set" : "on";
  sendCommandResult(devices::commandDimmer(id, action, level));
}

void handleShutterCommand() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = targetId(body, "shutterId");
  if (!id.length()) {
    sendError(400, F("Specifica shutterId (o address + channel)"));
    return;
  }
  sendCommandResult(devices::commandShutter(id, bodyString(body, "action", "stop")));
}

void handleThermostatCommand() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = targetId(body, "thermostatId");
  if (!id.length()) {
    sendError(400, F("Specifica thermostatId (o address + channel)"));
    return;
  }

  bool hasSetpoint = false;
  float setpoint = 21.0f;
  if (body["setpoint"].is<float>() || body["setpoint"].is<int>()) {
    setpoint = body["setpoint"].as<float>();
    hasSetpoint = true;
  } else if (body["set"].is<float>() || body["set"].is<int>()) {
    setpoint = body["set"].as<float>();
    hasSetpoint = true;
  } else if (body["setpoint"].is<const char *>()) {
    String text = body["setpoint"].as<const char *>();
    text.replace(',', '.');
    if (text.length()) {
      setpoint = text.toFloat();
      hasSetpoint = true;
    }
  }

  const String mode = bodyString(body, "mode");
  int8_t power = -1;
  if (body["power"].is<bool>()) {
    power = body["power"].as<bool>() ? 1 : 0;
  } else if (body["power"].is<const char *>()) {
    String text = body["power"].as<const char *>();
    text.toLowerCase();
    if (text == "on" || text == "true" || text == "1") power = 1;
    if (text == "off" || text == "false" || text == "0") power = 0;
  }

  sendCommandResult(devices::commandThermostat(id, hasSetpoint, setpoint, mode, power));
}

void handleRawFrame() {
  if (!requireSystem()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String hex = bodyString(body, "hex", bodyString(body, "payload"));
  uint8_t frame[protocol::FRAME_LEN];
  if (!hex.length() || !protocol::extractHex(hex.c_str(), hex.length(), frame)) {
    sendError(400, F("Frame non valido: servono 14 byte esadecimali"));
    return;
  }
  const devices::CommandResult result = devices::sendRawFrame(frame);
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = result.ok;
  doc["frame"] = result.requestHex;
  doc["responseHex"] = result.responseHex;
  if (!result.ok) doc["error"] = result.error;
  sendJson(result.ok ? 200 : 502, doc);
}

// ------------------------------------------------------------ configurazione

void handleGetConfig() {
  if (!requireAuth()) return;

  // `?secrets=1` esporta anche le password: serve per un backup davvero ripristinabile,
  // quindi è consentito solo con la sezione Sistema sbloccata.
  const bool wantSecrets = g_server.hasArg("secrets") && g_server.arg("secrets") != "0";
  if (wantSecrets && !requireSystem()) return;

  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  cfg::toJson(root, wantSecrets);
  root["exportedAt"] = devices::isoTimestamp();
  root["firmware"] = SHELTR_FW_VERSION;

  g_server.sendHeader("Cache-Control", "no-store");
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.send(200, "application/json", "");
  ChunkedPrint out;
  serializeJson(doc, out);
  out.finish();
}

void handlePutConfig() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;

  // Le impostazioni di rete si applicano solo al riavvio: lo segnaliamo all'interfaccia.
  const cfg::NetworkCfg before = cfg::config().network;

  String error;
  if (!cfg::applyJson(body.as<JsonObjectConst>(), error)) {
    sendError(400, error);
    return;
  }
  if (!cfg::save()) {
    sendError(500, F("Salvataggio configurazione fallito"));
    return;
  }
  applyRuntimeConfig();

  const cfg::NetworkCfg &after = cfg::config().network;
  const bool restartRequired = before.hostname != after.hostname ||
                               before.ethEnabled != after.ethEnabled ||
                               before.dhcp != after.dhcp || before.ip != after.ip ||
                               before.gateway != after.gateway || before.subnet != after.subnet ||
                               before.dns1 != after.dns1 || before.dns2 != after.dns2;

  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["revision"] = cfg::config().revision;
  doc["restartRequired"] = restartRequired;
  sendJson(200, doc);
}

void handleRoomColor() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String room = bodyString(body, "room");
  const String color = bodyString(body, "color");
  if (!room.length() || !color.startsWith("#")) {
    sendError(400, F("Specifica room e color (#rrggbb)"));
    return;
  }
  cfg::setRoomColor(room, color);
  cfg::save();
  sendOk();
}

// ------------------------------------------------------ favoriti e sequenze

void handleFavorite() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = bodyString(body, "id", bodyString(body, "channelId"));
  if (!id.length()) {
    sendError(400, F("Specifica l'id del dispositivo o della sequenza"));
    return;
  }
  const bool favorite = body["favorite"].is<bool>() ? body["favorite"].as<bool>() : true;

  cfg::Board *board = nullptr;
  cfg::Channel *channel = cfg::findChannel(id, &board);
  if (channel != nullptr) {
    channel->favorite = favorite;
  } else {
    cfg::Sequence *sequence = cfg::findSequence(id);
    if (sequence == nullptr) {
      sendError(404, F("Dispositivo o sequenza non trovati"));
      return;
    }
    sequence->favorite = favorite;
  }

  if (!cfg::save()) {
    sendError(500, F("Salvataggio preferito fallito"));
    return;
  }
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["id"] = id;
  doc["favorite"] = favorite;
  sendJson(200, doc);
}

void sequencesJson(JsonObject out) {
  JsonArray list = out["sequences"].to<JsonArray>();
  for (const cfg::Sequence &sequence : cfg::config().sequences) {
    JsonObject item = list.add<JsonObject>();
    item["id"] = sequence.id;
    item["name"] = sequence.name;
    item["room"] = sequence.room;
    item["favorite"] = sequence.favorite;
    item["busTrigger"] = sequence.busTrigger;
    item["scheduleEnabled"] = sequence.schedule.enabled;
    item["steps"] = static_cast<uint32_t>(sequence.steps.size());
    item["running"] = sequences::running(sequence.id);
  }
  sequences::statusJson(out["runner"].to<JsonObject>());
}

void handleSequences() {
  if (!requireAuth()) return;
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  root["ok"] = true;
  sequencesJson(root);
  sendJson(200, doc);
}

void runSequenceById(const String &id) {
  String error;
  if (!sequences::start(id, F("interfaccia"), error)) {
    sendError(sequences::running(id) ? 409 : 404, error);
    return;
  }
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["id"] = id;
  doc["running"] = true;
  sendJson(200, doc);
}

void handleSequenceRunPath() {
  if (!requireAuth()) return;
  runSequenceById(g_server.pathArg(0));
}

void handleSequenceRun() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = bodyString(body, "id", bodyString(body, "sequenceId"));
  if (!id.length()) {
    sendError(400, F("Specifica l'id della sequenza"));
    return;
  }
  runSequenceById(id);
}

void handleSequenceStop() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String id = bodyString(body, "id", bodyString(body, "sequenceId"));
  if (id.length()) {
    sequences::stop(id);
  } else {
    sequences::stopAll();
  }
  sendOk();
}

// Assegnazione della sequenza a un ingresso: si fa dal Controllo, quindi non
// richiede la password di Sistema (i parametri elettrici sì).
void handleInputAssign() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const int index = body["index"].is<int>() ? body["index"].as<int>() : -1;
  if (index < 0 || index >= static_cast<int>(cfg::INPUT_COUNT) ||
      index >= static_cast<int>(cfg::config().inputs.size())) {
    sendError(400, F("Indice ingresso non valido (0..7)"));
    return;
  }

  cfg::InputCfg &target = cfg::config().inputs[index];
  const String sequenceId = bodyString(body, "sequenceId");
  if (sequenceId.length() && cfg::findSequence(sequenceId) == nullptr) {
    sendError(404, F("Sequenza non trovata"));
    return;
  }
  target.sequenceId = sequenceId;
  const String name = bodyString(body, "name");
  if (name.length()) target.name = name;

  if (!cfg::save()) {
    sendError(500, F("Salvataggio ingresso fallito"));
    return;
  }
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["index"] = index;
  doc["sequenceId"] = target.sequenceId;
  sendJson(200, doc);
}

void handleRtcAction() {
  if (!requireSystem()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String action = bodyString(body, "action", "fromRtc");
  String error;
  bool ok = false;

  if (action == "fromRtc" || action == "read") {
    ok = rtc::syncFromRtc(error);
  } else if (action == "toRtc" || action == "write") {
    ok = rtc::syncToRtc(error);
  } else if (action == "set") {
    ok = rtc::setDateTime(bodyString(body, "time"), error);
  } else {
    sendError(400, F("Azione non valida (fromRtc, toRtc, set)"));
    return;
  }

  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = ok;
  if (!ok) doc["error"] = error;
  doc["time"] = devices::isoTimestamp();
  rtc::statusJson(doc["rtc"].to<JsonObject>());
  sendJson(ok ? 200 : 502, doc);
}

// -------------------------------------------------------------------- sistema

void systemJson(JsonObject out) {
  const cfg::Config &current = cfg::config();
  out["id"] = current.device.id;
  out["name"] = current.device.name;
  out["firmware"] = SHELTR_FW_VERSION;
  out["board"] = SHELTR_BOARD_NAME;
  out["protocolVersion"] = "1.6";
  out["uptimeSec"] = millis() / 1000;
  out["freeHeap"] = ESP.getFreeHeap();
  out["freePsram"] = ESP.getFreePsram();
  out["sketchSize"] = ESP.getSketchSize();
  out["freeSketchSpace"] = ESP.getFreeSketchSpace();
  out["time"] = devices::isoTimestamp();
  out["timeSynced"] = devices::timeSynced();
  out["authEnabled"] = current.auth.enabled;
  out["filesystem"] = cfg::filesystemMounted() ? "montato" : "NON montato";

  net::statusJson(out["network"].to<JsonObject>());
  mqtt::statusJson(out["mqtt"].to<JsonObject>());

  JsonObject bus = out["bus"].to<JsonObject>();
  bus["tx"] = Bus.txPin();
  bus["rx"] = Bus.rxPin();
  bus["de"] = Bus.dePin();
  bus["baud"] = Bus.baud();
  bus["timeoutMs"] = Bus.timeoutMs();
  bus["framesSent"] = Bus.sentCount();
  bus["framesOk"] = Bus.okCount();
  bus["framesError"] = Bus.errorCount();
  bus["lastError"] = Bus.lastError();
  bus["lastPollAgoMs"] = devices::lastPollAt() ? millis() - devices::lastPollAt() : 0;

  JsonObject profiles = out["schedules"].to<JsonObject>();
  profiles["appliedCount"] = schedules::appliedCount();
  profiles["lastRunAgoMs"] = schedules::lastRunAt() ? millis() - schedules::lastRunAt() : 0;

  sequences::statusJson(out["sequencer"].to<JsonObject>());
  metrics::statusJson(out["performance"].to<JsonObject>());
  rtc::statusJson(out["rtc"].to<JsonObject>());
  inputs::statusJson(out["inputs"].to<JsonArray>());
  out["bus"]["triggerCount"] = Bus.triggerCount();
  out["bus"]["lastTrigger"] = Bus.lastTrigger();
}

void handleSystem() {
  if (!requireSystem()) return;
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  systemJson(root);
  sendJson(200, doc);
}

void handleSystemUnlock() {
  if (!requireAuth()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String password = bodyString(body, "password");
  if (!password.length() || password != cfg::config().auth.systemPassword) {
    delay(400);  // rallenta i tentativi a forza bruta
    sendError(401, F("Password Sistema non valida"));
    return;
  }
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["token"] = issueToken(g_systemSessions, 2, 30UL * 60UL * 1000UL);
  doc["expiresInSec"] = 1800;
  sendJson(200, doc);
}

void handleSystemLock() {
  const String token = systemToken();
  for (Session &session : g_systemSessions) {
    if (session.token == token) {
      session.token = "";
      session.expiresAt = 0;
    }
  }
  sendOk();
}

void handleMeta() {
  // Endpoint pubblico: serve alla pagina di login e al captive portal.
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["id"] = cfg::config().device.id;
  doc["name"] = cfg::config().device.name;
  doc["firmware"] = SHELTR_FW_VERSION;
  doc["board"] = SHELTR_BOARD_NAME;
  doc["deviceType"] = "sheltr_esp";
  doc["protocolVersion"] = "1.6";
  doc["authRequired"] = cfg::config().auth.enabled;
  doc["authenticated"] = !cfg::config().auth.enabled || tokenValid(requestToken());
  doc["systemUnlocked"] = systemUnlocked();
  doc["apMode"] = net::apActive();
  doc["online"] = net::online();
  doc["hostname"] = cfg::config().network.hostname + ".local";
  doc["boardsConfigured"] = static_cast<uint32_t>(cfg::config().boards.size());
  doc["filesystem"] = cfg::filesystemMounted();

  // Riepilogo rete per l'intestazione: la pagina Sistema è protetta da password,
  // ma l'indicatore di connessione deve restare visibile.
  JsonObject network = doc["network"].to<JsonObject>();
  if (net::ethConnected()) {
    network["interface"] = "ethernet";
    network["ip"] = net::ethIp();
  } else if (net::wifiConnected()) {
    network["interface"] = "wifi";
    network["ip"] = net::wifiIp();
    network["ssid"] = net::wifiSsid();
  } else if (net::apActive()) {
    network["interface"] = "hotspot";
    network["ip"] = net::apIp();
    network["ssid"] = net::apSsid();
  } else {
    network["interface"] = "offline";
    network["ip"] = "";
  }
  sendJson(200, doc);
}

void handleRestart() {
  if (!requireSystem()) return;
  g_restartPending = true;
  g_restartAt = millis() + 600;
  sendOk();
}

void handleFactoryReset() {
  if (!requireSystem()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const bool keepNetwork = body["keepNetwork"].is<bool>() ? body["keepNetwork"].as<bool>() : true;
  cfg::resetDefaults(keepNetwork);
  cfg::save();
  g_restartPending = true;
  g_restartAt = millis() + 800;
  sendOk();
}

// -------------------------------------------------------------- provisioning

// In modalità hotspot il provisioning WiFi resta libero: è il primo avvio e
// l'access point è già protetto dalla propria password.
bool requireSystemUnlessProvisioning() {
  if (net::apActive()) return true;
  return requireSystem();
}

void handleWifiScan() {
  if (!requireSystemUnlessProvisioning()) return;
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  root["ok"] = true;
  net::scanNetworks(root["networks"].to<JsonArray>());
  sendJson(200, doc);
}

void handleWifiConnect() {
  if (!requireSystemUnlessProvisioning()) return;
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const String ssid = bodyString(body, "ssid");
  const String password = bodyString(body, "password");
  if (!ssid.length()) {
    sendError(400, F("SSID mancante"));
    return;
  }

  const bool connected = net::connectWifi(ssid, password, 18000);
  cfg::config().network.wifiSsid = ssid;
  cfg::config().network.wifiPassword = password;
  cfg::save();

  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = connected;
  doc["connected"] = connected;
  doc["ip"] = net::wifiIp();
  doc["hostname"] = cfg::config().network.hostname + ".local";
  if (!connected) doc["error"] = F("Connessione WiFi non riuscita: controlla la password");
  sendJson(connected ? 200 : 502, doc);
}

// --------------------------------------------------------------------- auth

void handleAuthState() {
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["authRequired"] = cfg::config().auth.enabled;
  doc["authenticated"] = !cfg::config().auth.enabled || tokenValid(requestToken());
  sendJson(200, doc);
}

void handleLogin() {
  JsonDocument body(&SpiRamAllocator::instance());
  if (!readBody(body)) return;
  const cfg::AuthCfg &auth = cfg::config().auth;
  if (!auth.enabled) {
    JsonDocument doc(&SpiRamAllocator::instance());
    doc["ok"] = true;
    doc["token"] = "";
    doc["authRequired"] = false;
    sendJson(200, doc);
    return;
  }
  const String username = bodyString(body, "username");
  const String password = bodyString(body, "password");
  if (username != auth.username || password != auth.password) {
    delay(400);  // rallenta i tentativi a forza bruta
    sendError(401, F("Credenziali non valide"));
    return;
  }
  const String token = issueToken();
  g_server.sendHeader("Set-Cookie", "sheltr_token=" + token + "; Path=/; Max-Age=43200; SameSite=Lax");
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = true;
  doc["token"] = token;
  doc["expiresInSec"] = 43200;
  sendJson(200, doc);
}

void handleLogout() {
  const String token = requestToken();
  for (Session &session : g_sessions) {
    if (session.token == token) {
      session.token = "";
      session.expiresAt = 0;
    }
  }
  g_server.sendHeader("Set-Cookie", "sheltr_token=; Path=/; Max-Age=0");
  sendOk();
}

// ---------------------------------------------------------------------- OTA

void handleOtaUpload() {
  HTTPUpload &upload = g_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    g_otaError = false;
    g_otaMessage = "";
    // L'autenticazione va verificata qui: il gestore POST viene eseguito solo a upload
    // concluso, quando la partizione sarebbe già stata riscritta.
    if (cfg::config().auth.enabled && !tokenValid(requestToken())) {
      g_otaError = true;
      g_otaMessage = F("Autenticazione richiesta");
      return;
    }
    if (!systemUnlocked()) {
      g_otaError = true;
      g_otaMessage = F("Sezione Sistema protetta: inserisci la password");
      return;
    }
    log_i("OTA: avvio aggiornamento (%s)", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      g_otaError = true;
      g_otaMessage = Update.errorString();
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && !g_otaError) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      g_otaError = true;
      g_otaMessage = Update.errorString();
    }
  } else if (upload.status == UPLOAD_FILE_END && !g_otaError) {
    if (!Update.end(true)) {
      g_otaError = true;
      g_otaMessage = Update.errorString();
    } else {
      log_i("OTA completato: %u byte", static_cast<unsigned>(upload.totalSize));
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    g_otaError = true;
    g_otaMessage = F("Upload interrotto");
  }
}

void handleOtaFinish() {
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["ok"] = !g_otaError;
  if (g_otaError) {
    doc["error"] = g_otaMessage.length() ? g_otaMessage : String(F("Aggiornamento fallito"));
    sendJson(500, doc);
    return;
  }
  doc["restarting"] = true;
  sendJson(200, doc);
  g_restartPending = true;
  g_restartAt = millis() + 800;
}

// ------------------------------------------- alias compatibili Sheltr Cloud

void handleInstances() {
  if (!requireAuth()) return;
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonArray instances = doc["instances"].to<JsonArray>();
  JsonObject item = instances.add<JsonObject>();
  item["id"] = cfg::config().device.id;
  item["name"] = cfg::config().device.name;
  item["deviceType"] = "sheltr_esp";
  sendJson(200, doc);
}

void handleInstanceMeta() {
  if (!requireAuth()) return;
  const cfg::Config &current = cfg::config();
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["id"] = current.device.id;
  doc["name"] = current.device.name;
  doc["deviceType"] = "sheltr_esp";
  doc["protocolVersion"] = "1.6";
  doc["firmware"] = SHELTR_FW_VERSION;
  doc["requiresLogin"] = current.auth.enabled;
  doc["dataSaver"] = false;
  JsonArray palette = doc["roomPalette"].to<JsonArray>();
  for (size_t i = 0; i < cfg::ROOM_PALETTE_SIZE; i++) palette.add(cfg::ROOM_PALETTE[i]);
  sendJson(200, doc);
}

void handleInstanceAuthState() {
  JsonDocument doc(&SpiRamAllocator::instance());
  doc["requiresLogin"] = cfg::config().auth.enabled;
  doc["authenticated"] = !cfg::config().auth.enabled || tokenValid(requestToken());
  sendJson(200, doc);
}

void registerRoutes() {
  const char *headerKeys[] = {"Authorization", "Cookie", "Content-Type", "X-Sheltr-System"};
  g_server.collectHeaders(headerKeys, 4);

  g_server.on("/", HTTP_GET, sendIndex);
  g_server.on("/control", HTTP_GET, sendIndex);
  g_server.on("/config", HTTP_GET, sendIndex);
  g_server.on("/setup", HTTP_GET, sendIndex);

  // Rilevamento captive portal dei sistemi operativi.
  g_server.on("/generate_204", HTTP_GET, handleNotFound);
  g_server.on("/gen_204", HTTP_GET, handleNotFound);
  g_server.on("/hotspot-detect.html", HTTP_GET, handleNotFound);
  g_server.on("/connecttest.txt", HTTP_GET, handleNotFound);
  g_server.on("/ncsi.txt", HTTP_GET, handleNotFound);

  g_server.on("/api/meta", HTTP_GET, handleMeta);
  g_server.on("/api/system", HTTP_GET, handleSystem);
  g_server.on("/api/system/unlock", HTTP_POST, handleSystemUnlock);
  g_server.on("/api/system/lock", HTTP_POST, handleSystemLock);
  g_server.on("/api/system/restart", HTTP_POST, handleRestart);
  g_server.on("/api/system/factory-reset", HTTP_POST, handleFactoryReset);
  g_server.on("/api/system/ota", HTTP_POST, handleOtaFinish, handleOtaUpload);

  g_server.on("/api/config", HTTP_GET, handleGetConfig);
  g_server.on("/api/config", HTTP_PUT, handlePutConfig);
  g_server.on("/api/config", HTTP_POST, handlePutConfig);

  g_server.on("/api/status", HTTP_GET, handleStatus);
  g_server.on("/api/poll", HTTP_POST, handlePoll);
  g_server.on("/api/lights/command", HTTP_POST, handleLightCommand);
  g_server.on("/api/dimmers/command", HTTP_POST, handleDimmerCommand);
  g_server.on("/api/shutters/command", HTTP_POST, handleShutterCommand);
  g_server.on("/api/thermostats/command", HTTP_POST, handleThermostatCommand);
  g_server.on("/api/frame", HTTP_POST, handleRawFrame);
  g_server.on("/api/rooms/color", HTTP_PUT, handleRoomColor);
  g_server.on("/api/rooms/color", HTTP_POST, handleRoomColor);
  g_server.on("/api/favorites", HTTP_POST, handleFavorite);
  g_server.on("/api/favorites", HTTP_PUT, handleFavorite);

  g_server.on("/api/inputs", HTTP_POST, handleInputAssign);
  g_server.on("/api/inputs", HTTP_PUT, handleInputAssign);
  g_server.on("/api/system/rtc", HTTP_POST, handleRtcAction);

  g_server.on("/api/sequences", HTTP_GET, handleSequences);
  g_server.on("/api/sequences/run", HTTP_POST, handleSequenceRun);
  g_server.on("/api/sequences/stop", HTTP_POST, handleSequenceStop);
  g_server.on(UriBraces("/api/sequences/{}/run"), HTTP_POST, handleSequenceRunPath);

  g_server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  g_server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);

  g_server.on("/api/auth", HTTP_GET, handleAuthState);
  g_server.on("/api/auth/login", HTTP_POST, handleLogin);
  g_server.on("/api/auth/logout", HTTP_POST, handleLogout);

  // Compatibilità con il portale Sheltr Cloud e con l'integrazione Home Assistant:
  // le stesse rotte `/api/instances/<id>/...` rispondono sul dispositivo locale.
  g_server.on("/api/instances", HTTP_GET, handleInstances);
  g_server.on(UriBraces("/api/instances/{}"), HTTP_GET, handleInstanceMeta);
  g_server.on(UriBraces("/api/instances/{}/auth"), HTTP_GET, handleInstanceAuthState);
  g_server.on(UriBraces("/api/instances/{}/auth/login"), HTTP_POST, handleLogin);
  g_server.on(UriBraces("/api/instances/{}/auth/logout"), HTTP_POST, handleLogout);
  g_server.on(UriBraces("/api/instances/{}/status"), HTTP_GET, handleStatus);
  g_server.on(UriBraces("/api/instances/{}/poll"), HTTP_POST, handlePoll);
  g_server.on(UriBraces("/api/instances/{}/lights/command"), HTTP_POST, handleLightCommand);
  g_server.on(UriBraces("/api/instances/{}/dimmers/command"), HTTP_POST, handleDimmerCommand);
  g_server.on(UriBraces("/api/instances/{}/shutters/command"), HTTP_POST, handleShutterCommand);
  g_server.on(UriBraces("/api/instances/{}/thermostats/command"), HTTP_POST,
              handleThermostatCommand);
  g_server.on(UriBraces("/api/instances/{}/rooms/color"), HTTP_PUT, handleRoomColor);

  g_server.onNotFound(handleNotFound);
}

}  // namespace

void begin() {
  registerRoutes();
  // Niente CORS permissivo: l'interfaccia è servita dallo stesso host e un
  // Access-Control-Allow-Origin aperto permetterebbe a qualunque sito visitato
  // dall'utente di comandare l'impianto dalla rete locale.
  g_server.begin();
  log_i("Web server avviato sulla porta 80");
}

void loop() {
  g_server.handleClient();
  if (g_restartPending && static_cast<int32_t>(millis() - g_restartAt) > 0) {
    log_w("Riavvio richiesto dall'interfaccia web");
    ESP.restart();
  }
}

}  // namespace webserver
