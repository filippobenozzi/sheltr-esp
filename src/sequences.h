#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Sequencer: pulsanti virtuali che eseguono una serie di azioni con attese
// intermedie. L'esecuzione è a passi, guidata dal loop principale: nessun
// `delay()` lungo, così web server, MQTT e bus restano reattivi.
//
// Più sequenze possono essere in esecuzione contemporaneamente: ognuna ha il
// proprio passo corrente e il proprio timer. Sul bus i comandi restano
// comunque serializzati dal mutex del trasporto.
namespace sequences {

constexpr size_t MAX_CONCURRENT = 8;

void loop();

// Avvia una sequenza. `source` compare nei log e nello stato ("interfaccia",
// "orario", "ingresso 3", "bus AA01"…). Ritorna false se l'id non esiste, se la
// sequenza è già in corso o se non ci sono slot liberi.
bool start(const String &sequenceId, const String &source, String &error);

// Avvio da comando bus: cerca la sequenza con quel numero di trigger.
bool startByBusTrigger(uint16_t trigger, String &error);

void stop(const String &sequenceId);
void stopAll();

bool running(const String &sequenceId);
bool anyRunning();

void statusJson(JsonObject out);

}  // namespace sequences
