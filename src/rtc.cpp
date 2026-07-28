#include "rtc.h"

#include <Wire.h>
#include <sys/time.h>

#include "settings.h"

namespace rtc {

namespace {

// Registri DS3231
constexpr uint8_t REG_TIME = 0x00;    // secondi, minuti, ore, giorno, data, mese, anno
constexpr uint8_t REG_STATUS = 0x0F;  // bit 7 = Oscillator Stop Flag
constexpr uint8_t REG_TEMP = 0x11;    // temperatura del chip (2 byte)

TwoWire *g_wire = nullptr;
bool g_present = false;
bool g_valid = false;
float g_temperature = 0.0f;
uint32_t g_nextCheckAt = 0;
uint32_t g_lastWriteAt = 0;
bool g_writtenSinceBoot = false;
String g_lastError;

uint8_t bcdToDec(uint8_t value) { return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F)); }
uint8_t decToBcd(uint8_t value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }

bool readRegisters(uint8_t reg, uint8_t *buffer, size_t length) {
  if (g_wire == nullptr) return false;
  const uint8_t address = cfg::config().rtc.address;
  g_wire->beginTransmission(address);
  g_wire->write(reg);
  if (g_wire->endTransmission(false) != 0) return false;
  if (g_wire->requestFrom(address, static_cast<uint8_t>(length)) != length) return false;
  for (size_t i = 0; i < length; i++) buffer[i] = g_wire->read();
  return true;
}

bool writeRegisters(uint8_t reg, const uint8_t *buffer, size_t length) {
  if (g_wire == nullptr) return false;
  g_wire->beginTransmission(cfg::config().rtc.address);
  g_wire->write(reg);
  for (size_t i = 0; i < length; i++) g_wire->write(buffer[i]);
  return g_wire->endTransmission() == 0;
}

bool readTime(struct tm &out) {
  uint8_t data[7] = {0};
  if (!readRegisters(REG_TIME, data, sizeof(data))) return false;
  out.tm_sec = bcdToDec(data[0] & 0x7F);
  out.tm_min = bcdToDec(data[1] & 0x7F);
  out.tm_hour = bcdToDec(data[2] & 0x3F);  // il firmware usa sempre il formato 24 ore
  out.tm_mday = bcdToDec(data[4] & 0x3F);
  out.tm_mon = bcdToDec(data[5] & 0x1F) - 1;
  out.tm_year = bcdToDec(data[6]) + ((data[5] & 0x80) ? 200 : 100);
  out.tm_isdst = -1;
  return out.tm_mon >= 0 && out.tm_mon <= 11 && out.tm_mday >= 1 && out.tm_mday <= 31;
}

bool writeTime(const struct tm &value) {
  uint8_t data[7];
  data[0] = decToBcd(value.tm_sec);
  data[1] = decToBcd(value.tm_min);
  data[2] = decToBcd(value.tm_hour);  // bit 6 = 0 -> formato 24 ore
  data[3] = decToBcd(value.tm_wday + 1);
  data[4] = decToBcd(value.tm_mday);
  const bool century = value.tm_year >= 200;
  data[5] = decToBcd(value.tm_mon + 1) | (century ? 0x80 : 0x00);
  data[6] = decToBcd(value.tm_year % 100);
  if (!writeRegisters(REG_TIME, data, sizeof(data))) return false;

  // Azzera l'Oscillator Stop Flag: da ora l'ora conservata è valida.
  uint8_t status = 0;
  if (readRegisters(REG_STATUS, &status, 1)) {
    status &= ~0x80;
    writeRegisters(REG_STATUS, &status, 1);
  }
  g_valid = true;
  return true;
}

void refreshStatus() {
  uint8_t status = 0;
  g_present = readRegisters(REG_STATUS, &status, 1);
  if (!g_present) {
    g_valid = false;
    return;
  }
  g_valid = (status & 0x80) == 0;

  uint8_t temp[2] = {0};
  if (readRegisters(REG_TEMP, temp, sizeof(temp))) {
    g_temperature = static_cast<int8_t>(temp[0]) + ((temp[1] >> 6) * 0.25f);
  }
}

bool systemTimeValid() { return time(nullptr) > 1700000000; }

}  // namespace

void reconfigure() {
  const cfg::RtcCfg &settings = cfg::config().rtc;
  if (!settings.enabled || settings.sda < 0 || settings.scl < 0) {
    g_wire = nullptr;
    g_present = false;
    g_valid = false;
    return;
  }
  g_wire = &Wire;
  g_wire->begin(settings.sda, settings.scl, 100000);
  g_wire->setTimeOut(50);
  refreshStatus();
  log_i("RTC DS3231: SDA=%d SCL=%d addr=0x%02X presente=%d ora valida=%d", settings.sda,
        settings.scl, settings.address, g_present, g_valid);
}

void begin() {
  reconfigure();
  if (!g_present || !g_valid) return;

  String error;
  if (syncFromRtc(error)) {
    log_i("Ora presa dall'RTC");
  } else {
    log_w("Lettura RTC fallita: %s", error.c_str());
  }
}

void loop() {
  if (g_wire == nullptr) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextCheckAt) < 0) return;
  g_nextCheckAt = now + 60000;

  refreshStatus();
  if (!g_present) return;

  // Primo aggancio NTP (o riallineamento orario): riporta l'ora sull'RTC.
  const bool hourElapsed = g_lastWriteAt == 0 || (now - g_lastWriteAt) > 3600000UL;
  if (systemTimeValid() && (!g_writtenSinceBoot || hourElapsed)) {
    String error;
    if (syncToRtc(error)) {
      g_writtenSinceBoot = true;
      g_lastWriteAt = now;
    }
  }
}

bool enabled() { return cfg::config().rtc.enabled; }
bool present() { return g_present; }
bool timeValid() { return g_valid; }

bool syncFromRtc(String &error) {
  if (g_wire == nullptr) {
    error = F("RTC non abilitato");
    return false;
  }
  refreshStatus();
  if (!g_present) {
    error = F("DS3231 non risponde: controlla i collegamenti I2C");
    return false;
  }
  if (!g_valid) {
    error = F("L'RTC ha perso l'ora (batteria scarica): impostala prima");
    return false;
  }

  struct tm parts = {};
  if (!readTime(parts)) {
    error = F("Lettura RTC non riuscita");
    return false;
  }
  const time_t seconds = mktime(&parts);  // il DS3231 conserva l'ora locale
  if (seconds <= 0) {
    error = F("Data letta dall'RTC non valida");
    return false;
  }
  struct timeval tv = {seconds, 0};
  settimeofday(&tv, nullptr);
  return true;
}

bool syncToRtc(String &error) {
  if (g_wire == nullptr) {
    error = F("RTC non abilitato");
    return false;
  }
  if (!systemTimeValid()) {
    error = F("Ora di sistema non sincronizzata");
    return false;
  }
  time_t now = time(nullptr);
  struct tm parts;
  localtime_r(&now, &parts);
  if (!writeTime(parts)) {
    error = F("Scrittura RTC non riuscita");
    return false;
  }
  g_writtenSinceBoot = true;
  g_lastWriteAt = millis();
  return true;
}

bool setDateTime(const String &isoLocalTime, String &error) {
  struct tm parts = {};
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  const int parsed = sscanf(isoLocalTime.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour,
                            &minute, &second);
  if (parsed < 5) {
    error = F("Formato data non valido (atteso YYYY-MM-DDTHH:MM)");
    return false;
  }
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = parsed >= 6 ? second : 0;
  parts.tm_isdst = -1;

  const time_t seconds = mktime(&parts);
  if (seconds <= 0) {
    error = F("Data non valida");
    return false;
  }
  struct timeval tv = {seconds, 0};
  settimeofday(&tv, nullptr);

  if (g_wire != nullptr) {
    struct tm normalized;
    localtime_r(&seconds, &normalized);
    if (!writeTime(normalized)) {
      error = F("Ora di sistema impostata, ma scrittura sull'RTC non riuscita");
      return false;
    }
    g_writtenSinceBoot = true;
    g_lastWriteAt = millis();
  }
  return true;
}

void statusJson(JsonObject out) {
  const cfg::RtcCfg &settings = cfg::config().rtc;
  out["enabled"] = settings.enabled;
  out["sda"] = settings.sda;
  out["scl"] = settings.scl;
  out["address"] = settings.address;
  out["present"] = g_present;
  out["timeValid"] = g_valid;
  out["temperature"] = roundf(g_temperature * 10.0f) / 10.0f;
  out["lastError"] = g_lastError;

  if (g_present && g_valid) {
    struct tm parts = {};
    if (readTime(parts)) {
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);
      out["time"] = buffer;
    }
  }
}

}  // namespace rtc
