#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Statistiche di carico del gateway.
//
// Il core Arduino compila FreeRTOS senza CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS,
// quindi le percentuali per core non sono disponibili. Misuriamo invece il tempo
// che il task principale passa a lavorare rispetto al tempo trascorso: è la
// grandezza che conta davvero qui, perché una transazione lenta sul bus è ciò che
// rallenta interfaccia, MQTT e profili orari.
namespace metrics {

// Da chiamare a ogni iterazione del loop, con la durata del lavoro svolto.
void sample(uint32_t busyMicros);

void statusJson(JsonObject out);

}  // namespace metrics
