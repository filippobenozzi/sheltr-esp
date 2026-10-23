#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Collegamento con il gateway TCP di Sheltr Cloud. Due strade, stesso protocollo:
//  - rete propria (Ethernet/WiFi): connessione TCP diretta al portale (porta 7700);
//  - modulo USR DR154: il gateway scrive sulla seriale (RS485) e il modulo porta
//    quei byte, in TCP, allo stesso gateway.
// Qui si gestiscono solo i byte e la connessione: l'incapsulamento con il nome del
// sotto-topic sta in link_codec, il significato dei messaggi in mqtt_bridge.
namespace cloudlink {

using MessageHandler = void (*)(const String &sub, const uint8_t *payload, size_t length);

void begin(MessageHandler handler);
// Da chiamare dopo un cambio di configurazione (indirizzo, trasporto, pin, baud).
void reload();
void loop();

// Manda un messaggio al portale: pub, config, event, bridge/status, hello, ping.
bool send(const String &sub, const uint8_t *payload, size_t length);
bool send(const String &sub, const String &payload);

// Trasporto pronto: socket TCP collegato oppure seriale verso il modulo aperta.
bool open();
// Cambia a ogni nuova connessione TCP (o apertura della seriale): e' il momento di
// ripresentarsi al portale.
uint32_t session();
// Il portale ha risposto di recente: col modulo e' l'unico modo per sapere che il
// ponte funziona davvero (la seriale da sola non lo dice).
bool alive();
bool enabled();
void statusJson(JsonObject out);

}  // namespace cloudlink
