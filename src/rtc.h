#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

// Orologio hardware DS3231 su I2C.
//
// All'avvio l'ora di sistema viene presa dall'RTC (utile quando non c'è internet);
// quando NTP sincronizza, l'ora viene riscritta sull'RTC così resta aggiornata
// anche dopo un'interruzione di corrente.
namespace rtc {

void begin();
void loop();

// Riapre il bus I2C dopo un cambio di configurazione.
void reconfigure();

bool enabled();
bool present();    // il chip risponde sull'indirizzo I2C
bool timeValid();  // l'oscillatore non si è mai fermato

// Legge l'ora dall'RTC e la imposta come ora di sistema.
bool syncFromRtc(String &error);

// Scrive l'ora di sistema sull'RTC.
bool syncToRtc(String &error);

// Imposta manualmente data e ora (RTC + sistema), formato "YYYY-MM-DDTHH:MM[:SS]".
bool setDateTime(const String &isoLocalTime, String &error);

void statusJson(JsonObject out);

}  // namespace rtc
