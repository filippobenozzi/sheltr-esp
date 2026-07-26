#pragma once

#include <Arduino.h>

// Protocollo Sheltr 1.6 - frame fisso di 14 byte:
//
//   | 0x49 | ADDR | CMD | G1..G10 | 0x46 |
//     start  addr   cmd   10 byte   end
namespace protocol {

constexpr uint8_t FRAME_START = 0x49;
constexpr uint8_t FRAME_END = 0x46;
constexpr size_t FRAME_LEN = 14;
constexpr size_t G_LEN = 10;

// Comandi
constexpr uint8_t CMD_POLL = 0x40;
constexpr uint8_t CMD_SHUTTER = 0x5C;
constexpr uint8_t CMD_DIMMER = 0x5B;
constexpr uint8_t CMD_THERMOSTAT_SETPOINT = 0x5A;
constexpr uint8_t CMD_THERMOSTAT_MODE = 0x6B;

// Relè luci: canale 1..8 -> comando
constexpr uint8_t RELAY_COMMANDS[8] = {0x51, 0x52, 0x53, 0x54, 0x65, 0x66, 0x67, 0x68};

// Azioni luce
constexpr uint8_t LIGHT_ON = 0x41;
constexpr uint8_t LIGHT_OFF = 0x53;
constexpr uint8_t LIGHT_PULSE = 0x50;
constexpr uint8_t LIGHT_TOGGLE = 0x55;

// Azioni tapparella
constexpr uint8_t SHUTTER_UP = 0x55;
constexpr uint8_t SHUTTER_DOWN = 0x44;
constexpr uint8_t SHUTTER_STOP = 0x53;

// Dimmer
constexpr uint8_t DIMMER_SET_KEY = 0x53;
constexpr uint8_t DIMMER_MIN_LEVEL = 0;
constexpr uint8_t DIMMER_MAX_LEVEL = 9;

// Termostato
constexpr uint8_t THERMOSTAT_MODE_WINTER = 0x00;
constexpr uint8_t THERMOSTAT_MODE_SUMMER = 0x01;

struct Frame {
  uint8_t address = 0;
  uint8_t command = 0;
  uint8_t g[G_LEN] = {0};
  uint8_t raw[FRAME_LEN] = {0};
};

struct Poll {
  uint8_t boardType = 0;
  uint8_t release = 0;
  uint8_t outputMask = 0;
  uint8_t inputMask = 0;
  uint8_t dimmerLevel = 0;
  float temperature = 0.0f;
  float powerKw = 0.0f;
  uint8_t setpoint = 0;
};

// Costruisce il frame nel buffer `out` (almeno FRAME_LEN byte).
void build(uint8_t address, uint8_t command, const uint8_t *g, size_t gLen, uint8_t *out);

// Interpreta 14 byte grezzi. Ritorna false se start/end non coincidono.
bool parse(const uint8_t *raw, Frame &frame);

// Cerca un frame valido dentro un buffer binario.
bool extractBinary(const uint8_t *data, size_t len, uint8_t *out);

// Cerca un frame valido dentro una stringa esadecimale ("49 01 51 ..." o compatta).
bool extractHex(const char *text, size_t len, uint8_t *out);

// Accetta sia payload binari sia esadecimali (come fa sheltr-cloud).
bool extractAny(const uint8_t *data, size_t len, uint8_t *out);

String toHex(const uint8_t *raw, bool compact = false);

// Applica il formato payload usato da Sheltr Cloud
// (frame_hex_space, frame_hex_compact, frame_hex_space_crlf, frame_hex_compact_crlf).
String formatPayload(const uint8_t *raw, const String &payloadFormat);

bool decodePoll(const Frame &frame, Poll &poll);

// Separa un setpoint in parte intera e decimale (21.5 -> 21 / 5).
void splitTemperature(float value, uint8_t &intPart, uint8_t &decPart);

}  // namespace protocol
