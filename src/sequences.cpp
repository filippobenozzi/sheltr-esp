#include "sequences.h"

#include "devices.h"
#include "settings.h"

namespace sequences {

namespace {

String g_runningId;
size_t g_stepIndex = 0;
uint32_t g_nextStepAt = 0;
uint32_t g_startedAt = 0;
String g_lastError;
String g_lastFinishedId;
uint32_t g_runCount = 0;

// Esegue l'azione di un passo sul canale indicato.
bool runStep(const cfg::SequenceStep &step, String &error) {
  if (!step.channelId.length()) return true;  // passo di sola attesa

  devices::Entity entity;
  if (!devices::entityById(step.channelId, entity)) {
    error = String(F("canale non trovato: ")) + step.channelId;
    return false;
  }

  devices::CommandResult result;
  const String &action = step.action;

  if (entity.kind == "light") {
    result = devices::commandLight(step.channelId, action.length() ? action : "on");
  } else if (entity.kind == "dimmer") {
    if (action == "level" || action == "set" || step.hasValue) {
      result = devices::commandDimmer(step.channelId, "set",
                                      constrain(static_cast<int>(step.value), 0, 9));
    } else {
      result = devices::commandDimmer(step.channelId, action.length() ? action : "on", -1);
    }
  } else if (entity.kind == "shutter") {
    result = devices::commandShutter(step.channelId, action.length() ? action : "stop");
  } else {
    if (action == "mode" && step.mode.length()) {
      result = devices::commandThermostat(step.channelId, false, 0, step.mode, 1);
    } else if (action == "off") {
      result = devices::commandThermostat(step.channelId, false, 0, "", 0);
    } else if (action == "on") {
      result = devices::commandThermostat(step.channelId, false, 0, step.mode, 1);
    } else {
      result = devices::commandThermostat(step.channelId, true, step.value, step.mode, 1);
    }
  }

  if (!result.ok) {
    error = result.error;
    return false;
  }
  return true;
}

}  // namespace

bool start(const String &sequenceId, String &error) {
  const cfg::Sequence *sequence = cfg::findSequence(sequenceId);
  if (sequence == nullptr) {
    error = F("Sequenza non trovata");
    return false;
  }
  if (sequence->steps.empty()) {
    error = F("La sequenza non ha passi");
    return false;
  }
  if (g_runningId.length()) {
    error = String(F("Sequenza già in esecuzione: ")) + g_runningId;
    return false;
  }
  g_runningId = sequenceId;
  g_stepIndex = 0;
  g_nextStepAt = millis();
  g_startedAt = millis();
  g_lastError = "";
  log_i("Sequenza '%s' avviata (%u passi)", sequenceId.c_str(),
        static_cast<unsigned>(sequence->steps.size()));
  return true;
}

void stop() {
  if (!g_runningId.length()) return;
  log_w("Sequenza '%s' interrotta", g_runningId.c_str());
  g_runningId = "";
  g_stepIndex = 0;
}

void loop() {
  if (!g_runningId.length()) return;
  if (static_cast<int32_t>(millis() - g_nextStepAt) < 0) return;

  const cfg::Sequence *sequence = cfg::findSequence(g_runningId);
  if (sequence == nullptr) {  // configurazione cambiata durante l'esecuzione
    stop();
    return;
  }
  if (g_stepIndex >= sequence->steps.size()) {
    log_i("Sequenza '%s' completata", g_runningId.c_str());
    g_lastFinishedId = g_runningId;
    g_runCount++;
    g_runningId = "";
    g_stepIndex = 0;
    return;
  }

  const cfg::SequenceStep &step = sequence->steps[g_stepIndex];
  String error;
  if (!runStep(step, error)) {
    g_lastError = String(F("passo ")) + (g_stepIndex + 1) + ": " + error;
    log_w("Sequenza '%s' fallita al passo %u: %s", g_runningId.c_str(),
          static_cast<unsigned>(g_stepIndex + 1), error.c_str());
    g_lastFinishedId = g_runningId;
    g_runningId = "";
    g_stepIndex = 0;
    return;
  }

  g_stepIndex++;
  g_nextStepAt = millis() + static_cast<uint32_t>(step.delaySec) * 1000UL;
}

bool running() { return g_runningId.length() > 0; }
String runningId() { return g_runningId; }

void statusJson(JsonObject out) {
  out["running"] = running();
  out["id"] = g_runningId;
  out["step"] = static_cast<uint32_t>(g_stepIndex);
  out["runCount"] = g_runCount;
  out["lastFinished"] = g_lastFinishedId;
  out["lastError"] = g_lastError;
  if (running()) out["elapsedMs"] = millis() - g_startedAt;
}

}  // namespace sequences
