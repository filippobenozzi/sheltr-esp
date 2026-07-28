#include "sequences.h"

#include "devices.h"
#include "settings.h"

namespace sequences {

namespace {

struct Runner {
  String id;
  String source;
  size_t stepIndex = 0;
  uint32_t nextStepAt = 0;
  uint32_t startedAt = 0;
};

Runner g_runners[MAX_CONCURRENT];
uint32_t g_runCount = 0;
String g_lastFinishedId;
String g_lastError;

Runner *findRunner(const String &id) {
  for (Runner &runner : g_runners) {
    if (runner.id == id) return &runner;
  }
  return nullptr;
}

Runner *freeRunner() {
  for (Runner &runner : g_runners) {
    if (!runner.id.length()) return &runner;
  }
  return nullptr;
}

void release(Runner &runner) {
  runner.id = "";
  runner.source = "";
  runner.stepIndex = 0;
  runner.nextStepAt = 0;
}

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

void advance(Runner &runner) {
  const cfg::Sequence *sequence = cfg::findSequence(runner.id);
  if (sequence == nullptr) {  // configurazione cambiata durante l'esecuzione
    release(runner);
    return;
  }
  if (runner.stepIndex >= sequence->steps.size()) {
    log_i("Sequenza '%s' completata (%s)", sequence->name.c_str(), runner.source.c_str());
    g_lastFinishedId = runner.id;
    g_runCount++;
    release(runner);
    return;
  }

  const cfg::SequenceStep &step = sequence->steps[runner.stepIndex];
  String error;
  if (!runStep(step, error)) {
    g_lastError = sequence->name + " · passo " + (runner.stepIndex + 1) + ": " + error;
    log_w("Sequenza '%s' interrotta al passo %u: %s", sequence->name.c_str(),
          static_cast<unsigned>(runner.stepIndex + 1), error.c_str());
    g_lastFinishedId = runner.id;
    release(runner);
    return;
  }

  runner.stepIndex++;
  runner.nextStepAt = millis() + static_cast<uint32_t>(step.delaySec) * 1000UL;
}

}  // namespace

bool start(const String &sequenceId, const String &source, String &error) {
  const cfg::Sequence *sequence = cfg::findSequence(sequenceId);
  if (sequence == nullptr) {
    error = F("Sequenza non trovata");
    return false;
  }
  if (sequence->steps.empty()) {
    error = F("La sequenza non ha passi");
    return false;
  }
  if (findRunner(sequenceId) != nullptr) {
    error = F("Sequenza già in esecuzione");
    return false;
  }
  Runner *runner = freeRunner();
  if (runner == nullptr) {
    error = F("Troppe sequenze in esecuzione");
    return false;
  }

  runner->id = sequenceId;
  runner->source = source.length() ? source : String(F("interfaccia"));
  runner->stepIndex = 0;
  runner->nextStepAt = millis();
  runner->startedAt = millis();
  log_i("Sequenza '%s' avviata da %s (%u passi)", sequence->name.c_str(), runner->source.c_str(),
        static_cast<unsigned>(sequence->steps.size()));
  return true;
}

bool startByBusTrigger(uint16_t trigger, String &error) {
  const cfg::Sequence *sequence = cfg::findSequenceByBusTrigger(trigger);
  if (sequence == nullptr) {
    error = String(F("Nessuna sequenza con trigger bus ")) + trigger;
    return false;
  }
  char label[16];
  snprintf(label, sizeof(label), "bus AA%02X", trigger);
  return start(sequence->id, label, error);
}

void stop(const String &sequenceId) {
  Runner *runner = findRunner(sequenceId);
  if (runner == nullptr) return;
  log_w("Sequenza '%s' interrotta", sequenceId.c_str());
  release(*runner);
}

void stopAll() {
  for (Runner &runner : g_runners) release(runner);
}

bool running(const String &sequenceId) { return findRunner(sequenceId) != nullptr; }

bool anyRunning() {
  for (const Runner &runner : g_runners) {
    if (runner.id.length()) return true;
  }
  return false;
}

void loop() {
  const uint32_t now = millis();
  for (Runner &runner : g_runners) {
    if (!runner.id.length()) continue;
    if (static_cast<int32_t>(now - runner.nextStepAt) < 0) continue;
    advance(runner);
  }
}

void statusJson(JsonObject out) {
  out["running"] = anyRunning();
  out["runCount"] = g_runCount;
  out["lastFinished"] = g_lastFinishedId;
  out["lastError"] = g_lastError;
  out["slots"] = static_cast<uint32_t>(MAX_CONCURRENT);

  JsonArray active = out["active"].to<JsonArray>();
  for (const Runner &runner : g_runners) {
    if (!runner.id.length()) continue;
    JsonObject item = active.add<JsonObject>();
    item["id"] = runner.id;
    item["source"] = runner.source;
    item["step"] = static_cast<uint32_t>(runner.stepIndex);
    item["elapsedMs"] = millis() - runner.startedAt;
    const cfg::Sequence *sequence = cfg::findSequence(runner.id);
    if (sequence != nullptr) {
      item["name"] = sequence->name;
      item["steps"] = static_cast<uint32_t>(sequence->steps.size());
    }
  }
}

}  // namespace sequences
