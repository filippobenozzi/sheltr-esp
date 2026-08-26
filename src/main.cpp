#include <Arduino.h>

#include "board_pins.h"
#include "bus.h"
#include "devices.h"
#include "inputs.h"
#include "metrics.h"
#include "mqtt_bridge.h"
#include "net_manager.h"
#include "rtc.h"
#include "schedules.h"
#include "sequences.h"
#include "settings.h"
#include "updater.h"
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
  rtc::begin();
  inputs::begin();

  // Comandi di scenario inviati spontaneamente sul bus (AA01, AA02, …).
  Bus.onTrigger([](uint16_t trigger) {
    String error;
    if (!sequences::startByBusTrigger(trigger, error)) {
      log_w("Trigger bus AA%02X ignorato: %s", trigger, error.c_str());
    }
  });

  net::begin();
  webserver::begin();
  updater::begin();
  mqtt::begin();

  Serial.printf("Interfaccia locale: http://%s.local\n", cfg::config().network.hostname.c_str());
}

void loop() {
  const uint32_t startedAt = micros();

  net::loop();
  webserver::loop();
  mqtt::loop();
  devices::loop();
  inputs::loop();
  Bus.listen();
  schedules::loop();
  sequences::loop();
  rtc::loop();
  updater::loop();

  metrics::sample(micros() - startedAt);
  delay(2);  // lascia girare il task idle (watchdog) e le attività di rete
}
