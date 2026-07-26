# Sheltr ESP

[![Build firmware](https://github.com/filippobenozzi/sheltr-esp/actions/workflows/build.yml/badge.svg)](https://github.com/filippobenozzi/sheltr-esp/actions/workflows/build.yml)
[![Licenza MIT](https://img.shields.io/badge/licenza-MIT-black)](LICENSE)

> ⚡ **Flash dal browser**: <https://filippobenozzi.github.io/sheltr-esp/>
> 📖 **Documentazione**: cartella [`docs/`](docs/)

Firmware per **LilyGO T-ETH-Lite ESP32S3** che trasforma la scheda in un **gateway locale del protocollo
Sheltr 1.6**: parla direttamente con le schede sul bus seriale, espone un'interfaccia di controllo web
identica a quella di [Sheltr Cloud](https://github.com/filippobenozzi/sheltr-cloud), pubblica gli accessori
su MQTT per Home Assistant e — se serve — si collega anche al portale cloud come istanza remota.

Niente Raspberry, niente server: tutto il protocollo è implementato a livello firmware.

## Cosa fa

- **Protocollo 1.6 completo a bordo**: frame da 14 byte (`0x49 … 0x46`), relè luci `0x51`–`0x54` /
  `0x65`–`0x68`, tapparelle `0x5C`, dimmer `0x5B`, termostato `0x5A` + `0x6B`, polling stato `0x40`
  con decodifica di uscite, ingressi, livello dimmer, temperatura, setpoint e potenza.
- **Interfaccia di controllo locale** in stile Sheltr Cloud: dispositivi raggruppati per stanza con
  palette pastello, tile per luci / dimmer / tapparelle / termostati, aggiornamento stato dal bus.
- **API HTTP compatibili con il portale**: le stesse rotte `/api/instances/<id>/…` usate da
  [sheltr-homeassistant](https://github.com/filippobenozzi/sheltr-homeassistant), quindi
  l'integrazione Home Assistant funziona puntando direttamente al dispositivo.
- **MQTT + discovery Home Assistant**: ogni canale diventa `light`, `cover` o `climate` con
  disponibilità, attributi (`board_id`, `address`, `channel`, `room`) e pulsanti di polling.
- **Bridge Sheltr Cloud**: pubblica la configurazione retained su `<istanza>/config`, riceve i frame su
  `<istanza>/cmd` e rimanda la risposta del bus su `<istanza>/pub` (profilo `Sheltr Mini`).
- **Profili orari** per luci, tapparelle e termostati, con orologio sincronizzato via NTP.
- **Rete plug & play**: Ethernet W5500 in DHCP o IP statico, dominio locale `sheltr.local`, hotspot con
  captive portal al primo avvio per configurare il WiFi.
- **Aggiornamento firmware** dall'interfaccia web (OTA) o dal browser via
  [ESP Web Tools](https://esphome.github.io/esp-web-tools/).

## Avvio rapido

1. **Flash**: apri <https://filippobenozzi.github.io/sheltr-esp/> con Chrome/Edge, collega la scheda via
   USB e premi «Installa Sheltr ESP».
2. **Rete**:
   - con cavo Ethernet il gateway risponde su `http://sheltr.local`;
   - senza rete si accende l'hotspot `Sheltr-XXXX` (password `sheltr1234`) e il captive portal si apre da solo.
3. **Bus**: collega TX/RX (default `GPIO17` / `GPIO18`, 9600 8N1) alle schede Sheltr; con RS485 aggiungi un
   transceiver e imposta il pin DE/RE.
4. **Configura** le schede (tipo, indirizzo, canali, nomi, stanze) da *Configurazione* e salva.
5. **Opzionale**: attiva MQTT per Home Assistant e/o il collegamento a Sheltr Cloud.

## Struttura del repository

```
src/                 firmware (protocollo, bus, entità, rete, web server, MQTT, profili orari)
web/index.html       interfaccia locale (compressa e incorporata nel binario a build time)
site/                pagina GitHub Pages per il flash da browser
scripts/             script di build PlatformIO (asset web + binario unito)
docs/                documentazione completa
lib/ETHClass2/       driver Ethernet W5500 fornito da LilyGO (MIT)
```

## Compilazione

```bash
pip install platformio
pio run -e t-eth-lite-esp32s3
```

Il build produce `.pio/build/t-eth-lite-esp32s3/firmware.bin` (per l'OTA) e
`sheltr-esp-merged.bin` (immagine completa da scrivere a `0x0`). Ogni push su `main` ricompila tutto con
GitHub Actions e ripubblica la pagina di flash.

## Mappa GPIO usati

| Funzione | GPIO |
|---|---|
| W5500 (SCLK/MISO/MOSI/CS/INT/RST) | 10 / 11 / 12 / 9 / 13 / 14 |
| Bus Sheltr TX / RX (default) | 17 / 18 |
| DE/RE RS485 (opzionale) | configurabile |
| Slot microSD (non usato dal firmware) | 5 / 6 / 7 / 42 |

## Licenza

MIT — vedi [LICENSE](LICENSE).
