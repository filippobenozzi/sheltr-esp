#pragma once

#include <Arduino.h>

// Server HTTP locale: interfaccia di controllo, API compatibili con Sheltr Cloud,
// provisioning WiFi (captive portal) e aggiornamento firmware via browser.
namespace webserver {

void begin();
void loop();

}  // namespace webserver
