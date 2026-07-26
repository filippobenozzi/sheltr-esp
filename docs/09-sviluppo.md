# 09 · Sviluppo

## Requisiti

- Python 3.10+
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) (`pip install platformio`)

Il primo build scarica toolchain e framework (circa 1,5 GB in `~/.platformio`).

## Comandi

```bash
pio run                       # compila (env t-eth-lite-esp32s3)
pio run -t upload             # compila e scrive via seriale
pio device monitor            # log a 115200 baud
pio run -t clean              # pulizia
SHELTR_FORCE_WEB=1 pio run    # forza la rigenerazione degli asset web
```

Artefatti in `.pio/build/t-eth-lite-esp32s3/`:

- `firmware.bin` — solo applicazione, per l'OTA;
- `sheltr-esp-merged.bin` — bootloader + partizioni + applicazione, da scrivere a `0x0` (è il file usato
  dalla pagina di flash).

## Struttura del codice

| File | Ruolo |
|---|---|
| `src/main.cpp` | avvio dei sottosistemi e loop principale |
| `src/protocol.{h,cpp}` | frame 1.6: costruzione, parsing, decodifica polling, formati payload |
| `src/bus.{h,cpp}` | trasporto UART, mutex, RS485 DE/RE, timeout e ritentativi |
| `src/settings.{h,cpp}` | modello di configurazione, LittleFS, serializzazione JSON |
| `src/devices.{h,cpp}` | entità, stato runtime, motore comandi, documento di stato |
| `src/schedules.{h,cpp}` | profili orari luci/tapparelle/termostati |
| `src/network.{h,cpp}` | Ethernet W5500, WiFi, hotspot + captive portal, mDNS, NTP |
| `src/webserver.{h,cpp}` | server HTTP, API REST, autenticazione, OTA |
| `src/mqtt_bridge.{h,cpp}` | client MQTT locale (discovery HA) e client Sheltr Cloud |
| `src/json_utils.h` | allocatore ArduinoJson su PSRAM |
| `web/index.html` | interfaccia locale (file unico) |
| `scripts/build_web.py` | comprime `web/index.html` e genera `src/generated/web_assets.h` |
| `scripts/merge_firmware.py` | crea il binario unito dopo il build |

### Perché il web server è sincrono

`WebServer` fa parte del core Arduino: nessuna dipendenza da mantenere allineata alle versioni del
framework, e soprattutto un solo thread che tocca il bus seriale. Le transazioni sul bus durano
100–300 ms e sono comunque serializzate da un mutex: un server asincrono aggiungerebbe complessità senza
vantaggi misurabili su questo carico.

### Interfaccia web

L'interfaccia è un unico file HTML con CSS e JavaScript inline, senza dipendenze esterne (funziona anche
quando il dispositivo è isolato da internet, per esempio in modalità hotspot). Durante il build viene
compressa con gzip e inserita in un array `PROGMEM`; il server la serve con
`Content-Encoding: gzip`.

Per lavorare sull'interfaccia basta modificare `web/index.html` e ricompilare: lo script rileva la
modifica e rigenera l'header.

## Integrazione continua

`.github/workflows/build.yml`:

1. **build** — compila con PlatformIO e carica gli artefatti (`firmware.bin`, `merged`, bootloader,
   tabella partizioni).
2. **pages** — solo su `main`: costruisce il sito (`site/` + binari + `manifest.json` con la versione) e
   lo pubblica su GitHub Pages.
3. **release** — sui tag `v*`: allega i binari alla release.

Per pubblicare una nuova versione:

```bash
# aggiorna SHELTR_FW_VERSION in platformio.ini
git commit -am "release: 0.2.0"
git tag v0.2.0
git push origin main --tags
```

> La prima volta va abilitato GitHub Pages nelle impostazioni del repository
> (*Settings → Pages → Source: GitHub Actions*).

## Convenzioni

- Commenti e messaggi utente in italiano, identificatori in inglese.
- Niente allocazioni dinamiche nei percorsi di comando oltre a quelle di ArduinoJson (che usa la PSRAM).
- Ogni nuova funzione del protocollo va aggiunta in `protocol.h` con la relativa costante, non con numeri
  magici sparsi nel codice.
