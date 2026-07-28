#include <Arduino.h>

#include "board_pins.h"
#include "bus.h"
#include "devices.h"
#include "mqtt_bridge.h"
#include "net_manager.h"
#include "schedules.h"
#include "sequences.h"
#include "settings.h"
#include "web_ui.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("========================================"));
  Serial.printf("  Sheltr ESP %s (%s)\n", SHELTR_FW_VERSION, SHELTR_BOARD_NAME);
  Serial.println(F("  Gateway protocollo Sheltr 1.6"));
  Serial.println(F("========================================"));

  cfg::begin();
  Serial.printf("Dispositivo: %s (%s)\n", cfg::config().device.name.c_str(),
                cfg::config().device.id.c_str());

  devices::begin();
  net::begin();
  webserver::begin();
  mqtt::begin();

  Serial.printf("Interfaccia locale: http://%s.local\n", cfg::config().network.hostname.c_str());
}

void loop() {
  net::loop();
  webserver::loop();
  mqtt::loop();
  devices::loop();
  schedules::loop();
  sequences::loop();
  delay(2);
}
