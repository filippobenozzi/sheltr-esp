#include "protocol.h"

namespace protocol {

namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

void build(uint8_t address, uint8_t command, const uint8_t *g, size_t gLen, uint8_t *out) {
  memset(out, 0, FRAME_LEN);
  out[0] = FRAME_START;
  out[1] = address;
  out[2] = command;
  for (size_t i = 0; i < G_LEN; i++) {
    out[3 + i] = (g != nullptr && i < gLen) ? g[i] : 0;
  }
  out[FRAME_LEN - 1] = FRAME_END;
}

bool parse(const uint8_t *raw, Frame &frame) {
  if (raw[0] != FRAME_START || raw[FRAME_LEN - 1] != FRAME_END) return false;
  frame.address = raw[1];
  frame.command = raw[2];
  for (size_t i = 0; i < G_LEN; i++) frame.g[i] = raw[3 + i];
  memcpy(frame.raw, raw, FRAME_LEN);
  return true;
}

bool extractBinary(const uint8_t *data, size_t len, uint8_t *out) {
  if (len < FRAME_LEN) return false;
  for (size_t i = 0; i + FRAME_LEN <= len; i++) {
    if (data[i] != FRAME_START) continue;
    if (data[i + FRAME_LEN - 1] != FRAME_END) continue;
    memcpy(out, data + i, FRAME_LEN);
    return true;
  }
  return false;
}

bool extractHex(const char *text, size_t len, uint8_t *out) {
  uint8_t values[64];
  size_t count = 0;
  size_t i = 0;
  while (i + 1 < len && count < sizeof(values)) {
    int high = hexValue(text[i]);
    if (high < 0) {
      i++;
      continue;
    }
    int low = hexValue(text[i + 1]);
    if (low < 0) {
      i++;
      continue;
    }
    values[count++] = static_cast<uint8_t>((high << 4) | low);
    i += 2;
  }
  if (count < FRAME_LEN) return false;
  for (size_t idx = 0; idx + FRAME_LEN <= count; idx++) {
    if (values[idx] == FRAME_START && values[idx + FRAME_LEN - 1] == FRAME_END) {
      memcpy(out, values + idx, FRAME_LEN);
      return true;
    }
  }
  return false;
}

bool extractAny(const uint8_t *data, size_t len, uint8_t *out) {
  if (extractBinary(data, len, out)) return true;
  return extractHex(reinterpret_cast<const char *>(data), len, out);
}

String toHex(const uint8_t *raw, bool compact) {
  String result;
  result.reserve(compact ? FRAME_LEN * 2 : FRAME_LEN * 3);
  char buffer[4];
  for (size_t i = 0; i < FRAME_LEN; i++) {
    snprintf(buffer, sizeof(buffer), "%02X", raw[i]);
    if (!compact && i > 0) result += ' ';
    result += buffer;
  }
  return result;
}

String formatPayload(const uint8_t *raw, const String &payloadFormat) {
  if (payloadFormat == "frame_hex_compact") return toHex(raw, true);
  if (payloadFormat == "frame_hex_compact_crlf") return toHex(raw, true) + "\r\n";
  if (payloadFormat == "frame_hex_space") return toHex(raw, false);
  // Default: frame_hex_space_crlf (formato usato dal profilo Sheltr Mini).
  return toHex(raw, false) + "\r\n";
}

bool decodePoll(const Frame &frame, Poll &poll) {
  if (frame.command != CMD_POLL) return false;
  const uint8_t typeAndRelease = frame.g[0];
  poll.boardType = typeAndRelease & 0x0F;
  poll.release = (typeAndRelease >> 4) & 0x0F;
  poll.outputMask = frame.g[1];
  poll.inputMask = frame.g[2];
  poll.dimmerLevel = constrain(frame.g[3], DIMMER_MIN_LEVEL, DIMMER_MAX_LEVEL);
  const float sign = (frame.g[6] == 0x2D) ? -1.0f : 1.0f;
  poll.temperature = sign * (frame.g[4] + frame.g[5] / 10.0f);
  poll.powerKw = frame.g[7] / 10.0f;
  poll.setpoint = frame.g[8];
  return true;
}

void splitTemperature(float value, uint8_t &intPart, uint8_t &decPart) {
  float rounded = roundf(fabsf(value) * 10.0f) / 10.0f;
  int whole = static_cast<int>(rounded);
  int decimal = static_cast<int>(roundf((rounded - whole) * 10.0f));
  if (decimal >= 10) {
    whole += 1;
    decimal = 0;
  }
  intPart = static_cast<uint8_t>(constrain(whole, 0, 99));
  decPart = static_cast<uint8_t>(constrain(decimal, 0, 9));
}

}  // namespace protocol
