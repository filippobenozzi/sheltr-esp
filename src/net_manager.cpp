#include "net_manager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>

#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 0, 0)
#include <ETHClass2.h>
#define ETH ETH2
#else
#include <ETH.h>
#endif

#include "board_pins.h"
#include "settings.h"

namespace net {

namespace {

DNSServer g_dns;
bool g_ethConnected = false;
bool g_wifiConnected = false;
bool g_apActive = false;
bool g_mdnsStarted = false;
bool g_ntpConfigured = false;
uint32_t g_bootAt = 0;
uint32_t g_nextWifiRetryAt = 0;
uint32_t g_apStopAt = 0;  // spegnimento ritardato dell'hotspot dopo il provisioning
String g_apSsid;

IPAddress parseIp(const String &value) {
  IPAddress address;
  if (!value.length() || !address.fromString(value)) return IPAddress(0, 0, 0, 0);
  return address;
}

void applyStaticConfig() {
  const cfg::NetworkCfg &network = cfg::config().network;
  if (network.dhcp) return;
  const IPAddress ip = parseIp(network.ip);
  const IPAddress gateway = parseIp(network.gateway);
  const IPAddress subnet = parseIp(network.subnet.length() ? network.subnet : "255.255.255.0");
  const IPAddress dns1 = parseIp(network.dns1.length() ? network.dns1 : network.gateway);
  const IPAddress dns2 = parseIp(network.dns2);
  if (static_cast<uint32_t>(ip) == 0) return;
  if (cfg::config().network.ethEnabled) ETH.config(ip, gateway, subnet, dns1, dns2);
  WiFi.config(ip, gateway, subnet, dns1, dns2);
}

void startMdns() {
  const String host = cfg::config().network.hostname.length() ? cfg::config().network.hostname
                                                              : String("sheltr");
  if (g_mdnsStarted) MDNS.end();
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "device", "sheltr-esp");
    MDNS.addServiceTxt("http", "tcp", "version", SHELTR_FW_VERSION);
    g_mdnsStarted = true;
    log_i("mDNS attivo: http://%s.local", host.c_str());
  } else {
    log_w("mDNS non avviato");
  }
}

void configureTime() {
  const cfg::NtpCfg &ntp = cfg::config().ntp;
  if (!ntp.enabled || g_ntpConfigured) return;
  configTzTime(ntp.tz.c_str(), ntp.server.c_str(), "time.google.com", "pool.ntp.org");
  g_ntpConfigured = true;
  log_i("NTP configurato (%s, TZ=%s)", ntp.server.c_str(), ntp.tz.c_str());
}

void onNetworkEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(cfg::config().network.hostname.c_str());
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      g_ethConnected = true;
      log_i("Ethernet: %s", ETH.localIP().toString().c_str());
      startMdns();
      configureTime();
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      g_ethConnected = false;
      log_w("Ethernet disconnessa");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_wifiConnected = true;
      log_i("WiFi: %s (%s)", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
      startMdns();
      configureTime();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_wifiConnected = false;
      break;
    default:
      break;
  }
}

String defaultApSsid() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "Sheltr-%02X%02X", mac[4], mac[5]);
  return String(buffer);
}

void startWifiStation() {
  const cfg::NetworkCfg &network = cfg::config().network;
  if (!network.wifiSsid.length()) return;
  WiFi.setHostname(network.hostname.c_str());
  WiFi.begin(network.wifiSsid.c_str(), network.wifiPassword.c_str());
  log_i("Connessione WiFi a %s...", network.wifiSsid.c_str());
}

}  // namespace

void begin() {
  g_bootAt = millis();
  g_nextWifiRetryAt = g_bootAt + 30000;  // lascia tempo al primo tentativo di connessione
  WiFi.onEvent(onNetworkEvent);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);

  const cfg::NetworkCfg &network = cfg::config().network;
  g_apSsid = network.apSsid.length() ? network.apSsid : defaultApSsid();

  if (network.ethEnabled) {
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    // Il driver gestisce da sé il bus SPI3: non va inizializzato prima con SPI.begin(),
    // altrimenti i pin restano agganciati all'host SPI di default.
    if (!ETH.begin(ETH_PHY_W5500, ETH_PHY_ADDR, ETH_CS_PIN, ETH_INT_PIN, ETH_RST_PIN, SPI3_HOST,
                   ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN)) {
      log_e("Avvio Ethernet W5500 fallito");
    }
#else
    SPI.begin(ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN);
    if (!ETH.begin(ETH_PHY_W5500, ETH_PHY_ADDR, ETH_CS_PIN, ETH_INT_PIN, ETH_RST_PIN, SPI)) {
      log_e("Avvio Ethernet W5500 fallito");
    }
#endif
  }

  applyStaticConfig();
  startWifiStation();
}

void loop() {
  if (g_apActive) g_dns.processNextRequest();

  const cfg::NetworkCfg &network = cfg::config().network;
  const uint32_t now = millis();

  // Nessuna connettività dopo l'avvio: apre l'hotspot di configurazione.
  if (!g_apActive && network.apFallback && !g_ethConnected && !g_wifiConnected &&
      static_cast<int32_t>(now - (g_bootAt + 20000)) > 0) {
    startAccessPoint();
  }

  // Hotspot attivo ma la rete è tornata: lo chiude dopo una pausa, così chi ha appena
  // configurato il WiFi dal telefono fa in tempo a leggere l'esito.
  if (g_apActive && (g_ethConnected || g_wifiConnected)) {
    if (g_apStopAt == 0) {
      g_apStopAt = now + 20000;
    } else if (static_cast<int32_t>(now - g_apStopAt) > 0) {
      stopAccessPoint();
    }
  } else if (!g_apActive) {
    g_apStopAt = 0;
  }

  if (!g_wifiConnected && network.wifiSsid.length() && !g_apActive &&
      static_cast<int32_t>(now - g_nextWifiRetryAt) > 0) {
    g_nextWifiRetryAt = now + 30000;
    WiFi.disconnect();
    startWifiStation();
  }

  if ((g_ethConnected || g_wifiConnected) && !g_mdnsStarted) startMdns();
}

void startAccessPoint() {
  if (g_apActive) return;
  const cfg::NetworkCfg &network = cfg::config().network;
  g_apSsid = network.apSsid.length() ? network.apSsid : defaultApSsid();
  WiFi.mode(network.wifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  const bool ok = WiFi.softAP(g_apSsid.c_str(),
                              network.apPassword.length() >= 8 ? network.apPassword.c_str() : nullptr);
  if (!ok) {
    log_e("Hotspot non avviato");
    return;
  }
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", WiFi.softAPIP());
  g_apActive = true;
  log_i("Hotspot attivo: SSID=%s IP=%s", g_apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void stopAccessPoint() {
  if (!g_apActive) return;
  g_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_apActive = false;
  log_i("Hotspot disattivato");
}

bool connectWifi(const String &ssid, const String &password, uint32_t timeoutMs) {
  if (!ssid.length()) return false;
  WiFi.mode(g_apActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t deadline = millis() + timeoutMs;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiConnected = true;
      startMdns();
      configureTime();
      return true;
    }
    delay(200);
  }
  return false;
}

void scanNetworks(JsonArray out) {
  const int found = WiFi.scanNetworks(false, true);
  for (int i = 0; i < found && i < 24; i++) {
    JsonObject item = out.add<JsonObject>();
    item["ssid"] = WiFi.SSID(i);
    item["rssi"] = WiFi.RSSI(i);
    item["channel"] = WiFi.channel(i);
    item["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
}

bool ethConnected() { return g_ethConnected; }
bool wifiConnected() { return g_wifiConnected && WiFi.status() == WL_CONNECTED; }
bool apActive() { return g_apActive; }
bool online() { return ethConnected() || wifiConnected(); }

String ethIp() { return g_ethConnected ? ETH.localIP().toString() : String(); }
String wifiIp() { return wifiConnected() ? WiFi.localIP().toString() : String(); }
String apIp() { return g_apActive ? WiFi.softAPIP().toString() : String(); }
String apSsid() { return g_apSsid; }
String hostname() { return cfg::config().network.hostname; }
String macAddress() { return WiFi.macAddress(); }
String wifiSsid() { return WiFi.SSID(); }
int wifiRssi() { return wifiConnected() ? WiFi.RSSI() : 0; }

void statusJson(JsonObject out) {
  out["hostname"] = hostname();
  out["mdns"] = hostname() + ".local";
  out["online"] = online();

  JsonObject eth = out["ethernet"].to<JsonObject>();
  eth["enabled"] = cfg::config().network.ethEnabled;
  eth["connected"] = g_ethConnected;
  eth["ip"] = ethIp();
  if (g_ethConnected) {
    eth["mac"] = ETH.macAddress();
    eth["gateway"] = ETH.gatewayIP().toString();
    eth["subnet"] = ETH.subnetMask().toString();
    eth["linkSpeed"] = ETH.linkSpeed();
    eth["fullDuplex"] = ETH.fullDuplex();
  }

  JsonObject wifi = out["wifi"].to<JsonObject>();
  wifi["configured"] = cfg::config().network.wifiSsid.length() > 0;
  wifi["connected"] = wifiConnected();
  wifi["ssid"] = wifiConnected() ? WiFi.SSID() : cfg::config().network.wifiSsid;
  wifi["ip"] = wifiIp();
  wifi["rssi"] = wifiRssi();
  wifi["mac"] = WiFi.macAddress();

  JsonObject ap = out["accessPoint"].to<JsonObject>();
  ap["active"] = g_apActive;
  ap["ssid"] = g_apSsid;
  ap["ip"] = apIp();
  ap["fallback"] = cfg::config().network.apFallback;
}

}  // namespace net
