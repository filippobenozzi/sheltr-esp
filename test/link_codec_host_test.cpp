// Verifica del codec del collegamento seriale (USR DR154) compilata sull'host.
//
//   g++ -std=c++17 -O2 -I src test/link_codec_host_test.cpp src/link_codec.cpp -o /tmp/link_test && /tmp/link_test
//
// Le trame prodotte qui devono essere identiche a quelle del portale
// (webapp/link_codec.py nella repository sheltr-cloud): il test le stampa in modo
// che possano essere confrontate byte per byte con la gemella Python.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "link_codec.h"

namespace {

int failures = 0;

void check(const char *label, bool condition) {
  std::printf("%s %s\n", condition ? "OK " : "KO ", label);
  if (!condition) failures++;
}

void collectFrame(const char *frame, size_t length, void *context) {
  static_cast<std::vector<std::string> *>(context)->emplace_back(frame, length);
}

struct Received {
  std::vector<std::pair<std::string, std::string>> messages;
};

void collectMessage(const char *sub, const uint8_t *payload, size_t length, void *context) {
  static_cast<Received *>(context)->messages.emplace_back(
      std::string(sub), std::string(reinterpret_cast<const char *>(payload), length));
}

std::vector<std::string> encodeAll(const char *sub, const std::string &payload, uint16_t id) {
  std::vector<std::string> frames;
  linkcodec::encode(sub, reinterpret_cast<const uint8_t *>(payload.data()), payload.length(), id,
               collectFrame, &frames);
  return frames;
}

std::string join(const std::vector<std::string> &frames) {
  std::string out;
  for (const std::string &frame : frames) out += frame;
  return out;
}

std::string randomBytes(size_t length, unsigned seed) {
  std::srand(seed);
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; i++) out += static_cast<char>(std::rand() & 0xFF);
  return out;
}

}  // namespace

int main() {
  // 1. vettore di prova del CRC (CRC-16/CCITT-FALSE)
  check("crc16(\"123456789\") == 0x29B1",
        linkcodec::crc16(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0x29B1);

  // 2. andata e ritorno di un messaggio corto
  {
    const std::string payload = "49 01 40 11 01 00 00 00 00 00 00 00 00 46\r\n";
    const std::vector<std::string> frames = encodeAll("pub", payload, 1);
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::string stream = join(frames);
    decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.length());
    check("messaggio corto, una trama",
          frames.size() == 1 && received.messages.size() == 1 &&
              received.messages[0].first == "pub" && received.messages[0].second == payload);
  }

  // 3. messaggio grande spezzato, flusso tagliato a pezzi irregolari
  {
    const std::string payload = randomBytes(5000, 7);
    const std::string stream = join(encodeAll("config", payload, 7));
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    size_t position = 0;
    size_t step = 1;
    while (position < stream.length()) {
      const size_t size = std::min(step, stream.length() - position);
      decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()) + position, size);
      position += size;
      step = (step * 7 + 13) % 97 + 1;  // pezzi di lunghezza variabile
    }
    check("messaggio da 5000 byte a pezzi irregolari",
          received.messages.size() == 1 && received.messages[0].second == payload);
  }

  // 4. due messaggi uniti in un solo pezzo
  {
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::string stream = join(encodeAll("pub", "AAA", 1)) + join(encodeAll("event", "{\"x\":1}", 2));
    decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.length());
    check("due messaggi uniti", received.messages.size() == 2 &&
                                    received.messages[0].second == "AAA" &&
                                    received.messages[1].first == "event");
  }

  // 5. rumore intorno alla trama
  {
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::string stream = std::string("spazzatura\x01\x02") + join(encodeAll("pub", "BBB", 3)) + "coda";
    decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.length());
    check("rumore intorno alla trama",
          received.messages.size() == 1 && received.messages[0].second == "BBB");
  }

  // 6. trama corrotta: scartata, e il flusso riprende
  {
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    std::string corrupt = join(encodeAll("cmd", "49 01 51 41", 4));
    corrupt[20] = static_cast<char>(corrupt[20] ^ 0x01);
    decoder.feed(reinterpret_cast<const uint8_t *>(corrupt.data()), corrupt.length());
    const bool dropped = received.messages.empty() && decoder.crcErrors() == 1;
    const std::string good = join(encodeAll("pub", "CCC", 5));
    decoder.feed(reinterpret_cast<const uint8_t *>(good.data()), good.length());
    check("trama corrotta scartata e ripresa",
          dropped && received.messages.size() == 1 && received.messages[0].second == "CCC");
  }

  // 7. pezzo mancante: nessuna consegna a meta'
  {
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::vector<std::string> frames = encodeAll("config", randomBytes(1000, 3), 9);
    std::string partial;
    for (size_t i = 0; i < frames.size(); i++) {
      if (i == 1) continue;  // un pezzo si perde
      partial += frames[i];
    }
    decoder.feed(reinterpret_cast<const uint8_t *>(partial.data()), partial.length());
    check("pezzo mancante -> nessuna consegna", received.messages.empty());
  }

  // 8. un messaggio corto in mezzo non disturba il riassemblaggio di uno grande
  {
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::string payload = randomBytes(800, 11);
    const std::vector<std::string> big = encodeAll("settings", payload, 12);
    const std::string small = join(encodeAll("ack", "12", 13));
    std::string stream;
    for (size_t i = 0; i < big.size(); i++) {
      stream += big[i];
      if (i == 1) stream += small;
    }
    decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.length());
    bool bigOk = false;
    bool smallOk = false;
    for (const auto &item : received.messages) {
      if (item.first == "settings" && item.second == payload) bigOk = true;
      if (item.first == "ack" && item.second == "12") smallOk = true;
    }
    check("messaggio corto intercalato", bigOk && smallOk && received.messages.size() == 2);
  }

  // 9. contenuto binario compresi i delimitatori
  {
    std::string raw;
    for (int i = 0; i < 256; i++) raw += static_cast<char>(i);
    raw += "|#!S1|";
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::string stream = join(encodeAll("pub", raw, 14));
    decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.length());
    check("contenuto binario con delimitatori",
          received.messages.size() == 1 && received.messages[0].second == raw);
  }

  // 10. payload vuoto e sotto-topic con barra
  {
    Received received;
    linkcodec::Decoder decoder(collectMessage, &received);
    const std::string stream = join(encodeAll("bridge/status", "online", 15)) + join(encodeAll("ack", "", 16));
    decoder.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.length());
    check("sotto-topic con barra e payload vuoto",
          received.messages.size() == 2 && received.messages[0].first == "bridge/status" &&
              received.messages[1].second.empty());
  }

  // Trame di riferimento per il confronto con il portale (webapp/link_codec.py).
  if (getenv("DUMP_FRAMES")) {
    for (const std::string &frame : encodeAll("pub", "49 01 40 11 01 00 00 00 00 00 00 00 00 46\r\n", 1)) {
      std::fwrite(frame.data(), 1, frame.length(), stdout);
    }
    for (const std::string &frame : encodeAll("bridge/status", "online", 42)) {
      std::fwrite(frame.data(), 1, frame.length(), stdout);
    }
    for (const std::string &frame : encodeAll("config", randomBytes(600, 5), 9)) {
      std::fwrite(frame.data(), 1, frame.length(), stdout);
    }
  }

  std::printf(failures ? "\n%d TEST FALLITI\n" : "\nTUTTI OK\n", failures);
  return failures ? 1 : 0;
}
