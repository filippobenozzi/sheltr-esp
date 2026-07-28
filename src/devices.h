#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <map>
#include <vector>

#include "protocol.h"
#include "settings.h"

// Modello entità + motore comandi: la stessa logica del gateway Sheltr
// (comando sul bus, verifica con polling 0x40, stato aggiornato in memoria).
namespace devices {

struct LightState {
  int8_t isOn = -1;  // -1 sconosciuto
  uint32_t updatedAt = 0;
};

struct DimmerState {
  uint8_t level = 0;
  int8_t isOn = -1;
  uint8_t lastOnLevel = protocol::DIMMER_MAX_LEVEL;
  uint32_t updatedAt = 0;
};

struct ShutterState {
  String action = "unknown";
  uint32_t updatedAt = 0;
};

struct ThermostatState {
  float setpoint = 21.0f;
  String mode = "winter";
  int8_t isOn = -1;
  int8_t isActive = -1;
  bool hasTemperature = false;
  float temperature = 0.0f;
  uint32_t updatedAt = 0;
};

struct BoardState {
  bool online = false;
  bool hasPoll = false;
  protocol::Poll poll;
  uint32_t updatedAt = 0;
  String lastError;
};

struct Entity {
  String id;
  String kind;
  String boardId;
  String boardName;
  String name;
  String room;
  uint8_t address = 0;
  uint8_t channel = 0;
  bool favorite = false;
};

struct CommandResult {
  bool ok = false;
  String error;
  String requestHex;
  String responseHex;
  bool pollVerified = false;
};

void begin();
void loop();

// Polling
bool pollAddress(uint8_t address, String &error);
void pollAll();
uint32_t lastPollAt();

// Comandi (entityId = "<boardId>-c<channel>")
CommandResult commandLight(const String &entityId, const String &action);
CommandResult commandDimmer(const String &entityId, const String &action, int level);
CommandResult commandShutter(const String &entityId, const String &action);
CommandResult commandThermostat(const String &entityId, bool hasSetpoint, float setpoint,
                                const String &mode, int8_t power);

// Inoltra un frame grezzo sul bus (bridge Sheltr Cloud / API avanzata).
CommandResult sendRawFrame(const uint8_t *frame);

// Enumerazione entità
std::vector<Entity> entities(const String &kind);
std::vector<Entity> allEntities();
bool entityById(const String &entityId, Entity &out);

// Stato
const LightState *lightState(const String &entityId);
const DimmerState *dimmerState(const String &entityId);
const ShutterState *shutterState(const String &entityId);
const ThermostatState *thermostatState(const String &entityId);
const BoardState *boardState(uint8_t address);
bool boardOnline(uint8_t address);
bool anyBoardOnline();

// Revisione dello stato: incrementata a ogni cambiamento (usata da MQTT).
uint32_t stateRevision();

// Documento di stato completo, nello stesso formato del portale Sheltr Cloud.
void statusJson(JsonObject out, bool refresh, const std::vector<uint8_t> *refreshAddresses);

String isoTimestamp();
bool timeSynced();

}  // namespace devices
