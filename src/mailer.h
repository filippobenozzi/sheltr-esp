#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Invio di email dal gateway.
//
// Serve quando il portale non c'e': se il gateway e' collegato a Sheltr Cloud le
// notifiche le manda il portale (ha gli indirizzi di tutti gli utenti), altrimenti
// — purche' ci sia una connessione a internet — le manda direttamente il gateway
// agli indirizzi configurati qui.
//
// L'invio avviene in un task dedicato: una connessione SMTP con TLS puo' durare
// qualche secondo e il loop principale deve restare libero per bus, web e MQTT.
namespace mailer {

void begin();
// Da chiamare dopo un cambio di configurazione.
void reload();

// Mette un messaggio in coda. Ritorna false se la coda e' piena o se l'invio non
// e' configurato. `to` vuoto = i destinatari della configurazione.
bool queue(const String &subject, const String &body, const String &to = "");

// Invio immediato e bloccante: solo per la prova dalla pagina Sistema, che deve
// poter riportare l'errore esatto del server di posta.
bool sendNow(const String &subject, const String &body, const String &to, String &error);

bool configured();
void statusJson(JsonObject out);

}  // namespace mailer
