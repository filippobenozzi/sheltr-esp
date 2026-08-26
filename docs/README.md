# Documentazione Sheltr ESP

Firmware gateway del protocollo Sheltr 1.6 per **LilyGO T-ETH-Lite ESP32S3**.

| Documento | Contenuto |
|---|---|
| [01 · Installazione](01-installazione.md) | Flash dal browser, da riga di comando, aggiornamenti OTA |
| [02 · Rete e provisioning](02-rete-e-provisioning.md) | Ethernet, hotspot, captive portal, `sheltr.local`, IP statico |
| [03 · Configurazione](03-configurazione.md) | Schede, canali, stanze, profili orari, sicurezza |
| [04 · Protocollo 1.6](04-protocollo.md) | Formato frame, comandi, decodifica del polling |
| [05 · API HTTP](05-api.md) | Tutte le rotte REST, esempi `curl`, compatibilità Sheltr Cloud |
| [06 · MQTT e Home Assistant](06-mqtt-home-assistant.md) | Topic, discovery, entità create, esempi |
| [07 · Sheltr Cloud](07-sheltr-cloud.md) | Bridge verso il portale, topic, formato payload, collegamento via USR DR154, notifiche email |
| [08 · Hardware](08-hardware.md) | Pin, cablaggio TTL/RS485, alimentazione |
| [09 · Sviluppo](09-sviluppo.md) | Struttura del codice, build, CI, contribuire |
| [10 · Diagnostica](10-diagnostica.md) | Problemi comuni, log, strumenti di debug |

## Architettura in breve

```
        ┌──────────────────────────────────────────────────────────────┐
        │                    Sheltr ESP (ESP32-S3)                     │
        │                                                              │
 Bus    │  UART 9600 8N1  ┌───────────┐    ┌────────────────────────┐  │
schede ─┼─────────────────┤ protocollo├────┤ entità + stato (RAM)   │  │
 1.6    │   (TTL/RS485)   │   1.6     │    └───────────┬────────────┘  │
        │                 └───────────┘                │               │
        │                                ┌─────────────┼────────────┐  │
        │                                │             │            │  │
        │                        ┌───────▼──────┐ ┌────▼─────┐ ┌────▼────────┐
        │                        │ web + REST   │ │ MQTT HA  │ │ MQTT cloud  │
        │                        │ sheltr.local │ │ discovery│ │ <id>/cmd    │
        │                        └──────────────┘ └──────────┘ └─────────────┘
        └──────────────────────────────────────────────────────────────┘
                    Ethernet W5500  /  WiFi (client o hotspot)
```

Tutte le sorgenti di comando (interfaccia web, Home Assistant, portale cloud, profili orari) passano dallo
stesso motore comandi, che serializza le transazioni sul bus con un mutex e verifica l'esito con il
polling `0x40`.
