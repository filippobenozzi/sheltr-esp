#include "link_codec.h"

#include <string.h>

namespace linkcodec {

namespace {

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64Encode(const uint8_t *data, size_t length, std::string &out) {
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t byte0 = data[i];
    const uint32_t byte1 = (i + 1 < length) ? data[i + 1] : 0;
    const uint32_t byte2 = (i + 2 < length) ? data[i + 2] : 0;
    const uint32_t triple = (byte0 << 16) | (byte1 << 8) | byte2;
    out += B64[(triple >> 18) & 0x3F];
    out += B64[(triple >> 12) & 0x3F];
    out += (i + 1 < length) ? B64[(triple >> 6) & 0x3F] : '=';
    out += (i + 2 < length) ? B64[triple & 0x3F] : '=';
  }
}

int base64Value(char symbol) {
  if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
  if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
  if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
  if (symbol == '+') return 62;
  if (symbol == '/') return 63;
  return -1;
}

bool base64Decode(const std::string &text, std::string &out) {
  if (text.length() % 4 != 0) return false;
  for (size_t i = 0; i < text.length(); i += 4) {
    int values[4];
    size_t valid = 4;
    for (size_t j = 0; j < 4; j++) {
      const char symbol = text[i + j];
      if (symbol == '=') {
        // Il riempimento sta solo in fondo all'ultimo gruppo.
        if (i + 4 != text.length() || j < 2) return false;
        values[j] = 0;
        if (valid == 4) valid = j;
        continue;
      }
      if (valid != 4) return false;  // dati dopo il riempimento
      values[j] = base64Value(symbol);
      if (values[j] < 0) return false;
    }
    const uint32_t triple = (static_cast<uint32_t>(values[0]) << 18) |
                            (static_cast<uint32_t>(values[1]) << 12) |
                            (static_cast<uint32_t>(values[2]) << 6) |
                            static_cast<uint32_t>(values[3]);
    out += static_cast<char>((triple >> 16) & 0xFF);
    if (valid > 2 || valid == 4) out += static_cast<char>((triple >> 8) & 0xFF);
    if (valid == 4) out += static_cast<char>(triple & 0xFF);
  }
  return true;
}

void appendUint(std::string &out, uint32_t value) {
  char buffer[12];
  int length = 0;
  if (value == 0) buffer[length++] = '0';
  while (value > 0) {
    buffer[length++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  while (length > 0) out += buffer[--length];
}

bool parseUint(const std::string &text, uint32_t &out) {
  if (text.empty() || text.length() > 10) return false;
  uint32_t value = 0;
  for (char symbol : text) {
    if (symbol < '0' || symbol > '9') return false;
    value = value * 10 + static_cast<uint32_t>(symbol - '0');
  }
  out = value;
  return true;
}

bool parseHex16(const std::string &text, uint16_t &out) {
  if (text.empty() || text.length() > 4) return false;
  uint16_t value = 0;
  for (char symbol : text) {
    int digit;
    if (symbol >= '0' && symbol <= '9') digit = symbol - '0';
    else if (symbol >= 'A' && symbol <= 'F') digit = symbol - 'A' + 10;
    else if (symbol >= 'a' && symbol <= 'f') digit = symbol - 'a' + 10;
    else return false;
    value = static_cast<uint16_t>((value << 4) | static_cast<uint16_t>(digit));
  }
  out = value;
  return true;
}

}  // namespace

uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

bool encode(const char *sub, const uint8_t *payload, size_t length, uint16_t messageId,
            FrameSink sink, void *context) {
  if (sub == nullptr || sink == nullptr) return false;
  const std::string subject(sub);
  if (subject.empty() || subject.find('|') != std::string::npos ||
      subject.find(FRAME_SUFFIX) != std::string::npos) {
    return false;
  }
  if (length > MAX_MESSAGE_BYTES) return false;
  if (messageId == 0) messageId = 1;

  const size_t total = length ? (length + CHUNK_BYTES - 1) / CHUNK_BYTES : 1;
  for (size_t index = 0; index < total; index++) {
    const size_t offset = index * CHUNK_BYTES;
    const size_t size = (length > offset) ? ((length - offset < CHUNK_BYTES) ? length - offset : CHUNK_BYTES) : 0;

    std::string body;
    body.reserve(subject.length() + 24 + ((size + 2) / 3) * 4);
    body += subject;
    body += '|';
    appendUint(body, messageId);
    body += '|';
    appendUint(body, static_cast<uint32_t>(index));
    body += '|';
    appendUint(body, static_cast<uint32_t>(total));
    body += '|';
    base64Encode(payload + offset, size, body);

    const uint16_t crc = crc16(reinterpret_cast<const uint8_t *>(body.data()), body.length());
    std::string frame;
    frame.reserve(body.length() + 12);
    frame += FRAME_PREFIX;
    frame += body;
    frame += '|';
    static const char HEX[] = "0123456789ABCDEF";
    frame += HEX[(crc >> 12) & 0x0F];
    frame += HEX[(crc >> 8) & 0x0F];
    frame += HEX[(crc >> 4) & 0x0F];
    frame += HEX[crc & 0x0F];
    frame += FRAME_SUFFIX;
    frame += '\n';
    sink(frame.data(), frame.length(), context);
  }
  return true;
}

void Decoder::reset() {
  buffer_.clear();
  pending_.clear();
  pendingSub_.clear();
  pendingId_ = 0;
  pendingTotal_ = 0;
  pendingNext_ = 0;
}

void Decoder::feed(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0) return;
  buffer_.append(reinterpret_cast<const char *>(data), length);

  const size_t prefixLength = strlen(FRAME_PREFIX);
  while (true) {
    const size_t start = buffer_.find(FRAME_PREFIX);
    if (start == std::string::npos) {
      // Nessuna trama iniziata: si tiene solo la coda che potrebbe contenere un
      // marcatore spezzato a meta'.
      if (buffer_.length() > prefixLength - 1) {
        buffer_.erase(0, buffer_.length() - (prefixLength - 1));
      }
      break;
    }
    if (start > 0) buffer_.erase(0, start);

    const size_t end = buffer_.find(FRAME_SUFFIX, prefixLength);
    if (end == std::string::npos) {
      if (buffer_.length() > MAX_FRAME_CHARS) {
        // Trama iniziata e mai chiusa: si riparte dal marcatore successivo.
        droppedFrames_++;
        buffer_.erase(0, prefixLength);
        continue;
      }
      break;
    }

    const std::string frame = buffer_.substr(prefixLength, end - prefixLength);
    buffer_.erase(0, end + 1);
    accept(frame);
  }
}

bool Decoder::accept(const std::string &frame) {
  const size_t crcSep = frame.rfind('|');
  if (crcSep == std::string::npos) {
    droppedFrames_++;
    return false;
  }
  const std::string body = frame.substr(0, crcSep);
  uint16_t expected = 0;
  if (!parseHex16(frame.substr(crcSep + 1), expected) ||
      crc16(reinterpret_cast<const uint8_t *>(body.data()), body.length()) != expected) {
    crcErrors_++;
    return false;
  }

  // sub | msgId | idx | cnt | base64
  size_t cursor = 0;
  std::string fields[4];
  for (size_t i = 0; i < 4; i++) {
    const size_t sep = body.find('|', cursor);
    if (sep == std::string::npos) {
      droppedFrames_++;
      return false;
    }
    fields[i] = body.substr(cursor, sep - cursor);
    cursor = sep + 1;
  }
  const std::string &subject = fields[0];
  uint32_t messageId = 0;
  uint32_t index = 0;
  uint32_t total = 0;
  if (subject.empty() || !parseUint(fields[1], messageId) || !parseUint(fields[2], index) ||
      !parseUint(fields[3], total) || total < 1 || total > MAX_CHUNKS || index >= total) {
    droppedFrames_++;
    return false;
  }

  std::string chunk;
  if (!base64Decode(body.substr(cursor), chunk)) {
    droppedFrames_++;
    return false;
  }

  if (total == 1) {
    // Messaggio in una sola trama: consegnato subito, cosi' un `ack` che arriva in
    // mezzo non disturba il riassemblaggio di un messaggio grande.
    if (sink_) sink_(subject.c_str(), reinterpret_cast<const uint8_t *>(chunk.data()), chunk.length(), context_);
    return true;
  }

  const bool sameMessage = pendingTotal_ == total && pendingId_ == static_cast<uint16_t>(messageId) &&
                           pendingSub_ == subject;
  if (!sameMessage || index == 0) {
    if (pendingTotal_ && !sameMessage) droppedFrames_++;  // riassemblaggio abbandonato
    pendingSub_ = subject;
    pendingId_ = static_cast<uint16_t>(messageId);
    pendingTotal_ = total;
    pendingNext_ = 0;
    pending_.clear();
  }
  if (index != pendingNext_) {
    // Pezzo fuori sequenza (perso o duplicato): il messaggio non e' recuperabile.
    droppedFrames_++;
    pendingTotal_ = 0;
    pending_.clear();
    return false;
  }
  pending_ += chunk;
  pendingNext_++;
  if (pendingNext_ < pendingTotal_) return true;

  if (sink_) {
    sink_(pendingSub_.c_str(), reinterpret_cast<const uint8_t *>(pending_.data()), pending_.length(), context_);
  }
  pendingTotal_ = 0;
  pending_.clear();
  pendingSub_.clear();
  return true;
}

}  // namespace linkcodec
