#include "inputs.h"

#include "mqtt_bridge.h"
#include "sequences.h"
#include "settings.h"

namespace inputs {

namespace {

struct InputState {
  int8_t gpio = -1;
  bool configured = false;
  bool active = false;         // stato logico stabile (contatto chiuso)
  bool lastRaw = false;        // ultima lettura grezza
  uint32_t changedAt = 0;      // istante dell'ultima variazione grezza
  uint32_t triggerCount = 0;
  uint32_t lastTriggerAt = 0;
};

InputState g_states[cfg::INPUT_COUNT];

bool readActive(const cfg::InputCfg &config) {
  const int level = digitalRead(config.gpio);
  return config.activeLow ? level == LOW : level == HIGH;
}

}  // namespace

void reconfigure() {
  const std::vector<cfg::InputCfg> &configured = cfg::config().inputs;
  for (size_t i = 0; i < cfg::INPUT_COUNT; i++) {
    InputState &state = g_states[i];

    // Rilascia il pin precedente se è cambiato o se l'ingresso è stato disattivato.
    const bool wanted = i < configured.size() && configured[i].enabled && configured[i].gpio >= 0;
    if (state.configured && (!wanted || state.gpio != configured[i].gpio)) {
      pinMode(state.gpio, INPUT);
      state.configured = false;
      state.gpio = -1;
    }
    if (!wanted) continue;

    const cfg::InputCfg &item = configured[i];
    pinMode(item.gpio, item.pullup ? INPUT_PULLUP : INPUT);
    state.gpio = item.gpio;
    state.configured = true;
    state.lastRaw = readActive(item);
    state.active = state.lastRaw;
    state.changedAt = millis();
    log_i("Ingresso %u su GPIO%d (%s, attivo %s)", static_cast<unsigned>(i + 1), item.gpio,
          item.pullup ? "pull-up" : "senza pull-up", item.activeLow ? "basso" : "alto");
  }
}

void begin() { reconfigure(); }

void loop() {
  const std::vector<cfg::InputCfg> &configured = cfg::config().inputs;
  const uint32_t now = millis();

  for (size_t i = 0; i < cfg::INPUT_COUNT && i < configured.size(); i++) {
    InputState &state = g_states[i];
    if (!state.configured) continue;
    const cfg::InputCfg &item = configured[i];

    const bool raw = readActive(item);
    if (raw != state.lastRaw) {
      state.lastRaw = raw;
      state.changedAt = now;
      continue;  // attende che la lettura si stabilizzi (antirimbalzo)
    }
    if (raw == state.active) continue;
    if ((now - state.changedAt) < item.debounceMs) continue;

    state.active = raw;
    if (!raw) continue;  // si avvia sul fronte di attivazione, non al rilascio

    state.triggerCount++;
    state.lastTriggerAt = now;
    // L'evento va al portale a prescindere dalla sequenza collegata: se l'ingresso
    // ha la notifica attiva, sara' il cloud a inviarla.
    mqtt::publishInputEvent(i);
    if (!item.sequenceId.length()) {
      log_i("Ingresso %u attivato: nessuna sequenza assegnata", static_cast<unsigned>(i + 1));
      continue;
    }
    String error;
    const String source = String(F("ingresso ")) + (i + 1);
    if (!sequences::start(item.sequenceId, source, error)) {
      log_w("Ingresso %u: %s", static_cast<unsigned>(i + 1), error.c_str());
    }
  }
}

void statusJson(JsonArray out) {
  const std::vector<cfg::InputCfg> &configured = cfg::config().inputs;
  for (size_t i = 0; i < cfg::INPUT_COUNT && i < configured.size(); i++) {
    const cfg::InputCfg &item = configured[i];
    const InputState &state = g_states[i];
    JsonObject entry = out.add<JsonObject>();
    entry["index"] = static_cast<uint32_t>(i);
    entry["id"] = String("input-") + i;
    entry["kind"] = "input";
    entry["name"] = item.name;
    entry["room"] = item.room;
    entry["favorite"] = item.favorite;
    entry["notifyOnChange"] = item.notifyOnChange;
    entry["notifyText"] = item.notifyText;
    entry["enabled"] = item.enabled;
    entry["gpio"] = item.gpio;
    entry["activeLow"] = item.activeLow;
    entry["pullup"] = item.pullup;
    entry["debounceMs"] = item.debounceMs;
    entry["sequenceId"] = item.sequenceId;
    entry["active"] = state.configured && state.active;
    entry["triggerCount"] = state.triggerCount;
    entry["lastTriggerAgoMs"] = state.lastTriggerAt ? millis() - state.lastTriggerAt : 0;
  }
}

}  // namespace inputs
