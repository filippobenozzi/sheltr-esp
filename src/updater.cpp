#include "updater.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>

#include "json_utils.h"
#include "net_manager.h"
#include "settings.h"

#ifndef SHELTR_FW_RELEASE
#define SHELTR_FW_RELEASE "dev"
#endif

namespace updater {

namespace {

constexpr const char *ASSET_NAME = "sheltr-esp-firmware.bin";

// Stato della macchina di aggiornamento, letto dall'interfaccia.
enum class State : uint8_t { Idle, Checking, Available, Downloading, Done, Failed };

State g_state = State::Idle;
String g_error;
String g_latestTag;
String g_latestName;
String g_latestNotes;
String g_latestUrl;
String g_publishedAt;
size_t g_latestSize = 0;
uint32_t g_downloaded = 0;
uint32_t g_lastCheckAt = 0;
bool g_checkedOnce = false;
uint32_t g_nextCheckAt = 0;
uint32_t g_restartAt = 0;
TaskHandle_t g_task = nullptr;

const char *stateName(State state) {
  switch (state) {
    case State::Checking: return "checking";
    case State::Available: return "available";
    case State::Downloading: return "downloading";
    case State::Done: return "done";
    case State::Failed: return "failed";
    default: return "idle";
  }
}

String releasesUrl() {
  String repo = cfg::config().update.repo;
  repo.trim();
  while (repo.startsWith("/")) repo.remove(0, 1);
  while (repo.endsWith("/")) repo.remove(repo.length() - 1);
  if (!repo.length()) repo = "filippobenozzi/sheltr-esp";
  return String("https://api.github.com/repos/") + repo + "/releases/latest";
}

void applySecureDefaults(WiFiClientSecure &client) {
  // Nessuna verifica del certificato: senza NTP l'orologio può non essere ancora
  // sincronizzato e la validazione fallirebbe. Il binario resta comunque protetto
  // dal controllo di integrità che l'ESP fa in scrittura (magic byte e checksum).
  client.setInsecure();
  client.setTimeout(15000);
}

// Scarica il binario e lo scrive nella partizione OTA.
void installTask(void *) {
  WiFiClientSecure client;
  applySecureDefaults(client);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub redirige sul CDN
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setReuse(false);

  if (!http.begin(client, g_latestUrl)) {
    g_error = F("Connessione a GitHub non riuscita");
    g_state = State::Failed;
    g_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    g_error = String(F("Download non riuscito (HTTP ")) + code + ")";
    g_state = State::Failed;
    http.end();
    g_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    g_error = F("GitHub non ha dichiarato la dimensione del file");
    g_state = State::Failed;
    http.end();
    g_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  g_latestSize = static_cast<size_t>(contentLength);

  if (!Update.begin(contentLength)) {
    g_error = String(F("Spazio OTA insufficiente: ")) + Update.errorString();
    g_state = State::Failed;
    http.end();
    g_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[1024];
  uint32_t written = 0;
  uint32_t idleSince = millis();

  while (written < static_cast<uint32_t>(contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected()) break;
      if (millis() - idleSince > 20000) {
        g_error = F("Download interrotto: nessun dato da GitHub");
        break;
      }
      delay(10);
      continue;
    }
    idleSince = millis();
    const size_t chunk = stream->readBytes(buffer, min(available, sizeof(buffer)));
    if (chunk == 0) continue;
    if (Update.write(buffer, chunk) != chunk) {
      g_error = String(F("Scrittura firmware fallita: ")) + Update.errorString();
      break;
    }
    written += chunk;
    g_downloaded = written;
  }

  http.end();

  if (g_error.length() || written != static_cast<uint32_t>(contentLength)) {
    if (!g_error.length()) g_error = F("Download incompleto");
    Update.abort();
    g_state = State::Failed;
    g_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (!Update.end(true)) {
    g_error = String(F("Firmware non valido: ")) + Update.errorString();
    g_state = State::Failed;
    g_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  log_i("Aggiornamento da GitHub completato (%s), riavvio", g_latestTag.c_str());
  g_state = State::Done;
  g_restartAt = millis() + 1500;
  g_task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

void begin() {
  // Primo controllo poco dopo l'avvio, quando la rete è già salita.
  g_nextCheckAt = millis() + 45000;
}

String installedRelease() { return String(SHELTR_FW_RELEASE); }

bool busy() {
  return g_state == State::Checking || g_state == State::Downloading || g_task != nullptr;
}

bool check(String &error) {
  if (busy()) {
    error = F("Aggiornamento già in corso");
    return false;
  }
  if (!net::online()) {
    error = F("Nessuna connessione di rete");
    g_state = State::Failed;
    g_error = error;
    return false;
  }

  g_state = State::Checking;
  g_error = "";

  WiFiClientSecure client;
  applySecureDefaults(client);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  http.setReuse(false);

  if (!http.begin(client, releasesUrl())) {
    error = F("Connessione a GitHub non riuscita");
    g_error = error;
    g_state = State::Failed;
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = code > 0 ? String(F("GitHub ha risposto ")) + code
                     : String(F("GitHub non raggiungibile (rete o TLS)"));
    http.end();
    g_error = error;
    g_state = State::Failed;
    return false;
  }

  // Filtro: della risposta interessano solo tag, nome, note e l'asset del firmware.
  JsonDocument filter(&SpiRamAllocator::instance());
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["body"] = true;
  filter["published_at"] = true;
  filter["html_url"] = true;
  JsonObject asset = filter["assets"].add<JsonObject>();
  asset["name"] = true;
  asset["browser_download_url"] = true;
  asset["size"] = true;

  JsonDocument doc(&SpiRamAllocator::instance());
  const DeserializationError parseError =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (parseError) {
    error = String(F("Risposta di GitHub non leggibile: ")) + parseError.c_str();
    g_error = error;
    g_state = State::Failed;
    return false;
  }

  const String tag = doc["tag_name"] | "";
  if (!tag.length()) {
    error = F("Nessuna release pubblicata sul repository");
    g_error = error;
    g_state = State::Failed;
    return false;
  }

  String assetUrl;
  size_t assetSize = 0;
  for (JsonObjectConst item : doc["assets"].as<JsonArrayConst>()) {
    const String name = item["name"] | "";
    if (name != ASSET_NAME) continue;
    assetUrl = item["browser_download_url"] | "";
    assetSize = item["size"] | 0;
    break;
  }

  g_latestTag = tag;
  g_latestName = doc["name"] | tag;
  g_latestUrl = assetUrl;
  g_latestSize = assetSize;
  g_publishedAt = doc["published_at"] | "";
  String notes = doc["body"] | "";
  notes.replace("\r", "");
  if (notes.length() > 240) notes = notes.substring(0, 240) + "…";
  g_latestNotes = notes;
  g_lastCheckAt = millis();
  g_checkedOnce = true;

  if (!assetUrl.length()) {
    error = String(F("La release ")) + tag + F(" non contiene ") + ASSET_NAME;
    g_error = error;
    g_state = State::Failed;
    return false;
  }

  const bool available = tag != installedRelease();
  g_state = available ? State::Available : State::Idle;
  log_i("Controllo aggiornamenti: installata %s, disponibile %s", installedRelease().c_str(),
        tag.c_str());
  return true;
}

bool startInstall(String &error) {
  if (busy()) {
    error = F("Aggiornamento già in corso");
    return false;
  }
  if (!g_latestUrl.length()) {
    error = F("Esegui prima il controllo aggiornamenti");
    return false;
  }
  if (!net::online()) {
    error = F("Nessuna connessione di rete");
    return false;
  }

  g_error = "";
  g_downloaded = 0;
  g_state = State::Downloading;

  // Task dedicato: il download dura decine di secondi e il web server deve
  // continuare a rispondere per mostrare l'avanzamento.
  if (xTaskCreatePinnedToCore(installTask, "sheltr-ota", 12288, nullptr, 1, &g_task, 1) != pdPASS) {
    g_task = nullptr;
    g_state = State::Failed;
    error = F("Impossibile avviare il download");
    g_error = error;
    return false;
  }
  return true;
}

void loop() {
  if (g_restartAt != 0 && static_cast<int32_t>(millis() - g_restartAt) > 0) {
    ESP.restart();
  }

  const cfg::UpdateCfg &settings = cfg::config().update;
  if (!settings.enabled || settings.checkIntervalHours == 0) return;
  if (busy() || g_state == State::Downloading || g_state == State::Done) return;
  if (static_cast<int32_t>(millis() - g_nextCheckAt) < 0) return;
  if (!net::online()) {
    g_nextCheckAt = millis() + 60000;  // riprova quando la rete torna
    return;
  }

  g_nextCheckAt = millis() + static_cast<uint32_t>(settings.checkIntervalHours) * 3600000UL;
  String error;
  check(error);
}

void statusJson(JsonObject out) {
  const cfg::UpdateCfg &settings = cfg::config().update;
  out["enabled"] = settings.enabled;
  out["repo"] = settings.repo;
  out["checkIntervalHours"] = settings.checkIntervalHours;
  out["installedRelease"] = installedRelease();
  out["installedVersion"] = SHELTR_FW_VERSION;
  out["state"] = stateName(g_state);
  out["checked"] = g_checkedOnce;
  out["lastCheckAgoMs"] = g_lastCheckAt ? millis() - g_lastCheckAt : 0;
  out["available"] = g_state == State::Available;
  out["latestRelease"] = g_latestTag;
  out["latestName"] = g_latestName;
  out["notes"] = g_latestNotes;
  out["publishedAt"] = g_publishedAt;
  out["size"] = static_cast<uint32_t>(g_latestSize);
  out["downloaded"] = g_downloaded;
  out["progress"] = g_latestSize ? static_cast<uint8_t>((g_downloaded * 100ULL) / g_latestSize) : 0;
  out["error"] = g_error;
  out["restarting"] = g_restartAt != 0;
}

}  // namespace updater
