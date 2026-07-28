#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Otto ingressi digitali: un fronte attivo avvia la sequenza assegnata.
// I pin e i parametri elettrici si impostano in Sistema, la sequenza da avviare
// si sceglie dal Controllo.
namespace inputs {

void begin();
void loop();

// Riconfigura i pin dopo un cambio di configurazione.
void reconfigure();

void statusJson(JsonArray out);

}  // namespace inputs
