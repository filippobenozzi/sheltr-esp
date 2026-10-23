#include "cloud_link.h"

#include <HardwareSerial.h>
#include <WiFi.h>

#include "link_codec.h"
#include "net_manager.h"
#include "settings.h"

namespace cloudlink {

namespace {

// Seriale verso il DR154: UART2 (la 1 e' del bus delle schede, la 0 del log).
HardwareSerial *g_serial = nullptr;
// Connessione diretta al gateway del portale.
WiFiClient g_tcp;

MessageHandler g_handler = nullptr;
linkcodec::Decoder *g_decoder = nullptr;

int8_t g_tx = -1;
int8_t g_rx = -1;
int8_t g_de = -1;
uint32_t g_baud = 0;
bool g_serialOpen = false;
bool g_usingSerial = false;

uint32_t g_session = 0;
uint16_t g_messageId = 0;
uint32_t g_lastRxAt = 0;
uint32_t g_connectedAt = 0;
uint32_t g_nextAttempt = 0;
uint32_t g_backoffMs = 2000;
uint32_t g_sent = 0;
uint32_t g_received = 0;
uint32_t g_writeErrors = 0;
uint32_t g_connectFailures = 0;
String g_lastError;

// Senza risposte per questo tempo il collegamento non e' piu' considerato vivo: il
// portale conferma ogni messaggio e noi mandiamo comunque un ping ogni minuto.
constexpr uint32_t ALIVE_WINDOW_MS = 150000;
// Pausa fra una trama e l'altra sulla seriale: aiuta il modulo a non riempire il buffer.
constexpr uint32_t SERIAL_FRAME_GAP_MS = 5;
constexpr uint32_t CONNECT_TIMEOUT_MS = 2500;
constexpr uint32_t MAX_BACKOFF_MS = 30000;

bool wanted() { return cfg::config().cloud.enabled; }
bool viaSerial() { return cfg::config().cloud.transport == cfg::CloudTransport::Usr; }

void onDecoded(const char *sub, const uint8_t *payload, size_t length, void *) {
  g_received++;
  g_lastRxAt = millis();
  if (g_handler != nullptr) g_handler(String(sub), payload, length);
}

void writeFrame(const char *frame, size_t length, void *) {
  if (g_usingSerial) {
    if (g_serial == nullptr || !g_serialOpen) return;
    if (g_de >= 0) {
      digitalWrite(g_de, HIGH);  // driver RS485 in trasmissione
      delayMicroseconds(50);
    }
    const size_t written = g_serial->write(reinterpret_cast<const uint8_t *>(frame), length);
    g_serial->flush();
    if (g_de >= 0) {
      delayMicroseconds(50);
      digitalWrite(g_de, LOW);  // torna in ascolto
    }
    if (written != length) g_writeErrors++;
    delay(SERIAL_FRAME_GAP_MS);
    return;
  }
  if (!g_tcp.connected()) return;
  if (g_tcp.write(reinterpret_cast<const uint8_t *>(frame), length) != length) {
    g_writeErrors++;
    g_lastError = F("scrittura fallita");
    g_tcp.stop();
  }
}

void closeSerial() {
  if (g_serial != nullptr && g_serialOpen) g_serial->end();
  if (g_de >= 0) pinMode(g_de, INPUT);
  g_serialOpen = false;
}

void openSerial() {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  if (g_serial == nullptr) g_serial = new HardwareSerial(2);
  g_tx = settings.usr.tx;
  g_rx = settings.usr.rx;
  g_de = settings.usr.de;
  g_baud = settings.usr.baud;
  g_serial->end();
  g_serial->begin(g_baud, SERIAL_8N1, g_rx, g_tx);
  g_serial->setRxBufferSize(2048);
  if (g_de >= 0) {
    pinMode(g_de, OUTPUT);
    digitalWrite(g_de, LOW);
  }
  g_decoder->reset();
  g_serialOpen = true;
  g_session++;
  g_connectedAt = millis();
  log_i("Collegamento al portale via DR154: TX=%d RX=%d DE=%d baud=%u", g_tx, g_rx, g_de, g_baud);
}

void closeTcp() {
  if (g_tcp.connected()) g_tcp.stop();
}

void connectTcp() {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  if (!settings.host.length()) {
    g_lastError = F("indirizzo del portale mancante");
    return;
  }
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextAttempt) < 0 || !net::online()) return;
  if (!g_tcp.connect(settings.host.c_str(), settings.port, CONNECT_TIMEOUT_MS)) {
    g_connectFailures++;
    g_lastError = String(F("portale non raggiungibile (")) + settings.host + ":" + settings.port + ")";
    g_nextAttempt = now + g_backoffMs;
    g_backoffMs = min<uint32_t>(g_backoffMs * 2, MAX_BACKOFF_MS);
    log_w("Sheltr Cloud: %s", g_lastError.c_str());
    return;
  }
  g_tcp.setNoDelay(true);
  g_decoder->reset();
  g_backoffMs = 2000;
  g_lastError = "";
  g_lastRxAt = 0;
  g_connectedAt = now;
  g_session++;
  log_i("Collegato al portale %s:%u", settings.host.c_str(), settings.port);
}

}  // namespace

void begin(MessageHandler handler) {
  g_handler = handler;
  if (g_decoder == nullptr) g_decoder = new linkcodec::Decoder(onDecoded, nullptr);
  reload();
}

void reload() {
  closeTcp();
  closeSerial();
  g_usingSerial = viaSerial();
  g_nextAttempt = millis() + 500;
  g_backoffMs = 2000;
  g_lastRxAt = 0;
  g_lastError = "";
}

void loop() {
  if (!wanted()) {
    closeTcp();
    closeSerial();
    return;
  }
  if (g_usingSerial != viaSerial()) reload();

  uint8_t buffer[256];
  if (g_usingSerial) {
    if (!g_serialOpen) {
      openSerial();
      return;
    }
    while (g_serial->available() > 0) {
      const size_t read = g_serial->readBytes(buffer, sizeof(buffer));
      if (read == 0) break;
      g_decoder->feed(buffer, read);
    }
    return;
  }

  if (!g_tcp.connected()) {
    connectTcp();
    return;
  }
  while (g_tcp.available() > 0) {
    const int read = g_tcp.read(buffer, sizeof(buffer));
    if (read <= 0) break;
    g_decoder->feed(buffer, static_cast<size_t>(read));
  }
  // Collegato ma muto da troppo: la connessione e' morta anche se TCP non lo sa.
  const uint32_t now = millis();
  const uint32_t since = g_lastRxAt ? g_lastRxAt : g_connectedAt;
  if (now - since > ALIVE_WINDOW_MS) {
    log_w("Sheltr Cloud: nessuna risposta dal portale, mi ricollego");
    g_lastError = F("nessuna risposta dal portale");
    g_tcp.stop();
    g_nextAttempt = now + 1000;
  }
}

bool send(const String &sub, const uint8_t *payload, size_t length) {
  if (!wanted() || !open()) return false;
  g_messageId = static_cast<uint16_t>((g_messageId % 0xFFFF) + 1);
  if (!linkcodec::encode(sub.c_str(), payload, length, g_messageId, writeFrame, nullptr)) {
    log_w("Incapsulamento '%s' non riuscito (%u byte)", sub.c_str(), static_cast<unsigned>(length));
    return false;
  }
  g_sent++;
  return true;
}

bool send(const String &sub, const String &payload) {
  return send(sub, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
}

bool open() {
  if (!wanted()) return false;
  return g_usingSerial ? g_serialOpen : g_tcp.connected();
}

uint32_t session() { return g_session; }

bool alive() {
  if (!open() || g_lastRxAt == 0) return false;
  return (millis() - g_lastRxAt) < ALIVE_WINDOW_MS;
}

bool enabled() { return wanted(); }

void statusJson(JsonObject out) {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  out["enabled"] = wanted();
  out["transport"] = viaSerial() ? "usr" : "tcp";
  out["open"] = open();
  out["alive"] = alive();
  out["host"] = settings.host;
  out["port"] = settings.port;
  out["sent"] = g_sent;
  out["received"] = g_received;
  out["writeErrors"] = g_writeErrors;
  out["connectFailures"] = g_connectFailures;
  out["lastError"] = g_lastError;
  out["lastRxAgoMs"] = g_lastRxAt ? (millis() - g_lastRxAt) : 0;
  JsonObject usr = out["usr"].to<JsonObject>();
  usr["tx"] = settings.usr.tx;
  usr["rx"] = settings.usr.rx;
  usr["de"] = settings.usr.de;
  usr["baud"] = settings.usr.baud;
  if (g_decoder != nullptr) {
    out["droppedFrames"] = g_decoder->droppedFrames();
    out["crcErrors"] = g_decoder->crcErrors();
  }
}

}  // namespace cloudlink
