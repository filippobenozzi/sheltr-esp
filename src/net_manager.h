#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Gestione rete:
//  - Ethernet W5500 (DHCP o IP statico) con dominio locale <hostname>.local
//  - WiFi client con le credenziali salvate
//  - hotspot + captive portal quando non c'è connettività (primo avvio)
namespace net {

void begin();
void loop();

bool ethConnected();
bool wifiConnected();
bool apActive();
bool online();

String ethIp();
String wifiIp();
String apIp();
String apSsid();
String hostname();
String macAddress();
String wifiSsid();
int wifiRssi();

// Avvia/ferma l'hotspot di configurazione.
void startAccessPoint();
void stopAccessPoint();

// Applica nuove credenziali WiFi e riconnette (usato dall'API di provisioning).
bool connectWifi(const String &ssid, const String &password, uint32_t timeoutMs);

// Scansione reti per l'interfaccia di provisioning.
void scanNetworks(JsonArray out);

void statusJson(JsonObject out);

}  // namespace net
