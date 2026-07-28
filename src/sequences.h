#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Sequencer: pulsanti virtuali che eseguono una serie di azioni con attese
// intermedie. L'esecuzione è a passi, guidata dal loop principale: nessun
// `delay()` lungo, così web server, MQTT e bus restano reattivi.
namespace sequences {

void loop();

// Avvia una sequenza. Ritorna false se l'id non esiste o è già in esecuzione.
bool start(const String &sequenceId, String &error);

// Ferma la sequenza in corso.
void stop();

bool running();
String runningId();

// Stato per interfaccia e API.
void statusJson(JsonObject out);

}  // namespace sequences
