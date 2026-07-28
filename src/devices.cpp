#include "devices.h"

#include <time.h>

#include "bus.h"
#include "json_utils.h"

namespace devices {

namespace {

std::map<String, LightState> g_lights;
std::map<String, DimmerState> g_dimmers;
std::map<String, ShutterState> g_shutters;
std::map<String, ThermostatState> g_thermostats;
std::map<uint8_t, BoardState> g_boards;
uint32_t g_revision = 1;
uint32_t g_lastPollAt = 0;
uint32_t g_nextPollAt = 0;

void touch() { g_revision++; }

bool isThermostatActive(uint8_t channel, const protocol::Poll &poll) {
  const uint8_t bit = 1 << (constrain(channel, 1, 8) - 1);
  return (poll.outputMask & bit) != 0;
}

Entity makeEntity(const cfg::Board &board, const cfg::Channel &channel) {
  Entity entity;
  entity.id = cfg::entityId(board.id, channel.channel);
  entity.kind = board.kind;
  entity.boardId = board.id;
  entity.boardName = board.name;
  entity.name = channel.name;
  entity.room = channel.room;
  entity.address = board.address;
  entity.channel = channel.channel;
  entity.favorite = channel.favorite;
  return entity;
}

void applyPollToEntities(uint8_t address, const protocol::Poll &poll) {
  const uint32_t now = millis();
  for (const cfg::Board &board : cfg::config().boards) {
    if (board.address != address) continue;
    for (const cfg::Channel &channel : board.channels) {
      const String id = cfg::entityId(board.id, channel.channel);
      if (board.kind == "light") {
        const uint8_t bit = 1 << (constrain(channel.channel, 1, 8) - 1);
        LightState &state = g_lights[id];
        state.isOn = (poll.outputMask & bit) ? 1 : 0;
        state.updatedAt = now;
      } else if (board.kind == "dimmer") {
        DimmerState &state = g_dimmers[id];
        state.level = poll.dimmerLevel;
        state.isOn = poll.dimmerLevel > 0 ? 1 : 0;
        if (poll.dimmerLevel > 0) state.lastOnLevel = poll.dimmerLevel;
        state.updatedAt = now;
      } else if (board.kind == "thermostat") {
        ThermostatState &state = g_thermostats[id];
        state.temperature = poll.temperature;
        state.hasTemperature = true;
        if (poll.setpoint > 0) {
          state.setpoint = static_cast<float>(poll.setpoint);
          state.isOn = 1;
        } else {
          state.isOn = 0;
        }
        state.isActive = isThermostatActive(channel.channel, poll) ? 1 : 0;
        state.updatedAt = now;
      }
    }
  }
  touch();
}

SheltrBus::Result sendWithRetries(uint8_t address, uint8_t command, const uint8_t *g, size_t gLen,
                                  SheltrBus::Validator validator) {
  const uint8_t attempts = cfg::config().bus.retries + 1;
  SheltrBus::Result result;
  for (uint8_t attempt = 0; attempt < attempts; attempt++) {
    result = Bus.send(address, command, g, gLen, true, validator);
    if (result.ok) return result;
    delay(120);
  }
  return result;
}

bool resolveEntity(const String &entityId, const String &expectedKind, Entity &out, String &error) {
  cfg::Board *board = nullptr;
  cfg::Channel *channel = cfg::findChannel(entityId, &board);
  if (channel == nullptr || board == nullptr) {
    error = F("Dispositivo non trovato");
    return false;
  }
  if (expectedKind.length() && board->kind != expectedKind) {
    error = String(F("Il dispositivo non è di tipo ")) + expectedKind;
    return false;
  }
  out = makeEntity(*board, *channel);
  return true;
}

void channelStateJson(const Entity &entity, JsonObject item) {
  item["id"] = entity.id;
  item["boardId"] = entity.boardId;
  item["boardName"] = entity.boardName;
  item["address"] = entity.address;
  item["channel"] = entity.channel;
  item["kind"] = entity.kind;
  item["name"] = entity.name;
  item["room"] = entity.room;
  item["favorite"] = entity.favorite;
  item["online"] = boardOnline(entity.address);

  if (entity.kind == "light") {
    const LightState *state = lightState(entity.id);
    if (state != nullptr && state->isOn >= 0) {
      item["isOn"] = state->isOn == 1;
    } else {
      item["isOn"] = nullptr;
    }
  } else if (entity.kind == "dimmer") {
    const DimmerState *state = dimmerState(entity.id);
    const uint8_t level = state != nullptr ? state->level : 0;
    item["level"] = level;
    item["isOn"] = state != nullptr && state->isOn >= 0 ? state->isOn == 1 : level > 0;
  } else if (entity.kind == "shutter") {
    const ShutterState *state = shutterState(entity.id);
    item["action"] = state != nullptr ? state->action : String("unknown");
  } else {
    const ThermostatState *state = thermostatState(entity.id);
    if (state != nullptr) {
      if (state->hasTemperature) {
        item["temperature"] = roundf(state->temperature * 10.0f) / 10.0f;
      } else {
        item["temperature"] = nullptr;
      }
      item["setpoint"] = state->setpoint;
      item["mode"] = state->mode;
      item["isOn"] = state->isOn >= 0 ? state->isOn == 1 : true;
      item["isActive"] = state->isActive >= 0 ? state->isActive == 1 : false;
    } else {
      item["temperature"] = nullptr;
      item["setpoint"] = 21.0f;
      item["mode"] = "winter";
      item["isOn"] = true;
      item["isActive"] = false;
    }
  }
}

}  // namespace

void begin() {
  const cfg::BusCfg &bus = cfg::config().bus;
  Bus.begin(bus.rx, bus.tx, bus.de, bus.baud, bus.timeoutMs);
  g_nextPollAt = millis() + 4000;
}

uint32_t stateRevision() { return g_revision; }
uint32_t lastPollAt() { return g_lastPollAt; }

bool timeSynced() {
  time_t now = time(nullptr);
  return now > 1700000000;  // 2023-11-14, sufficiente per capire se NTP ha risposto
}

String isoTimestamp() {
  if (!timeSynced()) return String();
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buffer);
}

bool pollAddress(uint8_t address, String &error) {
  const uint8_t attempts = cfg::config().bus.retries + 1;
  SheltrBus::Result result;
  for (uint8_t attempt = 0; attempt < attempts; attempt++) {
    result = Bus.poll(address);
    if (result.ok && result.hasFrame) break;
    delay(120);
  }

  BoardState &state = g_boards[address];
  state.updatedAt = millis();
  if (!result.ok || !result.hasFrame) {
    state.online = false;
    state.lastError = result.error;
    error = result.error;
    touch();
    return false;
  }

  protocol::Poll poll;
  if (!protocol::decodePoll(result.frame, poll)) {
    state.online = false;
    state.lastError = F("Frame di polling non decodificabile");
    error = state.lastError;
    touch();
    return false;
  }

  state.online = true;
  state.hasPoll = true;
  state.poll = poll;
  state.lastError = "";
  applyPollToEntities(address, poll);
  return true;
}

void pollAll() {
  String error;
  for (uint8_t address : cfg::allAddresses()) {
    pollAddress(address, error);
    delay(30);
  }
  g_lastPollAt = millis();
}

void loop() {
  const uint16_t interval = cfg::config().bus.pollIntervalSec;
  if (interval == 0) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextPollAt) < 0) return;
  g_nextPollAt = now + static_cast<uint32_t>(interval) * 1000UL;
  pollAll();
}

std::vector<Entity> entities(const String &kind) {
  std::vector<Entity> out;
  for (const cfg::Board &board : cfg::config().boards) {
    if (kind.length() && board.kind != kind) continue;
    for (const cfg::Channel &channel : board.channels) out.push_back(makeEntity(board, channel));
  }
  return out;
}

std::vector<Entity> allEntities() { return entities(""); }

bool entityById(const String &id, Entity &out) {
  String error;
  return resolveEntity(id, "", out, error);
}

const LightState *lightState(const String &id) {
  auto found = g_lights.find(id);
  return found == g_lights.end() ? nullptr : &found->second;
}

const DimmerState *dimmerState(const String &id) {
  auto found = g_dimmers.find(id);
  return found == g_dimmers.end() ? nullptr : &found->second;
}

const ShutterState *shutterState(const String &id) {
  auto found = g_shutters.find(id);
  return found == g_shutters.end() ? nullptr : &found->second;
}

const ThermostatState *thermostatState(const String &id) {
  auto found = g_thermostats.find(id);
  return found == g_thermostats.end() ? nullptr : &found->second;
}

const BoardState *boardState(uint8_t address) {
  auto found = g_boards.find(address);
  return found == g_boards.end() ? nullptr : &found->second;
}

bool boardOnline(uint8_t address) {
  const BoardState *state = boardState(address);
  return state != nullptr && state->online;
}

bool anyBoardOnline() {
  for (const auto &pair : g_boards) {
    if (pair.second.online) return true;
  }
  return false;
}

CommandResult commandLight(const String &entityId, const String &action) {
  CommandResult result;
  Entity entity;
  if (!resolveEntity(entityId, "light", entity, result.error)) return result;

  String normalized = action;
  normalized.toLowerCase();
  uint8_t code = 0;
  if (normalized == "on") {
    code = protocol::LIGHT_ON;
  } else if (normalized == "off") {
    code = protocol::LIGHT_OFF;
  } else if (normalized == "toggle") {
    const LightState *state = lightState(entityId);
    const bool isOn = state != nullptr && state->isOn == 1;
    code = isOn ? protocol::LIGHT_OFF : protocol::LIGHT_ON;
    normalized = isOn ? "off" : "on";
  } else {
    result.error = F("Azione luce non valida (on, off, toggle)");
    return result;
  }

  if (entity.channel < 1 || entity.channel > 8) {
    result.error = F("Canale luce non valido (1..8)");
    return result;
  }
  const uint8_t command = protocol::RELAY_COMMANDS[entity.channel - 1];
  const uint8_t payload[1] = {code};

  SheltrBus::Result busResult = sendWithRetries(
      entity.address, command, payload, 1, [command, code, &entity](const protocol::Frame &frame) {
        return frame.address == entity.address && frame.command == command && frame.g[0] == code;
      });
  result.requestHex = busResult.requestHex;
  result.responseHex = busResult.responseHex;
  if (!busResult.ok) {
    result.error = busResult.error;
    g_boards[entity.address].online = false;
    touch();
    return result;
  }

  LightState &state = g_lights[entityId];
  state.isOn = (normalized == "on") ? 1 : 0;
  state.updatedAt = millis();
  touch();

  String pollError;
  result.pollVerified = pollAddress(entity.address, pollError);
  result.ok = true;
  return result;
}

CommandResult commandDimmer(const String &entityId, const String &action, int level) {
  CommandResult result;
  Entity entity;
  if (!resolveEntity(entityId, "dimmer", entity, result.error)) return result;

  String normalized = action;
  normalized.toLowerCase();
  DimmerState &state = g_dimmers[entityId];
  uint8_t target = state.level;

  if (level >= 0) {
    target = constrain(level, protocol::DIMMER_MIN_LEVEL, protocol::DIMMER_MAX_LEVEL);
    normalized = "set";
  } else if (normalized == "off") {
    target = protocol::DIMMER_MIN_LEVEL;
  } else if (normalized == "on") {
    target = state.level > 0 ? state.level : state.lastOnLevel;
  } else if (normalized == "toggle") {
    target = state.level > 0 ? protocol::DIMMER_MIN_LEVEL : state.lastOnLevel;
  } else {
    result.error = F("Azione dimmer non valida (on, off, toggle, set + level 0..9)");
    return result;
  }
  if (target < 1 && normalized == "on") target = protocol::DIMMER_MAX_LEVEL;

  const uint8_t payload[2] = {protocol::DIMMER_SET_KEY, target};
  SheltrBus::Result busResult =
      sendWithRetries(entity.address, protocol::CMD_DIMMER, payload, 2,
                      [target, &entity](const protocol::Frame &frame) {
                        return frame.address == entity.address &&
                               frame.command == protocol::CMD_DIMMER &&
                               frame.g[0] == protocol::DIMMER_SET_KEY && frame.g[1] == target;
                      });
  result.requestHex = busResult.requestHex;
  result.responseHex = busResult.responseHex;
  if (!busResult.ok) {
    result.error = busResult.error;
    g_boards[entity.address].online = false;
    touch();
    return result;
  }

  state.level = target;
  state.isOn = target > 0 ? 1 : 0;
  if (target > 0) state.lastOnLevel = target;
  state.updatedAt = millis();
  touch();

  String pollError;
  result.pollVerified = pollAddress(entity.address, pollError);
  result.ok = true;
  return result;
}

CommandResult commandShutter(const String &entityId, const String &action) {
  CommandResult result;
  Entity entity;
  if (!resolveEntity(entityId, "shutter", entity, result.error)) return result;

  String normalized = action;
  normalized.toLowerCase();
  uint8_t code = 0;
  if (normalized == "up" || normalized == "open") {
    code = protocol::SHUTTER_UP;
    normalized = "up";
  } else if (normalized == "down" || normalized == "close") {
    code = protocol::SHUTTER_DOWN;
    normalized = "down";
  } else if (normalized == "stop") {
    code = protocol::SHUTTER_STOP;
  } else {
    result.error = F("Azione tapparella non valida (up, down, stop)");
    return result;
  }

  const uint8_t payload[2] = {entity.channel, code};
  SheltrBus::Result busResult =
      sendWithRetries(entity.address, protocol::CMD_SHUTTER, payload, 2,
                      [code, &entity](const protocol::Frame &frame) {
                        return frame.address == entity.address &&
                               frame.command == protocol::CMD_SHUTTER &&
                               frame.g[0] == entity.channel && frame.g[1] == code;
                      });
  result.requestHex = busResult.requestHex;
  result.responseHex = busResult.responseHex;
  if (!busResult.ok) {
    result.error = busResult.error;
    g_boards[entity.address].online = false;
    touch();
    return result;
  }

  ShutterState &state = g_shutters[entityId];
  state.action = normalized;
  state.updatedAt = millis();
  touch();
  result.ok = true;
  return result;
}

CommandResult commandThermostat(const String &entityId, bool hasSetpoint, float setpoint,
                                const String &mode, int8_t power) {
  CommandResult result;
  Entity entity;
  if (!resolveEntity(entityId, "thermostat", entity, result.error)) return result;
  if (!hasSetpoint && !mode.length() && power < 0) {
    result.error = F("Specifica almeno uno tra setpoint, mode, power");
    return result;
  }

  ThermostatState &state = g_thermostats[entityId];
  float nextSetpoint = state.setpoint;
  if (hasSetpoint) {
    nextSetpoint = constrain(roundf(setpoint * 2.0f) / 2.0f, 5.0f, 30.0f);
  }

  if (mode.length()) {
    String normalized = mode;
    normalized.toLowerCase();
    const bool summer = (normalized == "summer" || normalized == "cool" || normalized == "estate");
    const uint8_t modeByte = summer ? protocol::THERMOSTAT_MODE_SUMMER : protocol::THERMOSTAT_MODE_WINTER;
    const uint8_t payload[1] = {modeByte};
    SheltrBus::Result busResult =
        sendWithRetries(entity.address, protocol::CMD_THERMOSTAT_MODE, payload, 1,
                        [summer, &entity](const protocol::Frame &frame) {
                          if (frame.address != entity.address ||
                              frame.command != protocol::CMD_THERMOSTAT_MODE) {
                            return false;
                          }
                          // Alcune release rispondono 0xFF sull'inverno.
                          return summer ? frame.g[0] == protocol::THERMOSTAT_MODE_SUMMER
                                        : (frame.g[0] == protocol::THERMOSTAT_MODE_WINTER ||
                                           frame.g[0] == 0xFF);
                        });
    result.requestHex = busResult.requestHex;
    result.responseHex = busResult.responseHex;
    if (!busResult.ok) {
      result.error = busResult.error;
      g_boards[entity.address].online = false;
      touch();
      return result;
    }
    state.mode = summer ? "summer" : "winter";
    delay(220);  // gap tra frame consecutivi verso la stessa scheda
  }

  const bool powerOff = (power == 0);
  if (powerOff || hasSetpoint || power == 1) {
    uint8_t intPart = 0;
    uint8_t decPart = 0;
    if (!powerOff) protocol::splitTemperature(nextSetpoint, intPart, decPart);
    const uint8_t payload[2] = {intPart, decPart};
    SheltrBus::Result busResult =
        sendWithRetries(entity.address, protocol::CMD_THERMOSTAT_SETPOINT, payload, 2,
                        [intPart, decPart, &entity](const protocol::Frame &frame) {
                          return frame.address == entity.address &&
                                 frame.command == protocol::CMD_THERMOSTAT_SETPOINT &&
                                 frame.g[0] == intPart && frame.g[1] == decPart;
                        });
    result.requestHex = busResult.requestHex;
    result.responseHex = busResult.responseHex;
    if (!busResult.ok) {
      result.error = busResult.error;
      g_boards[entity.address].online = false;
      touch();
      return result;
    }
    state.isOn = powerOff ? 0 : 1;
    if (!powerOff) state.setpoint = nextSetpoint;
  }

  state.updatedAt = millis();
  touch();

  String pollError;
  result.pollVerified = pollAddress(entity.address, pollError);
  result.ok = true;
  return result;
}

CommandResult sendRawFrame(const uint8_t *frame) {
  CommandResult result;
  SheltrBus::Result busResult = Bus.sendRaw(frame, true, nullptr);
  result.requestHex = busResult.requestHex;
  result.responseHex = busResult.responseHex;
  result.ok = busResult.ok;
  result.error = busResult.error;

  if (busResult.ok && busResult.hasFrame) {
    protocol::Poll poll;
    if (protocol::decodePoll(busResult.frame, poll)) {
      BoardState &state = g_boards[busResult.frame.address];
      state.online = true;
      state.hasPoll = true;
      state.poll = poll;
      state.updatedAt = millis();
      state.lastError = "";
      applyPollToEntities(busResult.frame.address, poll);
    }
  }
  return result;
}

void statusJson(JsonObject out, bool refresh, const std::vector<uint8_t> *refreshAddresses) {
  const cfg::Config &current = cfg::config();

  JsonArray refreshErrors = out["refreshErrors"].to<JsonArray>();
  if (refresh) {
    std::vector<uint8_t> targets =
        refreshAddresses != nullptr ? *refreshAddresses : cfg::allAddresses();
    for (uint8_t address : targets) {
      String error;
      if (!pollAddress(address, error)) {
        JsonObject item = refreshErrors.add<JsonObject>();
        item["address"] = address;
        item["error"] = error;
      }
      delay(20);
    }
    g_lastPollAt = millis();
  }

  out["instanceId"] = current.device.id;
  out["id"] = current.device.id;
  out["name"] = current.device.name;
  out["deviceType"] = "sheltr_esp";
  out["protocolVersion"] = "1.6";
  out["firmware"] = SHELTR_FW_VERSION;
  out["updatedAt"] = isoTimestamp();
  out["dataSaver"] = false;
  out["commandTopic"] = current.cloud.instanceId + "/cmd";
  out["responseTopic"] = current.cloud.instanceId + "/pub";
  out["payloadFormat"] = current.cloud.payloadFormat;

  JsonObject device = out["device"].to<JsonObject>();
  device["id"] = current.device.id;
  device["name"] = current.device.name;
  device["online"] = anyBoardOnline();
  device["lastSeenAt"] = isoTimestamp();
  device["heartbeatMinutes"] = 0;
  device["type"] = "sheltr_esp";

  JsonArray palette = out["roomPalette"].to<JsonArray>();
  for (size_t i = 0; i < cfg::ROOM_PALETTE_SIZE; i++) palette.add(cfg::ROOM_PALETTE[i]);

  JsonObject roomColors = out["roomColors"].to<JsonObject>();

  // Stanze: raccolte da canali e sequenze (un pulsante virtuale può stare da solo).
  std::vector<String> roomNames;
  auto addRoom = [&roomNames](const String &name) {
    for (const String &existing : roomNames) {
      if (existing == name) return;
    }
    roomNames.push_back(name);
  };
  for (const cfg::Board &board : current.boards) {
    for (const cfg::Channel &channel : board.channels) addRoom(channel.room);
  }
  for (const cfg::Sequence &sequence : current.sequences) addRoom(sequence.room);

  JsonArray rooms = out["rooms"].to<JsonArray>();
  for (const String &roomName : roomNames) {
    JsonObject room = rooms.add<JsonObject>();
    room["name"] = roomName;
    const String color = cfg::roomColor(roomName);
    room["color"] = color;
    roomColors[roomName] = color;
    JsonArray lights = room["lights"].to<JsonArray>();
    JsonArray dimmers = room["dimmers"].to<JsonArray>();
    JsonArray shutters = room["shutters"].to<JsonArray>();
    JsonArray thermostats = room["thermostats"].to<JsonArray>();
    JsonArray sequences = room["sequences"].to<JsonArray>();

    for (const cfg::Sequence &sequence : current.sequences) {
      if (sequence.room != roomName) continue;
      JsonObject item = sequences.add<JsonObject>();
      item["id"] = sequence.id;
      item["kind"] = "sequence";
      item["name"] = sequence.name;
      item["room"] = sequence.room;
      item["favorite"] = sequence.favorite;
      item["steps"] = static_cast<uint32_t>(sequence.steps.size());
      item["online"] = true;
    }

    for (const cfg::Board &board : current.boards) {
      for (const cfg::Channel &channel : board.channels) {
        if (channel.room != roomName) continue;
        const Entity entity = makeEntity(board, channel);
        JsonObject item;
        if (board.kind == "light") {
          item = lights.add<JsonObject>();
        } else if (board.kind == "dimmer") {
          item = dimmers.add<JsonObject>();
        } else if (board.kind == "shutter") {
          item = shutters.add<JsonObject>();
        } else {
          item = thermostats.add<JsonObject>();
        }
        channelStateJson(entity, item);
        JsonObject profile = item["profile"].to<JsonObject>();
        profile["enabled"] = channel.profile.enabled;
        profile["entries"] = channel.profile.entries.size();
      }
    }
  }

  JsonArray boards = out["boards"].to<JsonArray>();
  for (const cfg::Board &board : current.boards) {
    JsonObject item = boards.add<JsonObject>();
    item["id"] = board.id;
    item["name"] = board.name;
    item["address"] = board.address;
    item["kind"] = board.kind;
    item["online"] = boardOnline(board.address);
    const BoardState *state = boardState(board.address);
    if (state != nullptr && state->hasPoll) {
      JsonObject poll = item["poll"].to<JsonObject>();
      poll["boardType"] = state->poll.boardType;
      poll["release"] = state->poll.release;
      poll["outputMask"] = state->poll.outputMask;
      poll["inputMask"] = state->poll.inputMask;
      poll["dimmerLevel"] = state->poll.dimmerLevel;
      poll["temperature"] = roundf(state->poll.temperature * 10.0f) / 10.0f;
      poll["powerKw"] = state->poll.powerKw;
      poll["setpoint"] = state->poll.setpoint;
    }
    if (state != nullptr && state->lastError.length()) item["lastError"] = state->lastError;

    JsonArray channels = item["channels"].to<JsonArray>();
    for (const cfg::Channel &channel : board.channels) {
      JsonObject entry = channels.add<JsonObject>();
      channelStateJson(makeEntity(board, channel), entry);
    }
  }
}

}  // namespace devices
