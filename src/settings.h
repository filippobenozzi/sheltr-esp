#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <map>
#include <vector>

namespace cfg {

// Palette pastello delle stanze: stessi valori del portale Sheltr Cloud.
extern const char *ROOM_PALETTE[10];
constexpr size_t ROOM_PALETTE_SIZE = 10;

struct ProfileEntry {
  String time = "00:00";     // luci / tapparelle: orario di scatto
  String from = "00:00";     // termostato: inizio fascia
  String to = "23:59";       // termostato: fine fascia
  String action = "off";     // luci: on/off - tapparelle: up/down
  float setpoint = 21.0f;    // termostato
  String mode = "winter";    // termostato: winter/summer
  uint8_t days = 0x7F;       // bit0 = lunedì ... bit6 = domenica
};

struct Profile {
  bool enabled = false;
  std::vector<ProfileEntry> entries;
};

struct Channel {
  uint8_t channel = 1;
  String name;
  String room = "Senza stanza";
  bool favorite = false;
  // Notifica di cambio stato: la invia il portale, il flag e' qui solo per tenerlo
  // sincronizzato in entrambe le direzioni (il dispositivo non invia notifiche).
  bool notifyOnChange = false;
  Profile profile;
};

// Passo di una sequenza: un'azione su un canale, seguita da un'attesa.
// `channelId` vuoto = passo di sola attesa.
struct SequenceStep {
  String channelId;
  String action;          // on | off | toggle | up | down | stop | level | setpoint | mode
  float value = 0.0f;     // livello dimmer (0-9) o setpoint (5-30)
  bool hasValue = false;
  String mode;            // winter | summer (termostati)
  uint16_t delaySec = 0;  // attesa dopo l'esecuzione del passo
};

// Pulsante virtuale: esegue una serie di passi, può essere assegnato a una stanza.
// Può partire da interfaccia, MQTT, orario, ingresso GPIO o comando sul bus.
struct Sequence {
  String id;
  String name;
  String room = "Senza stanza";
  bool favorite = false;
  uint16_t busTrigger = 0;  // 0 = disattivato; N = frame 0xAA con numero N (es. AA01)
  Profile schedule;         // orari di avvio (entry.time + entry.days)
  std::vector<SequenceStep> steps;
};

// Ingresso digitale: un contatto che avvia una sequenza.
// I parametri hardware si impostano in Sistema, la sequenza si assegna dal Controllo.
struct InputCfg {
  bool enabled = false;
  int8_t gpio = -1;
  bool pullup = true;      // resistenza interna di pull-up
  bool activeLow = true;   // contatto verso GND
  uint16_t debounceMs = 60;
  String name;
  String room = "Senza stanza";
  bool favorite = false;
  String sequenceId;
  // Notifica allo scatto: la invia il portale (il gateway pubblica solo l'evento).
  bool notifyOnChange = false;
  String notifyText;  // testo personalizzato; vuoto = testo predefinito
};

constexpr size_t INPUT_COUNT = 8;

struct Board {
  String id;
  String name;
  String kind = "light";  // light | shutter | dimmer | thermostat
  uint8_t address = 1;
  uint8_t channelStart = 1;
  uint8_t channelEnd = 8;
  std::vector<Channel> channels;
};

struct DeviceCfg {
  String id;  // UUID generato al primo avvio, non modificabile
  String name = "Sheltr ESP";
};

struct AuthCfg {
  bool enabled = false;
  String username = "admin";
  String password = "sheltr";
  // Password della sezione Sistema (bus, rete, OTA, manutenzione): sempre richiesta.
  String systemPassword = "Algo1962";
};

struct BusCfg {
  int8_t tx = -1;
  int8_t rx = -1;
  int8_t de = -1;
  uint32_t baud = 9600;
  uint32_t timeoutMs = 1200;
  uint8_t retries = 2;
  uint16_t pollIntervalSec = 60;
};

struct NetworkCfg {
  String hostname = "sheltr";
  String wifiSsid;
  String wifiPassword;
  String apSsid;  // vuoto = Sheltr-<mac>
  String apPassword = "sheltr1234";
  bool apFallback = true;
  bool ethEnabled = true;
  bool dhcp = true;
  String ip;
  String gateway;
  String subnet;
  String dns1;
  String dns2;
};

struct NtpCfg {
  bool enabled = true;
  String server = "pool.ntp.org";
  String tz = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europa/Roma
};

// Orologio hardware DS3231 su I2C: tiene l'ora anche senza rete.
struct RtcCfg {
  bool enabled = false;
  int8_t sda = 15;
  int8_t scl = 16;
  uint8_t address = 0x68;
};

struct MqttCfg {
  bool enabled = false;
  String host;
  uint16_t port = 1883;
  String username;
  String password;
  String clientId;
  String baseTopic = "sheltr";
  bool discovery = true;
  String discoveryPrefix = "homeassistant";
  bool retain = true;
  uint8_t qos = 0;
  uint16_t stateIntervalSec = 30;
};

// Come il gateway raggiunge il portale.
//  - CloudTransport::Mqtt: rete propria (WiFi/Ethernet), client MQTT sul dispositivo;
//  - CloudTransport::Usr: nessuna rete propria, la connettivita' la da' un modulo
//    USR DR154 collegato in RS485 sulla seriale, che fa da ponte verso il broker.
enum class CloudTransport : uint8_t { Mqtt = 0, Usr = 1 };

// Collegamento seriale verso il modulo USR DR154 (di default la UART0, quella dei
// pin di programmazione GPIO1/GPIO3).
struct UsrLinkCfg {
  int8_t tx = 1;
  int8_t rx = 3;
  int8_t de = -1;  // pin di direzione del transceiver RS485; -1 = automatico
  uint32_t baud = 115200;
};

struct CloudCfg {
  bool enabled = false;
  CloudTransport transport = CloudTransport::Mqtt;
  UsrLinkCfg usr;
  String host;
  uint16_t port = 1883;
  String username;
  String password;
  String instanceId = "sheltr-esp";
  String instanceName = "Sheltr ESP";
  String payloadFormat = "frame_hex_space_crlf";
  // URL del portale usato per l'associazione tramite codice (POST /api/provision/claim).
  String portalUrl;
  // Ultima revisione di impostazioni (preferiti/profili) applicata dal cloud: evita
  // che un messaggio retained vecchio sovrascriva modifiche fatte dopo in locale.
  uint32_t settingsRevision = 0;
  // Heartbeat verso il portale: ogni quanti secondi ripubblicare lo stato delle
  // schede, così il cloud sa che il gateway è vivo. 0 = disattivato.
  uint16_t heartbeatSec = 300;
  // Codice di associazione da redimere: con il modulo USR il gateway non ha rete
  // propria, quindi la richiesta viaggia sul collegamento seriale invece che via HTTPS.
  String pendingClaimCode;
};

struct Config {
  DeviceCfg device;
  AuthCfg auth;
  BusCfg bus;
  NetworkCfg network;
  NtpCfg ntp;
  RtcCfg rtc;
  MqttCfg mqtt;
  CloudCfg cloud;
  std::map<String, String> roomColors;
  std::vector<Board> boards;
  std::vector<Sequence> sequences;
  std::vector<InputCfg> inputs;
  uint32_t revision = 0;
};

Config &config();

// Filesystem + caricamento configurazione. Se il file non esiste crea i default.
bool begin();
bool save();
bool filesystemMounted();
void resetDefaults(bool keepNetwork);

// Applica un documento JSON (dalla UI o da un backup) alla configurazione corrente.
bool applyJson(JsonObjectConst input, String &error);
void toJson(JsonObject out, bool includeSecrets);

// Serializza un profilo orario nel formato usato da UI e portale.
void profileJson(const Profile &profile, const String &kind, JsonObject out);

// Serializza una sequenza completa (passi e orari) per il portale.
void sequenceJson(const Sequence &sequence, JsonObject out);

// Applica preferiti e profili orari arrivati dal cloud (topic <istanza>/settings).
// Tocca solo i canali indicati: schede, stanze e nomi restano di competenza locale.
// Ritorna true se qualcosa e' cambiato (allora va salvato e ripubblicato).
bool applyCloudSettings(JsonObjectConst input);

// Utility condivise
String slugify(const String &value, const String &fallback);
String cleanText(const String &value, const String &fallback);
uint8_t maxChannelsForKind(const String &kind);
String defaultChannelName(const String &kind, uint8_t channel);
String kindLabel(const String &kind);
String entityId(const String &boardId, uint8_t channel);
String roomColor(const String &room);
void setRoomColor(const String &room, const String &color);

Board *findBoard(const String &boardId);
Board *findBoardByAddress(uint8_t address);
Channel *findChannel(const String &entityId, Board **boardOut);
Sequence *findSequence(const String &sequenceId);
Sequence *findSequenceByBusTrigger(uint16_t trigger);
std::vector<uint8_t> allAddresses();

// Identità del dispositivo: UUID generato una sola volta, non modificabile.
String newUuid();

}  // namespace cfg
