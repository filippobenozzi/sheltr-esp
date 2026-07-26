#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Due client MQTT indipendenti:
//  - "locale": discovery Home Assistant + stati + comandi (rete di casa)
//  - "cloud": profilo Sheltr Cloud (config retained, frame protocollo su /cmd, risposte su /pub)
namespace mqtt {

void begin();
void loop();

// Da chiamare dopo un cambio di configurazione: riavvia i client e ripubblica discovery/config.
void reload();

// Forza la pubblicazione degli stati (usata dopo i comandi dalla UI).
void publishStates();

bool localConnected();
bool cloudConnected();
void statusJson(JsonObject out);

}  // namespace mqtt
