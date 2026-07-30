#include "mqtt_bridge.h"

#include <PubSubClient.h>
#include <WiFi.h>

#include "devices.h"
#include "inputs.h"
#include "json_utils.h"
#include "net_manager.h"
#include "protocol.h"
#include "sequences.h"
#include "settings.h"

namespace mqtt {

namespace {

WiFiClient g_localSocket;
WiFiClient g_cloudSocket;
PubSubClient g_local(g_localSocket);
PubSubClient g_cloud(g_cloudSocket);

uint32_t g_nextLocalAttempt = 0;
uint32_t g_nextCloudAttempt = 0;
uint32_t g_nextStatePublish = 0;
uint32_t g_publishedRevision = 0;
// Stato pubblicato verso il portale: revisione già inviata, prossimo heartbeat e
// pausa minima fra due invii dovuti a cambi di stato ravvicinati.
uint32_t g_cloudPublishedRevision = 0;
uint32_t g_nextCloudHeartbeat = 0;
uint32_t g_nextCloudStatePublish = 0;
bool g_discoveryPublished = false;
bool g_cloudConfigPublished = false;
uint32_t g_localFailures = 0;
uint32_t g_cloudFailures = 0;
String g_localError;
String g_cloudError;

String boardSlug(const cfg::Board &board) { return cfg::slugify(board.id, "board"); }

String topicPrefix(const cfg::Board &board) {
  return cfg::config().mqtt.baseTopic + "/" + boardSlug(board);
}

String availabilityTopic(const cfg::Board &board) { return topicPrefix(board) + "/availability"; }

String bridgeStatusTopic() { return cfg::config().mqtt.baseTopic + "/bridge/status"; }

String cloudStatusTopic() { return cfg::config().cloud.instanceId + "/bridge/status"; }

void publishLocal(const String &topic, const String &payload, bool retain) {
  if (!g_local.connected()) return;
  if (!g_local.publish(topic.c_str(), reinterpret_cast<const uint8_t *>(payload.c_str()),
                       payload.length(), retain)) {
    log_w("Publish MQTT fallita: %s (%u byte)", topic.c_str(),
          static_cast<unsigned>(payload.length()));
  }
}

void deviceJson(const cfg::Board &board, JsonObject device) {
  JsonArray identifiers = device["identifiers"].to<JsonArray>();
  identifiers.add(String("sheltr_") + boardSlug(board));
  device["name"] = board.name;
  device["manufacturer"] = "Sheltr";
  device["model"] = String("board-") + board.kind;
  device["sw_version"] = SHELTR_FW_VERSION;
  // Il gateway è il dispositivo padre logico di tutte le schede.
  device["via_device"] = String("sheltr_") + cfg::slugify(cfg::config().device.id, "sheltr-esp");
}

void bridgeDeviceJson(JsonObject device) {
  JsonArray identifiers = device["identifiers"].to<JsonArray>();
  identifiers.add(String("sheltr_") + cfg::slugify(cfg::config().device.id, "sheltr-esp"));
  device["name"] = cfg::config().device.name;
  device["manufacturer"] = "Sheltr";
  device["model"] = SHELTR_BOARD_NAME;
  device["sw_version"] = SHELTR_FW_VERSION;
  device["configuration_url"] = String("http://") + cfg::config().network.hostname + ".local";
}

void publishDiscoveryPayload(const String &topic, JsonDocument &doc) {
  String payload;
  serializeJson(doc, payload);
  publishLocal(topic, payload, true);
}

void publishDiscovery() {
  const cfg::Config &current = cfg::config();
  if (!current.mqtt.discovery) return;
  const String prefix = current.mqtt.discoveryPrefix;
  uint16_t count = 0;

  for (const cfg::Board &board : current.boards) {
    const String slug = boardSlug(board);
    const String base = topicPrefix(board);
    const String availability = availabilityTopic(board);

    {
      JsonDocument doc(&SpiRamAllocator::instance());
      const String uniqueId = String("sheltr_") + slug + "_poll";
      doc["name"] = board.name + " Polling";
      doc["unique_id"] = uniqueId;
      doc["command_topic"] = base + "/poll/set";
      doc["payload_press"] = "POLL";
      doc["availability_topic"] = availability;
      deviceJson(board, doc["device"].to<JsonObject>());
      publishDiscoveryPayload(prefix + "/button/" + uniqueId + "/config", doc);
      count++;
    }

    for (const cfg::Channel &channel : board.channels) {
      const String uniqueId = String("sheltr_") + slug + "_ch" + channel.channel;
      const String channelBase = base + "/ch" + channel.channel;
      JsonDocument doc(&SpiRamAllocator::instance());
      doc["name"] = channel.name;
      doc["unique_id"] = uniqueId;
      doc["availability_topic"] = availability;
      String component;

      if (board.kind == "light") {
        component = "light";
        doc["command_topic"] = channelBase + "/set";
        doc["state_topic"] = channelBase + "/state";
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
      } else if (board.kind == "dimmer") {
        component = "light";
        doc["command_topic"] = channelBase + "/set";
        doc["state_topic"] = channelBase + "/state";
        doc["brightness_command_topic"] = channelBase + "/brightness/set";
        doc["brightness_state_topic"] = channelBase + "/brightness/state";
        doc["brightness_scale"] = 255;
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
      } else if (board.kind == "shutter") {
        component = "cover";
        doc["command_topic"] = channelBase + "/set";
        doc["state_topic"] = channelBase + "/state";
        doc["payload_open"] = "OPEN";
        doc["payload_close"] = "CLOSE";
        doc["payload_stop"] = "STOP";
        doc["state_open"] = "OPEN";
        doc["state_opening"] = "OPENING";
        doc["state_closed"] = "CLOSED";
        doc["state_closing"] = "CLOSING";
        doc["assumed_state"] = true;
        doc["optimistic"] = true;
      } else {
        component = "climate";
        doc["mode_command_topic"] = channelBase + "/mode/set";
        doc["mode_state_topic"] = channelBase + "/mode/state";
        doc["temperature_command_topic"] = channelBase + "/temperature/set";
        doc["temperature_state_topic"] = channelBase + "/setpoint/state";
        doc["current_temperature_topic"] = channelBase + "/temperature/state";
        doc["action_topic"] = channelBase + "/action/state";
        JsonArray modes = doc["modes"].to<JsonArray>();
        modes.add("off");
        modes.add("heat");
        modes.add("cool");
        doc["min_temp"] = 5;
        doc["max_temp"] = 30;
        doc["temp_step"] = 0.5;
        doc["temperature_unit"] = "C";
        doc["precision"] = 0.5;
      }

      JsonObject device = doc["device"].to<JsonObject>();
      deviceJson(board, device);
      doc["json_attributes_topic"] = channelBase + "/attributes";
      publishDiscoveryPayload(prefix + "/" + component + "/" + uniqueId + "/config", doc);

      JsonDocument attributes(&SpiRamAllocator::instance());
      attributes["board_id"] = board.id;
      attributes["address"] = board.address;
      attributes["channel"] = channel.channel;
      attributes["room"] = channel.room;
      attributes["kind"] = board.kind;
      String attributesPayload;
      serializeJson(attributes, attributesPayload);
      publishLocal(channelBase + "/attributes", attributesPayload, true);
      count++;
    }
  }

  // Sequenze: un pulsante Home Assistant per ogni scena configurata.
  for (const cfg::Sequence &sequence : current.sequences) {
    JsonDocument doc(&SpiRamAllocator::instance());
    const String uniqueId = String("sheltr_seq_") + cfg::slugify(sequence.id, "seq");
    doc["name"] = sequence.name;
    doc["unique_id"] = uniqueId;
    doc["command_topic"] = current.mqtt.baseTopic + "/sequence/" + sequence.id + "/set";
    doc["payload_press"] = "RUN";
    doc["availability_topic"] = bridgeStatusTopic();
    doc["icon"] = "mdi:play-box-multiple";
    bridgeDeviceJson(doc["device"].to<JsonObject>());
    publishDiscoveryPayload(prefix + "/button/" + uniqueId + "/config", doc);
    count++;
  }

  // Pulsanti di servizio del gateway
  struct BridgeButton {
    const char *suffix;
    const char *name;
    const char *topic;
    const char *payload;
  };
  const BridgeButton buttons[] = {
      {"poll_all", "Polling tutte le schede", "/poll_all/set", "POLL"},
      {"restart", "Riavvia gateway", "/service/restart/set", "RESTART"},
  };
  for (const BridgeButton &button : buttons) {
    JsonDocument doc(&SpiRamAllocator::instance());
    const String uniqueId = String("sheltr_") + cfg::slugify(current.device.id, "gateway") + "_" +
                            button.suffix;
    doc["name"] = button.name;
    doc["unique_id"] = uniqueId;
    doc["command_topic"] = current.mqtt.baseTopic + button.topic;
    doc["payload_press"] = button.payload;
    doc["availability_topic"] = bridgeStatusTopic();
    bridgeDeviceJson(doc["device"].to<JsonObject>());
    publishDiscoveryPayload(prefix + "/button/" + uniqueId + "/config", doc);
    count++;
  }

  log_i("Discovery Home Assistant pubblicata: %u entità", count);
  g_discoveryPublished = true;
}

void publishBoardStates() {
  const cfg::Config &current = cfg::config();
  for (const cfg::Board &board : current.boards) {
    const String base = topicPrefix(board);
    publishLocal(availabilityTopic(board), devices::boardOnline(board.address) ? "online" : "offline",
                 true);
    for (const cfg::Channel &channel : board.channels) {
      const String id = cfg::entityId(board.id, channel.channel);
      const String channelBase = base + "/ch" + channel.channel;
      if (board.kind == "light") {
        const devices::LightState *state = devices::lightState(id);
        if (state == nullptr || state->isOn < 0) continue;
        publishLocal(channelBase + "/state", state->isOn == 1 ? "ON" : "OFF", true);
      } else if (board.kind == "dimmer") {
        const devices::DimmerState *state = devices::dimmerState(id);
        if (state == nullptr) continue;
        const int brightness = constrain((state->level * 255) / 9, 0, 255);
        publishLocal(channelBase + "/state", state->level > 0 ? "ON" : "OFF", true);
        publishLocal(channelBase + "/brightness/state", String(brightness), true);
      } else if (board.kind == "shutter") {
        const devices::ShutterState *state = devices::shutterState(id);
        if (state == nullptr) continue;
        String value = "STOP";
        if (state->action == "up") value = "OPENING";
        if (state->action == "down") value = "CLOSING";
        publishLocal(channelBase + "/state", value, true);
      } else {
        const devices::ThermostatState *state = devices::thermostatState(id);
        if (state == nullptr) continue;
        const bool isOn = state->isOn != 0;
        const bool summer = state->mode == "summer";
        String hvacMode = "off";
        String hvacAction = "off";
        if (isOn) {
          hvacMode = summer ? "cool" : "heat";
          hvacAction = state->isActive == 1 ? (summer ? "cooling" : "heating") : "idle";
        }
        if (state->hasTemperature) {
          publishLocal(channelBase + "/temperature/state",
                       String(roundf(state->temperature * 10.0f) / 10.0f, 1), true);
        }
        publishLocal(channelBase + "/setpoint/state", String(state->setpoint, 1), true);
        publishLocal(channelBase + "/mode/state", hvacMode, true);
        publishLocal(channelBase + "/action/state", hvacAction, true);
        publishLocal(channelBase + "/power/state", isOn ? "ON" : "OFF", true);
      }
    }
  }
}

void handleLocalCommand(const String &topic, const String &payload) {
  const cfg::Config &current = cfg::config();
  const String base = current.mqtt.baseTopic + "/";
  if (!topic.startsWith(base)) return;
  const String tail = topic.substring(base.length());
  String value = payload;
  value.trim();
  String upper = value;
  upper.toUpperCase();

  if (tail == "poll_all/set") {
    devices::pollAll();
    publishBoardStates();
    return;
  }
  if (tail == "service/restart/set") {
    log_w("Riavvio richiesto da MQTT");
    delay(200);
    ESP.restart();
    return;
  }
  if (tail.startsWith("sequence/") && tail.endsWith("/set")) {
    const String id = tail.substring(9, tail.length() - 4);
    String error;
    if (!sequences::start(id, F("MQTT"), error)) {
      log_w("Sequenza '%s' non avviata: %s", id.c_str(), error.c_str());
    }
    return;
  }

  const int firstSlash = tail.indexOf('/');
  if (firstSlash <= 0) return;
  const String slug = tail.substring(0, firstSlash);
  String rest = tail.substring(firstSlash + 1);

  cfg::Board *board = nullptr;
  for (cfg::Board &item : cfg::config().boards) {
    if (boardSlug(item) == slug) board = &item;
  }
  if (board == nullptr) return;

  if (rest == "poll/set") {
    String error;
    devices::pollAddress(board->address, error);
    publishBoardStates();
    return;
  }

  if (!rest.startsWith("ch")) return;
  const int slash = rest.indexOf('/');
  if (slash < 0) return;
  const uint8_t channel = rest.substring(2, slash).toInt();
  const String action = rest.substring(slash + 1);
  const String id = cfg::entityId(board->id, channel);

  if (board->kind == "light" && action == "set") {
    devices::commandLight(id, upper == "TOGGLE" ? "toggle" : (upper == "ON" ? "on" : "off"));
  } else if (board->kind == "shutter" && action == "set") {
    String command = "stop";
    if (upper == "OPEN" || upper == "UP") command = "up";
    if (upper == "CLOSE" || upper == "DOWN") command = "down";
    devices::commandShutter(id, command);
  } else if (board->kind == "dimmer") {
    if (action == "brightness/set") {
      const int raw = value.toInt();
      int level = raw;
      if (raw > 9 && raw <= 100) level = static_cast<int>(roundf(raw * 9.0f / 100.0f));
      if (raw > 100) level = static_cast<int>(roundf(raw * 9.0f / 255.0f));
      devices::commandDimmer(id, "set", constrain(level, 0, 9));
    } else if (action == "set") {
      if (upper == "ON" || upper == "OFF" || upper == "TOGGLE") {
        String command = upper;
        command.toLowerCase();
        devices::commandDimmer(id, command, -1);
      } else {
        devices::commandDimmer(id, "set", constrain(value.toInt(), 0, 9));
      }
    }
  } else if (board->kind == "thermostat") {
    if (action == "temperature/set" || action == "setpoint/set") {
      devices::commandThermostat(id, true, value.toFloat(), "", -1);
    } else if (action == "mode/set") {
      if (upper == "OFF") {
        devices::commandThermostat(id, false, 0, "", 0);
      } else {
        devices::commandThermostat(id, false, 0, upper == "COOL" ? "summer" : "winter", 1);
      }
    } else if (action == "power/set") {
      devices::commandThermostat(id, false, 0, "", upper == "ON" ? 1 : 0);
    }
  }

  publishBoardStates();
}

void cloudInstanceJson(JsonObject out) {
  const cfg::Config &current = cfg::config();
  out["id"] = current.cloud.instanceId;
  out["name"] = current.cloud.instanceName;
  out["deviceType"] = "sheltr_mini";
  JsonObject device = out["device"].to<JsonObject>();
  device["type"] = "sheltr_esp";
  device["board"] = SHELTR_BOARD_NAME;
  device["firmware"] = SHELTR_FW_VERSION;
  out["protocolVersion"] = "1.6";

  JsonArray boards = out["boards"].to<JsonArray>();
  JsonArray deviceList = out["devices"].to<JsonArray>();
  for (const cfg::Board &board : current.boards) {
    JsonObject item = boards.add<JsonObject>();
    item["id"] = board.id;
    item["name"] = board.name;
    item["address"] = board.address;
    item["kind"] = board.kind;
    item["channelStart"] = board.channelStart;
    item["channelEnd"] = board.channelEnd;
    JsonArray channels = item["channels"].to<JsonArray>();
    for (const cfg::Channel &channel : board.channels) {
      JsonObject entry = channels.add<JsonObject>();
      entry["channel"] = channel.channel;
      entry["name"] = channel.name;
      entry["room"] = channel.room;
      entry["favorite"] = channel.favorite;
      entry["notifyOnChange"] = channel.notifyOnChange;
      // Il profilo viaggia anche verso il portale: così una modifica fatta qui in
      // locale si riflette sul cloud (sincronizzazione bidirezionale).
      cfg::profileJson(channel.profile, board.kind, entry["profile"].to<JsonObject>());

      JsonObject deviceEntry = deviceList.add<JsonObject>();
      deviceEntry["id"] = cfg::entityId(board.id, channel.channel);
      deviceEntry["kind"] = board.kind;
      deviceEntry["boardId"] = board.id;
      deviceEntry["boardName"] = board.name;
      deviceEntry["address"] = board.address;
      deviceEntry["channel"] = channel.channel;
      deviceEntry["name"] = channel.name;
      deviceEntry["room"] = channel.room;
    }
  }

  // Sequenze e ingressi: il portale li mostra in sola lettura (restano gestiti qui).
  JsonArray sequences = out["sequences"].to<JsonArray>();
  for (const cfg::Sequence &sequence : current.sequences) {
    // Definizione completa (passi e orari): il portale la usa per l'editor.
    cfg::sequenceJson(sequence, sequences.add<JsonObject>());
  }
  inputs::statusJson(out["inputs"].to<JsonArray>());

  // Colori stanze assegnati: il portale li rispecchia (per quelli non assegnati
  // entrambi derivano lo stesso colore dal nome).
  JsonObject roomColors = out["roomColors"].to<JsonObject>();
  for (const auto &entry : current.roomColors) {
    roomColors[entry.first] = entry.second;
  }

  JsonObject mqttInfo = out["mqtt"].to<JsonObject>();
  mqttInfo["baseTopic"] = current.cloud.instanceId;
  mqttInfo["configTopic"] = current.cloud.instanceId + "/config";
  mqttInfo["lightCommandTopic"] = current.cloud.instanceId + "/cmd";
  mqttInfo["lightResponseTopic"] = current.cloud.instanceId + "/pub";
  mqttInfo["lightPayloadFormat"] = current.cloud.payloadFormat;
  out["updatedAt"] = devices::isoTimestamp();
}

void publishCloudConfig() {
  if (!g_cloud.connected()) return;
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  cloudInstanceJson(root);
  const size_t length = measureJson(doc);
  const String topic = cfg::config().cloud.instanceId + "/config";
  if (!g_cloud.beginPublish(topic.c_str(), length, true)) {
    log_w("Publish configurazione cloud fallita");
    return;
  }
  serializeJson(doc, g_cloud);
  g_cloud.endPublish();
  g_cloudConfigPublished = true;
  log_i("Configurazione Sheltr Cloud pubblicata su %s (%u byte)", topic.c_str(),
        static_cast<unsigned>(length));
}

void handleCloudCommand(const uint8_t *payload, unsigned int length) {
  uint8_t frame[protocol::FRAME_LEN];
  if (!protocol::extractAny(payload, length, frame)) {
    log_w("Payload cloud non valido (%u byte)", length);
    return;
  }
  const devices::CommandResult result = devices::sendRawFrame(frame);
  if (!result.ok || !result.responseHex.length()) {
    log_w("Frame cloud senza risposta: %s", result.error.c_str());
    return;
  }
  uint8_t response[protocol::FRAME_LEN];
  if (!protocol::extractHex(result.responseHex.c_str(), result.responseHex.length(), response)) {
    return;
  }
  const String out = protocol::formatPayload(response, cfg::config().cloud.payloadFormat);
  const String topic = cfg::config().cloud.instanceId + "/pub";
  g_cloud.publish(topic.c_str(), reinterpret_cast<const uint8_t *>(out.c_str()), out.length(),
                  false);
}

void onLocalMessage(char *topic, uint8_t *payload, unsigned int length) {
  String value;
  value.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) value += static_cast<char>(payload[i]);
  handleLocalCommand(String(topic), value);
}

// Preferiti e profili orari impostati dal cloud: si applicano alla configurazione
// locale (che resta la fonte di verita' e li esegue anche a cloud irraggiungibile).
void handleCloudSettings(const uint8_t *payload, unsigned int length) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    log_w("Impostazioni cloud non valide: %s", error.c_str());
    return;
  }
  if (!cfg::applyCloudSettings(doc.as<JsonObjectConst>())) return;
  if (!cfg::save()) {
    log_w("Salvataggio impostazioni dal cloud fallito");
    return;
  }
  // I profili orari vengono riletti dalla configurazione a ogni giro di schedules::loop().
  log_i("Impostazioni applicate dal cloud (preferiti/profili)");
  publishCloudConfig();  // rimanda la config aggiornata al portale
}

// Azioni immediate richieste dal portale (avvio/stop sequenze): le esegue il gateway.
void handleCloudAction(const uint8_t *payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    log_w("Azione cloud non valida");
    return;
  }
  const String action = doc["action"].is<const char *>() ? String(doc["action"].as<const char *>()) : String();
  if (action == "sequence.run") {
    const String id = doc["id"].is<const char *>() ? String(doc["id"].as<const char *>()) : String();
    if (!id.length()) return;
    String error;
    if (!sequences::start(id, "cloud", error)) {
      log_w("Avvio sequenza '%s' dal cloud fallito: %s", id.c_str(), error.c_str());
      return;
    }
    log_i("Sequenza '%s' avviata dal cloud", id.c_str());
    return;
  }
  if (action == "sequence.stop") {
    sequences::stopAll();
    log_i("Sequenze interrotte dal cloud");
  }
}

// Ripubblica al portale lo stato reale delle schede: interroga il bus (frame 0x40)
// e manda la risposta sul topic di risposta, che è esattamente ciò che il portale
// interpreta per aggiornare le card, tenere il dispositivo "online" e inviare le
// notifiche di cambio stato. Usata sia dall'heartbeat sia dopo un comando locale.
void publishCloudBoardStates() {
  if (!g_cloud.connected()) return;
  const cfg::Config &current = cfg::config();
  const String topic = current.cloud.instanceId + "/pub";
  for (uint8_t address : cfg::allAddresses()) {
    uint8_t frame[protocol::FRAME_LEN];
    const uint8_t g[1] = {0};
    protocol::build(address, protocol::CMD_POLL, g, 0, frame);
    const devices::CommandResult result = devices::sendRawFrame(frame);
    if (!result.ok || !result.responseHex.length()) continue;
    uint8_t response[protocol::FRAME_LEN];
    if (!protocol::extractHex(result.responseHex.c_str(), result.responseHex.length(), response)) {
      continue;
    }
    const String out = protocol::formatPayload(response, current.cloud.payloadFormat);
    g_cloud.publish(topic.c_str(), reinterpret_cast<const uint8_t *>(out.c_str()), out.length(),
                    false);
    g_cloud.loop();  // svuota il buffer fra una scheda e l'altra
  }
}

void onCloudMessage(char *topic, uint8_t *payload, unsigned int length) {
  const String instanceId = cfg::config().cloud.instanceId;
  if (instanceId + "/cmd" == topic) {
    handleCloudCommand(payload, length);
    return;
  }
  if (instanceId + "/settings" == topic) {
    handleCloudSettings(payload, length);
    return;
  }
  if (instanceId + "/action" == topic) {
    handleCloudAction(payload, length);
  }
}

void connectLocal() {
  const cfg::MqttCfg &settings = cfg::config().mqtt;
  g_local.setServer(settings.host.c_str(), settings.port);
  g_local.setBufferSize(1024);
  g_local.setKeepAlive(30);
  g_local.setCallback(onLocalMessage);

  const String clientId =
      settings.clientId.length() ? settings.clientId : cfg::config().device.id;
  const String willTopic = bridgeStatusTopic();
  const bool connected =
      settings.username.length()
          ? g_local.connect(clientId.c_str(), settings.username.c_str(), settings.password.c_str(),
                            willTopic.c_str(), 0, true, "offline")
          : g_local.connect(clientId.c_str(), willTopic.c_str(), 0, true, "offline");

  if (!connected) {
    g_localFailures++;
    g_localError = String(F("connessione fallita, stato ")) + g_local.state();
    log_w("MQTT locale: %s", g_localError.c_str());
    return;
  }

  g_localError = "";
  log_i("MQTT locale connesso a %s:%u", settings.host.c_str(), settings.port);
  publishLocal(willTopic, "online", true);

  const String base = settings.baseTopic;
  g_local.subscribe((base + "/poll_all/set").c_str());
  g_local.subscribe((base + "/service/restart/set").c_str());
  g_local.subscribe((base + "/sequence/+/set").c_str());
  g_local.subscribe((base + "/+/+/set").c_str());
  g_local.subscribe((base + "/+/+/+/set").c_str());

  publishDiscovery();
  devices::pollAll();
  publishBoardStates();
}

void connectCloud() {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  g_cloud.setServer(settings.host.c_str(), settings.port);
  // Le impostazioni dal portale (preferiti, profili, sequenze complete) possono
  // superare i 2 KB: con un buffer troppo piccolo il messaggio verrebbe scartato.
  g_cloud.setBufferSize(8192);
  g_cloud.setKeepAlive(45);
  g_cloud.setCallback(onCloudMessage);

  const String clientId = settings.instanceId + "-esp";
  const String willTopic = cloudStatusTopic();
  const bool connected =
      settings.username.length()
          ? g_cloud.connect(clientId.c_str(), settings.username.c_str(), settings.password.c_str(),
                            willTopic.c_str(), 0, true, "offline")
          : g_cloud.connect(clientId.c_str(), willTopic.c_str(), 0, true, "offline");

  if (!connected) {
    g_cloudFailures++;
    g_cloudError = String(F("connessione fallita, stato ")) + g_cloud.state();
    log_w("MQTT cloud: %s", g_cloudError.c_str());
    return;
  }

  g_cloudError = "";
  log_i("MQTT Sheltr Cloud connesso a %s:%u", settings.host.c_str(), settings.port);
  g_cloud.publish(willTopic.c_str(), "online", true);
  g_cloud.subscribe((settings.instanceId + "/cmd").c_str());
  // QoS 1: alla sottoscrizione riceviamo subito il retained con preferiti/profili.
  g_cloud.subscribe((settings.instanceId + "/settings").c_str(), 1);
  g_cloud.subscribe((settings.instanceId + "/action").c_str(), 1);
  publishCloudConfig();
}

}  // namespace

void begin() {
  g_nextLocalAttempt = millis() + 3000;
  g_nextCloudAttempt = millis() + 5000;
}

void reload() {
  if (g_local.connected()) g_local.disconnect();
  if (g_cloud.connected()) g_cloud.disconnect();
  g_discoveryPublished = false;
  g_cloudConfigPublished = false;
  g_nextLocalAttempt = millis() + 500;
  g_nextCloudAttempt = millis() + 800;
  // Primo heartbeat poco dopo la riconnessione, così il portale vede subito lo stato.
  g_nextCloudHeartbeat = millis() + 5000;
  g_nextCloudStatePublish = 0;
}

void publishStates() {
  if (g_local.connected()) publishBoardStates();
  g_publishedRevision = devices::stateRevision();
}

void publishConfig() { publishCloudConfig(); }

void publishInputEvent(size_t index) {
  if (!g_cloud.connected()) return;
  const std::vector<cfg::InputCfg> &inputs = cfg::config().inputs;
  if (index >= inputs.size()) return;
  const cfg::InputCfg &item = inputs[index];
  if (!item.notifyOnChange) return;  // notifica non richiesta per questo ingresso

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "input";
  root["id"] = String("input-") + index;
  root["name"] = item.name;
  root["room"] = item.room;
  root["text"] = item.notifyText;
  root["active"] = true;
  String payload;
  serializeJson(doc, payload);
  const String topic = cfg::config().cloud.instanceId + "/event";
  g_cloud.publish(topic.c_str(), reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length(),
                  false);
}

void loop() {
  const cfg::Config &current = cfg::config();
  const uint32_t now = millis();

  if (current.mqtt.enabled && current.mqtt.host.length() && net::online()) {
    if (g_local.connected()) {
      g_local.loop();
    } else if (static_cast<int32_t>(now - g_nextLocalAttempt) > 0) {
      g_nextLocalAttempt = now + 8000;
      connectLocal();
    }
  } else if (g_local.connected()) {
    g_local.disconnect();
  }

  if (current.cloud.enabled && current.cloud.host.length() && net::online()) {
    if (g_cloud.connected()) {
      g_cloud.loop();
      if (!g_cloudConfigPublished) publishCloudConfig();

      // Qualcosa è cambiato in locale (comando dall'interfaccia, ingresso, sequenza,
      // profilo orario): avvisiamo il portale così si aggiorna e, se il canale ha la
      // notifica attiva, la invia. Con una pausa minima per non intasare il bus se
      // arrivano più cambi di fila.
      const bool cloudStateChanged = devices::stateRevision() != g_cloudPublishedRevision;
      if (cloudStateChanged && static_cast<int32_t>(now - g_nextCloudStatePublish) > 0) {
        g_nextCloudStatePublish = now + 3000;
        publishCloudBoardStates();
        // La revisione va allineata DOPO la pubblicazione: il polling che facciamo qui
        // aggiorna a sua volta lo stato interno e, se la leggessimo prima, il cambio si
        // ri-innescherebbe da solo a ogni giro (polling continuo sul bus).
        g_cloudPublishedRevision = devices::stateRevision();
        g_nextCloudHeartbeat = now + static_cast<uint32_t>(current.cloud.heartbeatSec) * 1000UL;
      } else if (current.cloud.heartbeatSec > 0 &&
                 static_cast<int32_t>(now - g_nextCloudHeartbeat) > 0) {
        g_nextCloudHeartbeat = now + static_cast<uint32_t>(current.cloud.heartbeatSec) * 1000UL;
        publishCloudBoardStates();
        g_cloudPublishedRevision = devices::stateRevision();
      }
    } else if (static_cast<int32_t>(now - g_nextCloudAttempt) > 0) {
      g_nextCloudAttempt = now + 10000;
      connectCloud();
    }
  } else if (g_cloud.connected()) {
    g_cloud.disconnect();
  }

  if (!g_local.connected()) return;

  const bool intervalElapsed = static_cast<int32_t>(now - g_nextStatePublish) > 0;
  const bool stateChanged = devices::stateRevision() != g_publishedRevision;
  if (intervalElapsed || stateChanged) {
    g_nextStatePublish = now + static_cast<uint32_t>(current.mqtt.stateIntervalSec) * 1000UL;
    g_publishedRevision = devices::stateRevision();
    publishBoardStates();
  }
}

bool localConnected() { return g_local.connected(); }
bool cloudConnected() { return g_cloud.connected(); }

void statusJson(JsonObject out) {
  const cfg::Config &current = cfg::config();
  JsonObject local = out["local"].to<JsonObject>();
  local["enabled"] = current.mqtt.enabled;
  local["connected"] = g_local.connected();
  local["host"] = current.mqtt.host;
  local["port"] = current.mqtt.port;
  local["baseTopic"] = current.mqtt.baseTopic;
  local["discovery"] = current.mqtt.discovery;
  local["discoveryPrefix"] = current.mqtt.discoveryPrefix;
  local["discoveryPublished"] = g_discoveryPublished;
  local["failures"] = g_localFailures;
  local["lastError"] = g_localError;

  JsonObject cloud = out["cloud"].to<JsonObject>();
  cloud["enabled"] = current.cloud.enabled;
  cloud["connected"] = g_cloud.connected();
  cloud["host"] = current.cloud.host;
  cloud["port"] = current.cloud.port;
  cloud["instanceId"] = current.cloud.instanceId;
  cloud["configTopic"] = current.cloud.instanceId + "/config";
  cloud["commandTopic"] = current.cloud.instanceId + "/cmd";
  cloud["responseTopic"] = current.cloud.instanceId + "/pub";
  cloud["payloadFormat"] = current.cloud.payloadFormat;
  cloud["configPublished"] = g_cloudConfigPublished;
  cloud["failures"] = g_cloudFailures;
  cloud["lastError"] = g_cloudError;
}

}  // namespace mqtt
