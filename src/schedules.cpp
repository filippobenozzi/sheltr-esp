#include "schedules.h"

#include <time.h>

#include <map>

#include "devices.h"
#include "settings.h"

namespace schedules {

namespace {

constexpr uint32_t INTERVAL_MS = 20000;

uint32_t g_nextRunAt = 15000;
uint32_t g_lastRunAt = 0;
uint32_t g_applied = 0;
std::map<String, uint32_t> g_lastSwitchRun;  // chiave profilo -> minuto assoluto già eseguito

int minuteOfDay(const String &value) {
  const int colon = value.indexOf(':');
  if (colon <= 0) return 0;
  return value.substring(0, colon).toInt() * 60 + value.substring(colon + 1).toInt();
}

bool dayEnabled(uint8_t mask, int weekday) {  // weekday 1 = lunedì
  if (weekday < 1 || weekday > 7) return false;
  return (mask & (1 << (weekday - 1))) != 0;
}

void applySwitchProfiles(int nowMinute, int weekday, uint32_t minuteStamp) {
  for (const cfg::Board &board : cfg::config().boards) {
    if (board.kind != "light" && board.kind != "shutter") continue;
    for (const cfg::Channel &channel : board.channels) {
      if (!channel.profile.enabled) continue;
      const String id = cfg::entityId(board.id, channel.channel);
      for (size_t index = 0; index < channel.profile.entries.size(); index++) {
        const cfg::ProfileEntry &entry = channel.profile.entries[index];
        if (!dayEnabled(entry.days, weekday)) continue;
        if (minuteOfDay(entry.time) != nowMinute) continue;
        const String key = id + "#" + String(index);
        auto found = g_lastSwitchRun.find(key);
        if (found != g_lastSwitchRun.end() && found->second == minuteStamp) continue;

        devices::CommandResult result;
        if (board.kind == "light") {
          result = devices::commandLight(id, entry.action == "on" ? "on" : "off");
        } else {
          result = devices::commandShutter(id, entry.action == "up" ? "up" : "down");
        }
        g_lastSwitchRun[key] = minuteStamp;
        if (result.ok) {
          g_applied++;
        } else {
          log_w("Profilo %s %s fallito: %s", board.kind.c_str(), id.c_str(), result.error.c_str());
        }
      }
    }
  }
}

void thermostatTarget(const cfg::Profile &profile, int nowMinute, int weekday, float &setpoint,
                      String &mode) {
  setpoint = 5.0f;
  mode = "winter";
  const int previousWeekday = weekday <= 1 ? 7 : weekday - 1;
  for (const cfg::ProfileEntry &entry : profile.entries) {
    const int start = minuteOfDay(entry.from);
    const int end = minuteOfDay(entry.to);
    bool match = false;
    if (start == end) {
      match = dayEnabled(entry.days, weekday);
    } else if (start < end) {
      match = dayEnabled(entry.days, weekday) && nowMinute >= start && nowMinute < end;
    } else if (nowMinute >= start) {
      match = dayEnabled(entry.days, weekday);
    } else {
      match = dayEnabled(entry.days, previousWeekday);
    }
    if (!match) continue;
    setpoint = constrain(roundf(entry.setpoint * 2.0f) / 2.0f, 5.0f, 30.0f);
    mode = entry.mode == "summer" ? "summer" : "winter";
    return;
  }
}

void applyThermostatProfiles(int nowMinute, int weekday) {
  for (const cfg::Board &board : cfg::config().boards) {
    if (board.kind != "thermostat") continue;
    for (const cfg::Channel &channel : board.channels) {
      if (!channel.profile.enabled) continue;
      const String id = cfg::entityId(board.id, channel.channel);
      float target = 21.0f;
      String mode = "winter";
      thermostatTarget(channel.profile, nowMinute, weekday, target, mode);

      const devices::ThermostatState *state = devices::thermostatState(id);
      const bool needSetpoint = state == nullptr || fabsf(state->setpoint - target) > 0.24f;
      const bool needMode = state == nullptr || state->mode != mode;
      if (!needSetpoint && !needMode) continue;

      const devices::CommandResult result = devices::commandThermostat(id, true, target, mode, -1);
      if (result.ok) {
        g_applied++;
      } else {
        log_w("Profilo termostato %s fallito: %s", id.c_str(), result.error.c_str());
      }
    }
  }
}

}  // namespace

void loop() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextRunAt) < 0) return;
  g_nextRunAt = now + INTERVAL_MS;
  if (!devices::timeSynced()) return;

  time_t rawTime = time(nullptr);
  struct tm timeinfo;
  localtime_r(&rawTime, &timeinfo);
  const int nowMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  const int weekday = timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday;  // 1 = lunedì
  const uint32_t minuteStamp = static_cast<uint32_t>(rawTime / 60);

  applySwitchProfiles(nowMinute, weekday, minuteStamp);
  applyThermostatProfiles(nowMinute, weekday);
  g_lastRunAt = now;
}

uint32_t lastRunAt() { return g_lastRunAt; }
uint32_t appliedCount() { return g_applied; }

}  // namespace schedules
