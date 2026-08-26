#pragma once

// Incapsulamento dei messaggi sul collegamento seriale verso il modulo USR DR154.
//
// Quando il gateway non ha rete propria, la connettivita' gliela da' un USR DR154:
// il gateway scrive sulla seriale, il modulo pubblica quei byte su un topic MQTT
// (e viceversa). Sulla seriale pero' c'e' un solo canale, mentre il protocollo del
// portale usa piu' topic (cmd, pub, config, settings, action, bridge/status):
// l'incapsulamento porta il nome del sotto-topic insieme al contenuto, con gli
// stessi nomi del collegamento diretto.
//
// Formato di una trama (ASCII stampabile, una riga):
//
//     !S1|<sub>|<msgId>|<idx>|<cnt>|<base64>|<crc16>#\n
//
// Il contenuto viaggia in base64 (nessun byte puo' essere scambiato per un
// delimitatore) ed e' protetto da CRC-16/CCITT-FALSE: una trama corrotta viene
// scartata, mai applicata a meta'. I messaggi grandi (la configurazione supera i
// 5 KB) vengono spezzati perche' il modulo USR ha un buffer seriale limitato.
//
// Questo file non dipende da Arduino apposta: la stessa implementazione si compila
// sull'host per essere verificata contro la gemella Python del portale
// (webapp/link_codec.py). Vedi test/link_codec_host_test.cpp.

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace linkcodec {

// Stessi valori del portale (webapp/link_codec.py): vanno cambiati insieme.
constexpr const char *FRAME_PREFIX = "!S1|";
constexpr char FRAME_SUFFIX = '#';
constexpr size_t CHUNK_BYTES = 192;
constexpr size_t MAX_CHUNKS = 128;
constexpr size_t MAX_MESSAGE_BYTES = CHUNK_BYTES * MAX_CHUNKS;
constexpr size_t MAX_FRAME_CHARS = 512;

uint16_t crc16(const uint8_t *data, size_t length);

// Trasforma un messaggio nelle trame da scrivere sulla seriale: `sink` viene
// chiamata una volta per trama (gia' completa di terminatore di riga).
using FrameSink = void (*)(const char *frame, size_t length, void *context);
bool encode(const char *sub, const uint8_t *payload, size_t length, uint16_t messageId,
            FrameSink sink, void *context);

// Rimette insieme i messaggi da un flusso di byte che arriva a pezzi: il modulo USR
// puo' spezzare una trama o unirne due, e il decoder si riallinea da solo cercando
// il marcatore di inizio.
class Decoder {
 public:
  using MessageSink = void (*)(const char *sub, const uint8_t *payload, size_t length,
                               void *context);

  Decoder(MessageSink sink, void *context) : sink_(sink), context_(context) {}

  void feed(const uint8_t *data, size_t length);
  void reset();

  uint32_t droppedFrames() const { return droppedFrames_; }
  uint32_t crcErrors() const { return crcErrors_; }

 private:
  bool accept(const std::string &frame);

  MessageSink sink_ = nullptr;
  void *context_ = nullptr;
  std::string buffer_;
  // Un solo messaggio spezzato per volta: il portale manda le trame di un messaggio
  // una dopo l'altra. I messaggi che stanno in una sola trama vengono consegnati
  // subito, quindi non disturbano un riassemblaggio in corso.
  std::string pendingSub_;
  std::string pending_;
  uint16_t pendingId_ = 0;
  size_t pendingTotal_ = 0;
  size_t pendingNext_ = 0;
  uint32_t droppedFrames_ = 0;
  uint32_t crcErrors_ = 0;
};

}  // namespace linkcodec
