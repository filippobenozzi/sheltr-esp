#include "usr_link.h"

#include <HardwareSerial.h>

#include "link_codec.h"
#include "settings.h"

namespace usr {

namespace {

// UART2: la 1 e' del bus delle schede, la 0 resta al log di sistema.
HardwareSerial *g_serial = nullptr;
MessageHandler g_handler = nullptr;
linkcodec::Decoder *g_decoder = nullptr;

int8_t g_tx = -1;
int8_t g_rx = -1;
int8_t g_de = -1;
uint32_t g_baud = 0;
bool g_open = false;

uint16_t g_messageId = 0;
uint32_t g_lastRxAt = 0;
uint32_t g_sent = 0;
uint32_t g_received = 0;
uint32_t g_writeErrors = 0;

// Senza risposte per questo tempo il collegamento non e' piu' considerato vivo:
// il portale conferma ogni messaggio, e il gateway manda comunque l'heartbeat.
constexpr uint32_t ALIVE_WINDOW_MS = 150000;
// Pausa fra una trama e l'altra: aiuta il modulo a mantenere i confini dei
// pacchetti e a non riempire il proprio buffer seriale.
constexpr uint32_t FRAME_GAP_MS = 5;

void onDecoded(const char *sub, const uint8_t *payload, size_t length, void *) {
  g_received++;
  g_lastRxAt = millis();
  if (g_handler != nullptr) g_handler(String(sub), payload, length);
}

void writeFrame(const char *frame, size_t length, void *) {
  if (g_serial == nullptr || !g_open) return;
  if (g_de >= 0) {
    digitalWrite(g_de, HIGH);  // abilita il driver RS485 in trasmissione
    delayMicroseconds(50);
  }
  const size_t written = g_serial->write(reinterpret_cast<const uint8_t *>(frame), length);
  g_serial->flush();
  if (g_de >= 0) {
    delayMicroseconds(50);
    digitalWrite(g_de, LOW);  // torna in ascolto
  }
  if (written != length) g_writeErrors++;
  delay(FRAME_GAP_MS);
}

void closePort() {
  if (g_serial != nullptr && g_open) {
    g_serial->end();
  }
  if (g_de >= 0) pinMode(g_de, INPUT);
  g_open = false;
}

void openPort() {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  if (g_serial == nullptr) g_serial = new HardwareSerial(2);
  if (g_decoder == nullptr) g_decoder = new linkcodec::Decoder(onDecoded, nullptr);

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
  g_open = true;
  log_i("Collegamento USR DR154: TX=%d RX=%d DE=%d baud=%u", g_tx, g_rx, g_de, g_baud);
}

bool wanted() {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  return settings.enabled && settings.transport == cfg::CloudTransport::Usr;
}

}  // namespace

void begin(MessageHandler handler) {
  g_handler = handler;
  reload();
}

void reload() {
  if (!wanted()) {
    closePort();
    return;
  }
  const cfg::CloudCfg &settings = cfg::config().cloud;
  const bool sameWiring = g_open && g_tx == settings.usr.tx && g_rx == settings.usr.rx &&
                          g_de == settings.usr.de && g_baud == settings.usr.baud;
  if (sameWiring) return;
  closePort();
  openPort();
}

void loop() {
  if (!wanted()) {
    if (g_open) closePort();
    return;
  }
  if (!g_open) {
    openPort();
    return;
  }
  uint8_t buffer[256];
  while (g_serial->available() > 0) {
    const size_t read = g_serial->readBytes(buffer, sizeof(buffer));
    if (read == 0) break;
    g_decoder->feed(buffer, read);
  }
}

bool send(const String &sub, const uint8_t *payload, size_t length) {
  if (!wanted()) return false;
  if (!g_open) openPort();
  if (!g_open) return false;
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

bool alive() {
  if (!wanted() || !g_open || g_lastRxAt == 0) return false;
  return (millis() - g_lastRxAt) < ALIVE_WINDOW_MS;
}

bool enabled() { return wanted(); }

void statusJson(JsonObject out) {
  const cfg::CloudCfg &settings = cfg::config().cloud;
  out["enabled"] = wanted();
  out["open"] = g_open;
  out["alive"] = alive();
  out["tx"] = settings.usr.tx;
  out["rx"] = settings.usr.rx;
  out["de"] = settings.usr.de;
  out["baud"] = settings.usr.baud;
  out["sent"] = g_sent;
  out["received"] = g_received;
  out["writeErrors"] = g_writeErrors;
  out["lastRxAgoMs"] = g_lastRxAt ? (millis() - g_lastRxAt) : 0;
  if (g_decoder != nullptr) {
    out["droppedFrames"] = g_decoder->droppedFrames();
    out["crcErrors"] = g_decoder->crcErrors();
  }
}

}  // namespace usr
