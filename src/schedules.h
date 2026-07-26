#pragma once

#include <Arduino.h>

// Profili orari (stessa semantica del gateway Sheltr):
//  - luci e tapparelle: azione puntuale all'orario indicato, nei giorni scelti
//  - termostati: fasce orarie con setpoint e stagione, riapplicate se lo stato diverge
namespace schedules {

void loop();
uint32_t lastRunAt();
uint32_t appliedCount();

}  // namespace schedules
