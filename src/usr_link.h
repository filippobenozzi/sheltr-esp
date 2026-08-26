#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Collegamento verso il modulo USR DR154, usato quando il gateway non ha rete
// propria: il gateway scrive sulla seriale (RS485), il modulo pubblica quei byte
// sul topic MQTT del portale e gli consegna sulla seriale quello che arriva
// dall'altro topic. Qui si gestiscono solo i byte: l'incapsulamento con il nome
// del sotto-topic sta in link_codec.
namespace usr {

using MessageHandler = void (*)(const String &sub, const uint8_t *payload, size_t length);

void begin(MessageHandler handler);
// Da chiamare dopo un cambio di configurazione (pin, baud, trasporto).
void reload();
void loop();

// Manda un messaggio al portale attraverso il modulo. `sub` usa gli stessi nomi
// del collegamento diretto: pub, config, event, bridge/status, claim.
bool send(const String &sub, const uint8_t *payload, size_t length);
bool send(const String &sub, const String &payload);

// Il collegamento e' considerato vivo se il portale ha risposto di recente: e'
// l'unico modo per saperlo, perche' la seriale da sola non dice se il modulo sia
// davvero connesso al broker.
bool alive();
bool enabled();
void statusJson(JsonObject out);

}  // namespace usr
