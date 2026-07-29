#include "settings.h"

#include <LittleFS.h>

#include "board_pins.h"
#include "json_utils.h"

namespace cfg {

const char *ROOM_PALETTE[10] = {
    "#f9d5d3",  // rosa corallo
    "#fde2c8",  // pesca
    "#faf3c0",  // giallo crema
    "#d9efd3",  // verde menta
    "#c9e9e3",  // acqua
    "#d4e5f7",  // azzurro polvere
    "#dcd6f7",  // lilla
    "#f3d9ec",  // rosa orchidea
    "#ece0d1",  // sabbia
    "#e3e7ee",  // grigio perla
};

namespace {

constexpr const char *CONFIG_PATH = "/config.json";
Config g_config;
bool g_fsMounted = false;

int toInt(JsonVariantConst value, int fallback) {
  if (value.is<int>() || value.is<float>()) return value.as<int>();
  if (value.is<const char *>()) {
    const char *text = value.as<const char *>();
    if (text == nullptr || *text == '\0') return fallback;
    return atoi(text);
  }
  if (value.is<bool>()) return value.as<bool>() ? 1 : 0;
  return fallback;
}

float toFloat(JsonVariantConst value, float fallback) {
  if (value.is<float>() || value.is<int>()) return value.as<float>();
  if (value.is<const char *>()) {
    String text = value.as<const char *>();
    text.replace(',', '.');
    if (!text.length()) return fallback;
    return text.toFloat();
  }
  return fallback;
}

bool toBool(JsonVariantConst value, bool fallback) {
  if (value.is<bool>()) return value.as<bool>();
  if (value.is<int>()) return value.as<int>() != 0;
  if (value.is<const char *>()) {
    String text = value.as<const char *>();
    text.toLowerCase();
    if (text == "1" || text == "true" || text == "on" || text == "yes") return true;
    if (text == "0" || text == "false" || text == "off" || text == "no") return false;
  }
  return fallback;
}

String toText(JsonVariantConst value, const String &fallback) {
  if (value.is<const char *>()) {
    String text = value.as<const char *>();
    text.trim();
    return text.length() ? text : fallback;
  }
  if (value.is<int>() || value.is<float>()) return String(value.as<float>(), 0);
  return fallback;
}

String normalizeTime(const String &value, const String &fallback) {
  int colon = value.indexOf(':');
  if (colon <= 0) return fallback;
  int hours = value.substring(0, colon).toInt();
  int minutes = value.substring(colon + 1).toInt();
  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return fallback;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", hours, minutes);
  return String(buffer);
}

uint8_t normalizeDays(JsonVariantConst value) {
  if (value.is<JsonArrayConst>()) {
    uint8_t mask = 0;
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
      const int day = toInt(item, 0);
      if (day >= 1 && day <= 7) mask |= (1 << (day - 1));
    }
    return mask ? mask : 0x7F;
  }
  if (value.is<int>()) {
    const uint8_t mask = static_cast<uint8_t>(value.as<int>()) & 0x7F;
    return mask ? mask : 0x7F;
  }
  return 0x7F;
}

void daysToJson(uint8_t mask, JsonArray out) {
  for (uint8_t day = 1; day <= 7; day++) {
    if (mask & (1 << (day - 1))) out.add(day);
  }
}

void applyDefaults(Config &target, bool keepNetwork) {
  NetworkCfg network = target.network;
  const String deviceId = target.device.id;  // l'UUID sopravvive al ripristino
  target = Config();
  target.device.id = deviceId.length() ? deviceId : newUuid();
  target.device.name = "Sheltr ESP";
  target.bus.tx = SHELTR_BUS_TX_DEFAULT;
  target.bus.rx = SHELTR_BUS_RX_DEFAULT;
  target.bus.de = SHELTR_BUS_DE_DEFAULT;
  target.cloud.instanceId = "sheltr-esp";
  target.cloud.instanceName = target.device.name;
  target.mqtt.clientId = String("sheltr-") + target.device.id.substring(0, 8);
  if (keepNetwork) {
    target.network = network;
  } else {
    target.network.hostname = "sheltr";
  }
  // Nessuna scheda predefinita: l'impianto si configura dall'interfaccia.
  target.boards.clear();
  target.sequences.clear();

  // Gli 8 ingressi digitali esistono sempre, disattivati e sui GPIO liberi del connettore.
  const int8_t inputPins[INPUT_COUNT] = {1, 2, 21, 38, 39, 40, 41, 47};
  target.inputs.assign(INPUT_COUNT, InputCfg());
  for (size_t i = 0; i < INPUT_COUNT; i++) {
    target.inputs[i].gpio = inputPins[i];
    target.inputs[i].name = String("Ingresso ") + (i + 1);
  }
}

void parseProfile(JsonVariantConst raw, const String &kind, Profile &profile) {
  profile.enabled = false;
  profile.entries.clear();
  if (!raw.is<JsonObjectConst>()) return;
  JsonObjectConst input = raw.as<JsonObjectConst>();
  profile.enabled = toBool(input["enabled"], false);
  JsonArrayConst entries = input["entries"].as<JsonArrayConst>();
  if (entries.isNull()) return;
  for (JsonVariantConst item : entries) {
    if (!item.is<JsonObjectConst>()) continue;
    JsonObjectConst entry = item.as<JsonObjectConst>();
    ProfileEntry parsed;
    parsed.days = normalizeDays(entry["days"]);
    if (kind == "thermostat") {
      parsed.from = normalizeTime(toText(entry["from"], "00:00"), "00:00");
      parsed.to = normalizeTime(toText(entry["to"], "23:59"), "23:59");
      parsed.setpoint = constrain(toFloat(entry["setpoint"], 21.0f), 5.0f, 30.0f);
      String mode = toText(entry["mode"], "winter");
      mode.toLowerCase();
      parsed.mode = (mode == "summer" || mode == "estate") ? "summer" : "winter";
    } else {
      parsed.time = normalizeTime(toText(entry["time"], "00:00"), "00:00");
      String action = toText(entry["action"], kind == "shutter" ? "down" : "off");
      action.toLowerCase();
      if (kind == "shutter") {
        parsed.action = (action == "up") ? "up" : "down";
      } else {
        parsed.action = (action == "on") ? "on" : "off";
      }
    }
    profile.entries.push_back(parsed);
    if (profile.entries.size() >= 24) break;
  }
}

void profileToJson(const Profile &profile, const String &kind, JsonObject out) {
  out["enabled"] = profile.enabled;
  JsonArray entries = out["entries"].to<JsonArray>();
  for (const ProfileEntry &entry : profile.entries) {
    JsonObject item = entries.add<JsonObject>();
    if (kind == "thermostat") {
      item["from"] = entry.from;
      item["to"] = entry.to;
      item["setpoint"] = entry.setpoint;
      item["mode"] = entry.mode;
    } else {
      item["time"] = entry.time;
      item["action"] = entry.action;
    }
    daysToJson(entry.days, item["days"].to<JsonArray>());
  }
}

void parseBoards(JsonArrayConst input, std::vector<Board> &boards) {
  boards.clear();
  if (input.isNull()) return;
  size_t index = 0;
  for (JsonVariantConst raw : input) {
    if (!raw.is<JsonObjectConst>()) continue;
    JsonObjectConst item = raw.as<JsonObjectConst>();
    Board board;
    index++;
    board.kind = toText(item["kind"], "light");
    board.kind.toLowerCase();
    if (board.kind != "light" && board.kind != "shutter" && board.kind != "dimmer" &&
        board.kind != "thermostat") {
      board.kind = "light";
    }
    const uint8_t maxChannels = maxChannelsForKind(board.kind);
    // L'id è un UUID generato alla creazione della scheda: qui viene solo preservato
    // (gli impianti già configurati mantengono il loro id storico) o creato se manca.
    const String incomingId = cleanText(toText(item["id"], ""), "");
    board.id = incomingId.length() ? slugify(incomingId, newUuid()) : newUuid();
    for (const Board &existing : boards) {
      if (existing.id == board.id) board.id = newUuid();  // niente id duplicati sul bus
    }
    board.name = toText(item["name"], String("Scheda ") + index);
    board.address = constrain(toInt(item["address"], index), 0, 254);
    board.channelStart = constrain(toInt(item["channelStart"], 1), 1, maxChannels);
    board.channelEnd = constrain(toInt(item["channelEnd"], maxChannels), board.channelStart,
                                 maxChannels);

    std::map<uint8_t, JsonObjectConst> saved;
    JsonArrayConst channels = item["channels"].as<JsonArrayConst>();
    if (!channels.isNull()) {
      for (JsonVariantConst channelRaw : channels) {
        if (!channelRaw.is<JsonObjectConst>()) continue;
        JsonObjectConst channelItem = channelRaw.as<JsonObjectConst>();
        const int number = toInt(channelItem["channel"], -1);
        if (number >= 1 && number <= maxChannels) saved[static_cast<uint8_t>(number)] = channelItem;
      }
    }

    for (uint8_t number = board.channelStart; number <= board.channelEnd; number++) {
      Channel channel;
      channel.channel = number;
      auto found = saved.find(number);
      if (found != saved.end()) {
        channel.name = toText(found->second["name"], defaultChannelName(board.kind, number));
        channel.room = toText(found->second["room"], "Senza stanza");
        channel.favorite = toBool(found->second["favorite"], false);
        channel.notifyOnChange = toBool(found->second["notifyOnChange"], false);
        parseProfile(found->second["profile"], board.kind, channel.profile);
      } else {
        channel.name = defaultChannelName(board.kind, number);
        channel.room = "Senza stanza";
      }
      board.channels.push_back(channel);
    }
    boards.push_back(board);
    if (boards.size() >= 32) break;
  }
}

void parseSequences(JsonArrayConst input, std::vector<Sequence> &sequences) {
  sequences.clear();
  if (input.isNull()) return;
  size_t index = 0;
  for (JsonVariantConst raw : input) {
    if (!raw.is<JsonObjectConst>()) continue;
    JsonObjectConst item = raw.as<JsonObjectConst>();
    index++;
    Sequence sequence;
    sequence.id = slugify(toText(item["id"], ""), String("seq-") + index);
    sequence.name = toText(item["name"], sequence.id);
    sequence.room = toText(item["room"], "Senza stanza");
    sequence.favorite = toBool(item["favorite"], false);
    sequence.busTrigger = constrain(toInt(item["busTrigger"], 0), 0, 255);
    parseProfile(item["schedule"], "light", sequence.schedule);  // orari di avvio

    JsonArrayConst steps = item["steps"].as<JsonArrayConst>();
    if (!steps.isNull()) {
      for (JsonVariantConst stepRaw : steps) {
        if (!stepRaw.is<JsonObjectConst>()) continue;
        JsonObjectConst stepItem = stepRaw.as<JsonObjectConst>();
        SequenceStep step;
        step.channelId = toText(stepItem["channelId"], "");
        step.action = toText(stepItem["action"], "");
        step.action.toLowerCase();
        if (stepItem["value"].is<float>() || stepItem["value"].is<int>()) {
          step.value = toFloat(stepItem["value"], 0.0f);
          step.hasValue = true;
        }
        String mode = toText(stepItem["mode"], "");
        mode.toLowerCase();
        if (mode == "summer" || mode == "winter") step.mode = mode;
        step.delaySec = constrain(toInt(stepItem["delaySec"], 0), 0, 3600);
        if (!step.channelId.length() && !step.delaySec) continue;
        sequence.steps.push_back(step);
        if (sequence.steps.size() >= 32) break;
      }
    }
    sequences.push_back(sequence);
    if (sequences.size() >= 16) break;
  }
}

void sequenceToJson(const Sequence &sequence, JsonObject out) {
  out["id"] = sequence.id;
  out["name"] = sequence.name;
  out["room"] = sequence.room;
  out["favorite"] = sequence.favorite;
  out["busTrigger"] = sequence.busTrigger;
  JsonObject schedule = out["schedule"].to<JsonObject>();
  schedule["enabled"] = sequence.schedule.enabled;
  JsonArray entries = schedule["entries"].to<JsonArray>();
  for (const ProfileEntry &entry : sequence.schedule.entries) {
    JsonObject item = entries.add<JsonObject>();
    item["time"] = entry.time;
    daysToJson(entry.days, item["days"].to<JsonArray>());
  }
  JsonArray steps = out["steps"].to<JsonArray>();
  for (const SequenceStep &step : sequence.steps) {
    JsonObject item = steps.add<JsonObject>();
    item["channelId"] = step.channelId;
    item["action"] = step.action;
    if (step.hasValue) item["value"] = step.value;
    if (step.mode.length()) item["mode"] = step.mode;
    item["delaySec"] = step.delaySec;
  }
}

void parseInputs(JsonArrayConst input, std::vector<InputCfg> &inputs) {
  // Gli 8 ingressi esistono sempre: il JSON aggiorna solo quelli presenti.
  const int8_t defaults[INPUT_COUNT] = {1, 2, 21, 38, 39, 40, 41, 47};
  if (inputs.size() != INPUT_COUNT) {
    inputs.assign(INPUT_COUNT, InputCfg());
    for (size_t i = 0; i < INPUT_COUNT; i++) {
      inputs[i].gpio = defaults[i];
      inputs[i].name = String("Ingresso ") + (i + 1);
    }
  }
  if (input.isNull()) return;

  size_t index = 0;
  for (JsonVariantConst raw : input) {
    if (index >= INPUT_COUNT) break;
    if (!raw.is<JsonObjectConst>()) {
      index++;
      continue;
    }
    JsonObjectConst item = raw.as<JsonObjectConst>();
    InputCfg &target = inputs[index];
    target.enabled = toBool(item["enabled"], target.enabled);
    target.gpio = constrain(toInt(item["gpio"], target.gpio), -1, 48);
    target.pullup = toBool(item["pullup"], target.pullup);
    target.activeLow = toBool(item["activeLow"], target.activeLow);
    target.debounceMs = constrain(toInt(item["debounceMs"], target.debounceMs), 5, 2000);
    target.name = toText(item["name"], target.name.length() ? target.name
                                                            : String("Ingresso ") + (index + 1));
    target.room = toText(item["room"], target.room.length() ? target.room : String("Senza stanza"));
    target.favorite = toBool(item["favorite"], target.favorite);
    target.notifyOnChange = toBool(item["notifyOnChange"], target.notifyOnChange);
    if (item["notifyText"].is<const char *>()) {
      target.notifyText = toText(item["notifyText"], "");
    }
    if (item["sequenceId"].is<const char *>()) {
      target.sequenceId = toText(item["sequenceId"], "");
    }
    index++;
  }
}

}  // namespace

Config &config() { return g_config; }

bool filesystemMounted() { return g_fsMounted; }

String cleanText(const String &value, const String &fallback) {
  String text = value;
  text.trim();
  return text.length() ? text : fallback;
}

String slugify(const String &value, const String &fallback) {
  String out;
  bool lastDash = false;
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (isalnum(static_cast<unsigned char>(c))) {
      out += static_cast<char>(tolower(c));
      lastDash = false;
    } else if (!lastDash && out.length()) {
      out += '-';
      lastDash = true;
    }
  }
  while (out.endsWith("-")) out.remove(out.length() - 1);
  return out.length() ? out : fallback;
}

uint8_t maxChannelsForKind(const String &kind) {
  if (kind == "shutter") return 4;
  if (kind == "dimmer") return 1;
  if (kind == "thermostat") return 1;
  return 8;
}

String defaultChannelName(const String &kind, uint8_t channel) {
  if (kind == "shutter") return String("Tapparella ") + channel;
  if (kind == "dimmer") return String("Dimmer ") + channel;
  if (kind == "thermostat") return String("Termostato ") + channel;
  return String("Luce ") + channel;
}

String kindLabel(const String &kind) {
  if (kind == "shutter") return "Tapparelle";
  if (kind == "dimmer") return "Dimmer";
  if (kind == "thermostat") return "Termostati";
  return "Luci";
}

String entityId(const String &boardId, uint8_t channel) {
  return boardId + "-c" + String(channel);
}

String roomColor(const String &room) {
  auto found = g_config.roomColors.find(room);
  if (found != g_config.roomColors.end()) return found->second;
  // Colore stabile derivato dal nome, come nel portale.
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < room.length(); i++) {
    hash ^= static_cast<uint8_t>(room[i]);
    hash *= 16777619u;
  }
  return ROOM_PALETTE[hash % ROOM_PALETTE_SIZE];
}

void setRoomColor(const String &room, const String &color) {
  if (!room.length()) return;
  g_config.roomColors[room] = color;
}

Board *findBoard(const String &boardId) {
  for (Board &board : g_config.boards) {
    if (board.id == boardId) return &board;
  }
  return nullptr;
}

Board *findBoardByAddress(uint8_t address) {
  for (Board &board : g_config.boards) {
    if (board.address == address) return &board;
  }
  return nullptr;
}

Sequence *findSequence(const String &sequenceId) {
  for (Sequence &sequence : g_config.sequences) {
    if (sequence.id == sequenceId) return &sequence;
  }
  return nullptr;
}

Sequence *findSequenceByBusTrigger(uint16_t trigger) {
  if (trigger == 0) return nullptr;
  for (Sequence &sequence : g_config.sequences) {
    if (sequence.busTrigger == trigger) return &sequence;
  }
  return nullptr;
}

String newUuid() {
  // UUID v4 costruito su esp_random() (alimentato dall'hardware RNG).
  uint8_t bytes[16];
  for (size_t i = 0; i < sizeof(bytes); i += 4) {
    const uint32_t value = esp_random();
    memcpy(bytes + i, &value, 4);
  }
  bytes[6] = (bytes[6] & 0x0F) | 0x40;  // versione 4
  bytes[8] = (bytes[8] & 0x3F) | 0x80;  // variante RFC 4122
  char buffer[37];
  snprintf(buffer, sizeof(buffer),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
           bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
           bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return String(buffer);
}

Channel *findChannel(const String &id, Board **boardOut) {
  for (Board &board : g_config.boards) {
    for (Channel &channel : board.channels) {
      if (entityId(board.id, channel.channel) == id) {
        if (boardOut != nullptr) *boardOut = &board;
        return &channel;
      }
    }
  }
  return nullptr;
}

std::vector<uint8_t> allAddresses() {
  std::vector<uint8_t> addresses;
  for (const Board &board : g_config.boards) {
    bool present = false;
    for (uint8_t value : addresses) {
      if (value == board.address) present = true;
    }
    if (!present) addresses.push_back(board.address);
  }
  return addresses;
}

void resetDefaults(bool keepNetwork) {
  applyDefaults(g_config, keepNetwork);
  g_config.revision++;
}

void profileJson(const Profile &profile, const String &kind, JsonObject out) {
  profileToJson(profile, kind, out);
}

void sequenceJson(const Sequence &sequence, JsonObject out) { sequenceToJson(sequence, out); }

bool applyCloudSettings(JsonObjectConst input) {
  if (input.isNull()) return false;

  // Guardia di revisione: i messaggi retained possono essere piu' vecchi di quanto
  // gia' applicato (o di una modifica fatta a mano sul dispositivo).
  const uint32_t revision = static_cast<uint32_t>(input["revision"] | 0);
  if (revision != 0 && revision <= g_config.cloud.settingsRevision) return false;

  JsonArrayConst channels = input["channels"].as<JsonArrayConst>();
  if (channels.isNull()) return false;

  bool changed = false;
  for (JsonVariantConst item : channels) {
    if (!item.is<JsonObjectConst>()) continue;
    JsonObjectConst entry = item.as<JsonObjectConst>();
    String id = toText(entry["id"], "");
    if (!id.length()) {
      const String boardId = toText(entry["boardId"], "");
      const uint8_t channel = static_cast<uint8_t>(constrain(toInt(entry["channel"], 0), 0, 8));
      if (!boardId.length() || channel == 0) continue;
      id = entityId(boardId, channel);
    }
    Board *board = nullptr;
    Channel *channel = findChannel(id, &board);
    if (channel == nullptr || board == nullptr) continue;

    if (entry["favorite"].is<bool>()) {
      const bool favorite = entry["favorite"].as<bool>();
      if (channel->favorite != favorite) {
        channel->favorite = favorite;
        changed = true;
      }
    }
    if (entry["notifyOnChange"].is<bool>()) {
      const bool notify = entry["notifyOnChange"].as<bool>();
      if (channel->notifyOnChange != notify) {
        channel->notifyOnChange = notify;
        changed = true;
      }
    }
    if (entry["profile"].is<JsonObjectConst>()) {
      parseProfile(entry["profile"], board->kind, channel->profile);
      changed = true;
    }
  }

  // Ingressi: il portale può impostare notifica e testo personalizzato.
  JsonArrayConst inputEntries = input["inputs"].as<JsonArrayConst>();
  if (!inputEntries.isNull()) {
    for (JsonVariantConst item : inputEntries) {
      if (!item.is<JsonObjectConst>()) continue;
      JsonObjectConst entry = item.as<JsonObjectConst>();
      const int index = toInt(entry["index"], -1);
      if (index < 0 || static_cast<size_t>(index) >= g_config.inputs.size()) continue;
      InputCfg &target = g_config.inputs[index];
      if (entry["notifyOnChange"].is<bool>()) {
        const bool notify = entry["notifyOnChange"].as<bool>();
        if (target.notifyOnChange != notify) {
          target.notifyOnChange = notify;
          changed = true;
        }
      }
      if (entry["notifyText"].is<const char *>()) {
        const String text = toText(entry["notifyText"], "");
        if (target.notifyText != text) {
          target.notifyText = text;
          changed = true;
        }
      }
    }
  }

  // Sequenze create/modificate dal portale: sostituiscono l'elenco locale.
  // Il portale invia sempre la lista completa (l'ha importata da qui poco prima).
  JsonArrayConst sequenceEntries = input["sequences"].as<JsonArrayConst>();
  if (!sequenceEntries.isNull()) {
    std::vector<Sequence> parsed;
    parseSequences(sequenceEntries, parsed);
    JsonDocument before;
    JsonDocument after;
    JsonArray beforeArr = before.to<JsonArray>();
    JsonArray afterArr = after.to<JsonArray>();
    for (const Sequence &item : g_config.sequences) sequenceToJson(item, beforeArr.add<JsonObject>());
    for (const Sequence &item : parsed) sequenceToJson(item, afterArr.add<JsonObject>());
    String beforeText;
    String afterText;
    serializeJson(before, beforeText);
    serializeJson(after, afterText);
    if (beforeText != afterText) {
      g_config.sequences = parsed;
      changed = true;
    }
  }

  if (revision != 0 && revision != g_config.cloud.settingsRevision) {
    g_config.cloud.settingsRevision = revision;
    changed = true;
  }
  if (changed) g_config.revision++;
  return changed;
}

bool applyJson(JsonObjectConst input, String &error) {
  if (input.isNull()) {
    error = F("payload non valido");
    return false;
  }

  Config next = g_config;

  if (input["device"].is<JsonObjectConst>()) {
    JsonObjectConst device = input["device"];
    // `device.id` è l'UUID del dispositivo: si genera al primo avvio e non si tocca.
    next.device.name = toText(device["name"], next.device.name);
  }

  if (input["auth"].is<JsonObjectConst>()) {
    JsonObjectConst auth = input["auth"];
    next.auth.enabled = toBool(auth["enabled"], next.auth.enabled);
    next.auth.username = toText(auth["username"], next.auth.username);
    const String password = toText(auth["password"], "");
    if (password.length() && password != "********") next.auth.password = password;
    const String systemPassword = toText(auth["systemPassword"], "");
    if (systemPassword.length() && systemPassword != "********") {
      next.auth.systemPassword = systemPassword;
    }
  }

  if (input["bus"].is<JsonObjectConst>()) {
    JsonObjectConst bus = input["bus"];
    next.bus.tx = constrain(toInt(bus["tx"], next.bus.tx), -1, 48);
    next.bus.rx = constrain(toInt(bus["rx"], next.bus.rx), -1, 48);
    next.bus.de = constrain(toInt(bus["de"], next.bus.de), -1, 48);
    next.bus.baud = constrain(toInt(bus["baud"], next.bus.baud), 1200, 115200);
    next.bus.timeoutMs = constrain(toInt(bus["timeoutMs"], next.bus.timeoutMs), 100, 8000);
    next.bus.retries = constrain(toInt(bus["retries"], next.bus.retries), 0, 5);
    next.bus.pollIntervalSec = constrain(toInt(bus["pollIntervalSec"], next.bus.pollIntervalSec), 0,
                                         3600);
  }

  if (input["network"].is<JsonObjectConst>()) {
    JsonObjectConst network = input["network"];
    next.network.hostname = slugify(toText(network["hostname"], next.network.hostname), "sheltr");
    next.network.wifiSsid = toText(network["wifiSsid"], next.network.wifiSsid);
    const String wifiPassword = toText(network["wifiPassword"], "");
    if (wifiPassword.length() && wifiPassword != "********") {
      next.network.wifiPassword = wifiPassword;
    }
    if (network["wifiSsid"].is<const char *>() && !toText(network["wifiSsid"], "").length()) {
      next.network.wifiSsid = "";
      next.network.wifiPassword = "";
    }
    next.network.apSsid = toText(network["apSsid"], next.network.apSsid);
    const String apPassword = toText(network["apPassword"], "");
    if (apPassword.length() && apPassword != "********") next.network.apPassword = apPassword;
    next.network.apFallback = toBool(network["apFallback"], next.network.apFallback);
    next.network.ethEnabled = toBool(network["ethEnabled"], next.network.ethEnabled);
    next.network.dhcp = toBool(network["dhcp"], next.network.dhcp);
    next.network.ip = toText(network["ip"], next.network.ip);
    next.network.gateway = toText(network["gateway"], next.network.gateway);
    next.network.subnet = toText(network["subnet"], next.network.subnet);
    next.network.dns1 = toText(network["dns1"], next.network.dns1);
    next.network.dns2 = toText(network["dns2"], next.network.dns2);
  }

  if (input["ntp"].is<JsonObjectConst>()) {
    JsonObjectConst ntp = input["ntp"];
    next.ntp.enabled = toBool(ntp["enabled"], next.ntp.enabled);
    next.ntp.server = toText(ntp["server"], next.ntp.server);
    next.ntp.tz = toText(ntp["tz"], next.ntp.tz);
  }

  if (input["rtc"].is<JsonObjectConst>()) {
    JsonObjectConst rtc = input["rtc"];
    next.rtc.enabled = toBool(rtc["enabled"], next.rtc.enabled);
    next.rtc.sda = constrain(toInt(rtc["sda"], next.rtc.sda), -1, 48);
    next.rtc.scl = constrain(toInt(rtc["scl"], next.rtc.scl), -1, 48);
    next.rtc.address = constrain(toInt(rtc["address"], next.rtc.address), 8, 119);
  }

  if (input["mqtt"].is<JsonObjectConst>()) {
    JsonObjectConst mqtt = input["mqtt"];
    next.mqtt.enabled = toBool(mqtt["enabled"], next.mqtt.enabled);
    next.mqtt.host = toText(mqtt["host"], next.mqtt.host);
    next.mqtt.port = constrain(toInt(mqtt["port"], next.mqtt.port), 1, 65535);
    next.mqtt.username = toText(mqtt["username"], next.mqtt.username);
    const String password = toText(mqtt["password"], "");
    if (password.length() && password != "********") next.mqtt.password = password;
    if (mqtt["password"].is<const char *>() && !toText(mqtt["password"], "").length()) {
      next.mqtt.password = "";
    }
    next.mqtt.clientId = toText(mqtt["clientId"], next.mqtt.clientId);
    next.mqtt.baseTopic = cleanText(toText(mqtt["baseTopic"], next.mqtt.baseTopic), "sheltr");
    next.mqtt.discovery = toBool(mqtt["discovery"], next.mqtt.discovery);
    next.mqtt.discoveryPrefix =
        cleanText(toText(mqtt["discoveryPrefix"], next.mqtt.discoveryPrefix), "homeassistant");
    next.mqtt.retain = toBool(mqtt["retain"], next.mqtt.retain);
    next.mqtt.qos = constrain(toInt(mqtt["qos"], next.mqtt.qos), 0, 1);
    next.mqtt.stateIntervalSec =
        constrain(toInt(mqtt["stateIntervalSec"], next.mqtt.stateIntervalSec), 5, 3600);
  }

  if (input["cloud"].is<JsonObjectConst>()) {
    JsonObjectConst cloud = input["cloud"];
    next.cloud.enabled = toBool(cloud["enabled"], next.cloud.enabled);
    next.cloud.host = toText(cloud["host"], next.cloud.host);
    next.cloud.port = constrain(toInt(cloud["port"], next.cloud.port), 1, 65535);
    next.cloud.username = toText(cloud["username"], next.cloud.username);
    const String password = toText(cloud["password"], "");
    if (password.length() && password != "********") next.cloud.password = password;
    if (cloud["password"].is<const char *>() && !toText(cloud["password"], "").length()) {
      next.cloud.password = "";
    }
    next.cloud.instanceId =
        slugify(toText(cloud["instanceId"], next.cloud.instanceId), next.device.id);
    next.cloud.instanceName = toText(cloud["instanceName"], next.cloud.instanceName);
    String format = toText(cloud["payloadFormat"], next.cloud.payloadFormat);
    format.toLowerCase();
    if (format == "frame_hex_space" || format == "frame_hex_compact" ||
        format == "frame_hex_space_crlf" || format == "frame_hex_compact_crlf") {
      next.cloud.payloadFormat = format;
    }
    next.cloud.portalUrl = toText(cloud["portalUrl"], next.cloud.portalUrl);
    next.cloud.settingsRevision =
        static_cast<uint32_t>(cloud["settingsRevision"] | next.cloud.settingsRevision);
  }

  if (input["roomColors"].is<JsonObjectConst>()) {
    next.roomColors.clear();
    for (JsonPairConst pair : input["roomColors"].as<JsonObjectConst>()) {
      const String color = toText(pair.value(), "");
      if (color.startsWith("#") && (color.length() == 7 || color.length() == 4)) {
        next.roomColors[String(pair.key().c_str())] = color;
      }
    }
  }

  if (input["boards"].is<JsonArrayConst>()) {
    parseBoards(input["boards"].as<JsonArrayConst>(), next.boards);
  }

  if (input["sequences"].is<JsonArrayConst>()) {
    parseSequences(input["sequences"].as<JsonArrayConst>(), next.sequences);
  }

  parseInputs(input["inputs"].as<JsonArrayConst>(), next.inputs);

  next.revision = g_config.revision + 1;
  g_config = next;
  return true;
}

void toJson(JsonObject out, bool includeSecrets) {
  const Config &current = g_config;
  out["revision"] = current.revision;

  JsonObject device = out["device"].to<JsonObject>();
  device["id"] = current.device.id;
  device["name"] = current.device.name;

  JsonObject auth = out["auth"].to<JsonObject>();
  auth["enabled"] = current.auth.enabled;
  auth["username"] = current.auth.username;
  auth["password"] = includeSecrets ? current.auth.password : String("********");
  auth["systemPassword"] = includeSecrets ? current.auth.systemPassword : String("********");

  JsonObject bus = out["bus"].to<JsonObject>();
  bus["tx"] = current.bus.tx;
  bus["rx"] = current.bus.rx;
  bus["de"] = current.bus.de;
  bus["baud"] = current.bus.baud;
  bus["timeoutMs"] = current.bus.timeoutMs;
  bus["retries"] = current.bus.retries;
  bus["pollIntervalSec"] = current.bus.pollIntervalSec;

  JsonObject network = out["network"].to<JsonObject>();
  network["hostname"] = current.network.hostname;
  network["wifiSsid"] = current.network.wifiSsid;
  network["wifiPassword"] =
      includeSecrets ? current.network.wifiPassword
                     : String(current.network.wifiPassword.length() ? "********" : "");
  network["apSsid"] = current.network.apSsid;
  network["apPassword"] = includeSecrets ? current.network.apPassword : String("********");
  network["apFallback"] = current.network.apFallback;
  network["ethEnabled"] = current.network.ethEnabled;
  network["dhcp"] = current.network.dhcp;
  network["ip"] = current.network.ip;
  network["gateway"] = current.network.gateway;
  network["subnet"] = current.network.subnet;
  network["dns1"] = current.network.dns1;
  network["dns2"] = current.network.dns2;

  JsonObject ntp = out["ntp"].to<JsonObject>();
  ntp["enabled"] = current.ntp.enabled;
  ntp["server"] = current.ntp.server;
  ntp["tz"] = current.ntp.tz;

  JsonObject rtc = out["rtc"].to<JsonObject>();
  rtc["enabled"] = current.rtc.enabled;
  rtc["sda"] = current.rtc.sda;
  rtc["scl"] = current.rtc.scl;
  rtc["address"] = current.rtc.address;

  JsonArray inputs = out["inputs"].to<JsonArray>();
  for (size_t i = 0; i < current.inputs.size(); i++) {
    const InputCfg &item = current.inputs[i];
    JsonObject entry = inputs.add<JsonObject>();
    entry["index"] = static_cast<uint32_t>(i);
    entry["enabled"] = item.enabled;
    entry["gpio"] = item.gpio;
    entry["pullup"] = item.pullup;
    entry["activeLow"] = item.activeLow;
    entry["debounceMs"] = item.debounceMs;
    entry["name"] = item.name;
    entry["room"] = item.room;
    entry["favorite"] = item.favorite;
    entry["notifyOnChange"] = item.notifyOnChange;
    entry["notifyText"] = item.notifyText;
    entry["sequenceId"] = item.sequenceId;
  }

  JsonObject mqtt = out["mqtt"].to<JsonObject>();
  mqtt["enabled"] = current.mqtt.enabled;
  mqtt["host"] = current.mqtt.host;
  mqtt["port"] = current.mqtt.port;
  mqtt["username"] = current.mqtt.username;
  mqtt["password"] =
      includeSecrets ? current.mqtt.password : String(current.mqtt.password.length() ? "********" : "");
  mqtt["clientId"] = current.mqtt.clientId;
  mqtt["baseTopic"] = current.mqtt.baseTopic;
  mqtt["discovery"] = current.mqtt.discovery;
  mqtt["discoveryPrefix"] = current.mqtt.discoveryPrefix;
  mqtt["retain"] = current.mqtt.retain;
  mqtt["qos"] = current.mqtt.qos;
  mqtt["stateIntervalSec"] = current.mqtt.stateIntervalSec;

  JsonObject cloud = out["cloud"].to<JsonObject>();
  cloud["enabled"] = current.cloud.enabled;
  cloud["host"] = current.cloud.host;
  cloud["port"] = current.cloud.port;
  cloud["username"] = current.cloud.username;
  cloud["password"] = includeSecrets ? current.cloud.password
                                     : String(current.cloud.password.length() ? "********" : "");
  cloud["instanceId"] = current.cloud.instanceId;
  cloud["instanceName"] = current.cloud.instanceName;
  cloud["payloadFormat"] = current.cloud.payloadFormat;
  cloud["portalUrl"] = current.cloud.portalUrl;
  cloud["settingsRevision"] = current.cloud.settingsRevision;
  cloud["configTopic"] = current.cloud.instanceId + "/config";
  cloud["commandTopic"] = current.cloud.instanceId + "/cmd";
  cloud["responseTopic"] = current.cloud.instanceId + "/pub";

  JsonObject roomColors = out["roomColors"].to<JsonObject>();
  for (const auto &pair : current.roomColors) roomColors[pair.first] = pair.second;

  JsonArray palette = out["roomPalette"].to<JsonArray>();
  for (size_t i = 0; i < ROOM_PALETTE_SIZE; i++) palette.add(ROOM_PALETTE[i]);

  JsonArray boards = out["boards"].to<JsonArray>();
  for (const Board &board : current.boards) {
    JsonObject item = boards.add<JsonObject>();
    item["id"] = board.id;
    item["name"] = board.name;
    item["kind"] = board.kind;
    item["address"] = board.address;
    item["channelStart"] = board.channelStart;
    item["channelEnd"] = board.channelEnd;
    JsonArray channels = item["channels"].to<JsonArray>();
    for (const Channel &channel : board.channels) {
      JsonObject entry = channels.add<JsonObject>();
      entry["channel"] = channel.channel;
      entry["name"] = channel.name;
      entry["room"] = channel.room;
      entry["favorite"] = channel.favorite;
      entry["notifyOnChange"] = channel.notifyOnChange;
      profileToJson(channel.profile, board.kind, entry["profile"].to<JsonObject>());
    }
  }

  JsonArray sequences = out["sequences"].to<JsonArray>();
  for (const Sequence &sequence : current.sequences) {
    sequenceToJson(sequence, sequences.add<JsonObject>());
  }
}

bool save() {
  if (!g_fsMounted) {
    log_e("Filesystem non montato: configurazione non salvata");
    return false;
  }
  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) {
    log_e("Impossibile aprire %s in scrittura", CONFIG_PATH);
    return false;
  }
  JsonDocument doc(&SpiRamAllocator::instance());
  JsonObject root = doc.to<JsonObject>();
  toJson(root, true);
  const size_t written = serializeJson(doc, file);
  file.close();
  log_i("Configurazione salvata (%u byte)", static_cast<unsigned>(written));
  return written > 0;
}

bool begin() {
  // La partizione dati si chiama "littlefs" nella nostra tabella, ma LittleFS.begin()
  // cerca di default l'etichetta "spiffs": senza il nome esplicito il mount fallisce
  // e ogni salvataggio viene perso al riavvio.
  g_fsMounted = LittleFS.begin(true, "/littlefs", 10, "littlefs");
  if (!g_fsMounted) {
    // Firmware installati con tabelle partizioni diverse (o precedenti) usano "spiffs".
    g_fsMounted = LittleFS.begin(true, "/littlefs", 10, "spiffs");
  }
  if (!g_fsMounted) {
    log_e("LittleFS non montato: la configurazione non verrà salvata");
    applyDefaults(g_config, false);
    return false;
  }

  applyDefaults(g_config, false);

  if (!LittleFS.exists(CONFIG_PATH)) {
    log_w("Nessuna configurazione salvata: uso i default");
    save();
    return true;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    log_e("Impossibile leggere %s", CONFIG_PATH);
    return false;
  }
  JsonDocument doc(&SpiRamAllocator::instance());
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    log_e("Configurazione non valida (%s): uso i default", error.c_str());
    return false;
  }

  String message;
  if (!applyJson(doc.as<JsonObjectConst>(), message)) {
    log_e("Configurazione rifiutata: %s", message.c_str());
    return false;
  }
  g_config.revision = 1;
  log_i("Configurazione caricata: %u schede", static_cast<unsigned>(g_config.boards.size()));
  return true;
}

}  // namespace cfg
