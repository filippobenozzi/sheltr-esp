#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Due collegamenti indipendenti:
//  - MQTT "locale": discovery Home Assistant + stati + comandi (rete di casa)
//  - Sheltr Cloud: connessione al gateway del portale (TCP diretto o via DR154, vedi
//    cloud_link) con configurazione, frame del protocollo, stati, eventi e impostazioni
namespace mqtt {

void begin();
void loop();

// Da chiamare dopo un cambio di configurazione: riavvia i client e ripubblica discovery/config.
void reload();

// Forza la pubblicazione degli stati (usata dopo i comandi dalla UI).
void publishStates();

// Notifica al portale lo scatto di un ingresso: e' il cloud a inviare la
// notifica vera (push/Telegram), il gateway pubblica solo l'evento.
void publishInputEvent(size_t index);

// Ripubblica subito la configurazione sul portale. Da chiamare dopo una modifica
// locale che il cloud rispecchia (preferiti, ingressi): senza questo il portale
// resterebbe con la configurazione precedente fino alla prossima riconnessione.
void publishConfig();

bool localConnected();
bool cloudConnected();
void statusJson(JsonObject out);

}  // namespace mqtt
