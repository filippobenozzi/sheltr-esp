#include "notifier.h"

#include <map>

#include "devices.h"
#include "mailer.h"
#include "mqtt_bridge.h"
#include "net_manager.h"
#include "settings.h"

namespace notifier {

namespace {

// Stato conosciuto di ogni canale con la notifica attiva, per capire cosa e'
// cambiato. Viene tenuto aggiornato anche mentre il portale e' collegato: cosi',
// se il collegamento cade, non parte una raffica di email per differenze vecchie.
std::map<String, String> g_known;
bool g_primed = false;
uint32_t g_lastRevision = 0;
uint32_t g_nextCheckAt = 0;

// Minimo fra due controlli: i comandi arrivano a raffica (una sequenza tocca piu'
// canali) e vale la pena raggrupparli in una sola email.
constexpr uint32_t CHECK_INTERVAL_MS = 2500;

// Il portale c'e' e ci parla: le notifiche le manda lui.
bool cloudHandlesNotifications() {
  const cfg::CloudCfg &cloud = cfg::config().cloud;
  return cloud.enabled && mqtt::cloudConnected();
}

String describe(const cfg::Board &board, const String &entityId) {
  if (board.kind == "light") {
    const devices::LightState *state = devices::lightState(entityId);
    if (state == nullptr || state->isOn < 0) return "";
    return state->isOn ? "ACCESA" : "SPENTA";
  }
  if (board.kind == "dimmer") {
    const devices::DimmerState *state = devices::dimmerState(entityId);
    if (state == nullptr || state->isOn < 0) return "";
    return state->level > 0 ? (String("livello ") + state->level) : String("SPENTO");
  }
  if (board.kind == "thermostat") {
    const devices::ThermostatState *state = devices::thermostatState(entityId);
    if (state == nullptr || state->isOn < 0) return "";
    if (!state->isOn) return "SPENTO";
    return String("setpoint ") + String(state->setpoint, 1) + "°C";
  }
  return "";  // tapparelle: non hanno uno stato stabile da confrontare
}

String label(const cfg::Channel &channel) {
  const String room = cfg::cleanText(channel.room, "");
  if (room.length() && room != "Senza stanza") return channel.name + " (" + room + ")";
  return channel.name;
}

}  // namespace

void begin() {
  mailer::begin();
  g_known.clear();
  g_primed = false;
}

void loop() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextCheckAt) < 0) return;
  g_nextCheckAt = now + CHECK_INTERVAL_MS;

  const uint32_t revision = devices::stateRevision();
  if (g_primed && revision == g_lastRevision) return;
  g_lastRevision = revision;

  // Il portale se ne occupa lui: qui si tiene solo memoria di com'e' lo stato.
  const bool silent = !g_primed || cloudHandlesNotifications() || !mailer::configured() || !net::online();

  std::vector<String> changes;
  for (const cfg::Board &board : cfg::config().boards) {
    for (const cfg::Channel &channel : board.channels) {
      if (!channel.notifyOnChange) continue;
      const String entityId = cfg::entityId(board.id, channel.channel);
      const String state = describe(board, entityId);
      if (!state.length()) continue;  // stato ancora ignoto: niente da dire
      auto found = g_known.find(entityId);
      const bool changed = found == g_known.end() ? false : found->second != state;
      g_known[entityId] = state;
      if (changed && !silent) changes.push_back(label(channel) + ": " + state);
    }
  }
  g_primed = true;
  if (changes.empty()) return;

  String body;
  for (const String &line : changes) {
    body += line;
    body += "\r\n";
  }
  body += "\r\n";
  body += cfg::config().device.name;
  const String subject = changes.size() == 1
                             ? changes[0]
                             : (String(changes.size()) + " cambiamenti di stato");
  mailer::queue(subject, body);
}

void inputTriggered(size_t index) {
  if (cloudHandlesNotifications()) return;  // lo notifica il portale
  if (!mailer::configured() || !net::online()) return;
  const std::vector<cfg::InputCfg> &inputs = cfg::config().inputs;
  if (index >= inputs.size()) return;
  const cfg::InputCfg &item = inputs[index];
  if (!item.notifyOnChange) return;

  const String room = cfg::cleanText(item.room, "");
  const String name = (room.length() && room != "Senza stanza") ? (item.name + " (" + room + ")") : item.name;
  const String text = item.notifyText.length() ? item.notifyText : (name + ": attivato");
  mailer::queue(text, text + "\r\n\r\n" + cfg::config().device.name);
}

}  // namespace notifier
