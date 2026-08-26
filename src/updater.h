#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Aggiornamento firmware dalle release GitHub del progetto.
//
// Ogni push su main pubblica una release con i binari: il gateway confronta il
// tag della release più recente con quello con cui è stato compilato
// (SHELTR_FW_RELEASE) e, se differisce, propone il download.
//
// Il download scrive direttamente nella partizione OTA da un task dedicato, così
// l'interfaccia resta raggiungibile e può mostrare l'avanzamento.
namespace updater {

void begin();
void loop();

// Interroga GitHub (bloccante, ~1-3 s). Aggiorna lo stato interno.
bool check(String &error);

// Avvia il download e l'installazione in background.
bool startInstall(String &error);

// True mentre è in corso un controllo, un download o un'installazione.
bool busy();

// Tag della release installata ("dev" per le build locali).
String installedRelease();

void statusJson(JsonObject out);

}  // namespace updater
