# 05 · API HTTP

Base URL: `http://sheltr.local` (oppure l'IP del dispositivo). Tutte le risposte sono JSON.

> Negli esempi gli id delle schede sono scritti in forma breve (`board-1`) per leggibilità: sul
> dispositivo sono UUID generati automaticamente, per esempio
> `3f8a1c22-6d41-4b0e-9a77-1e2b3c4d5e6f-c1`.

## Autenticazione

Se *Richiedi login* è disattivato (default) le API sono aperte sulla rete locale. Con il login attivo:

```bash
TOKEN=$(curl -s -X POST http://sheltr.local/api/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"sheltr"}' | jq -r .token)

curl -H "Authorization: Bearer $TOKEN" http://sheltr.local/api/status
```

Il token vale 12 ore e si può passare anche come `?token=…` o tramite cookie `sheltr_token`.

| Rotta | Metodo | Descrizione |
|---|---|---|
| `/api/auth` | GET | stato dell'autenticazione |
| `/api/auth/login` | POST | `{username, password}` → `{token}` |
| `/api/auth/logout` | POST | invalida il token corrente |

## Stato e comandi

### `GET /api/status`

Parametro opzionale `refresh=1`: interroga il bus prima di rispondere (polling `0x40` di tutte le schede).

```json
{
  "instanceId": "sheltr-a1b2c3",
  "name": "Casa",
  "deviceType": "sheltr_esp",
  "protocolVersion": "1.6",
  "device": { "online": true, "lastSeenAt": "2026-07-26T10:12:44" },
  "rooms": [
    {
      "name": "Cucina",
      "color": "#d4e5f7",
      "lights": [
        { "id": "board-1-c1", "name": "Faretti", "room": "Cucina", "address": 1,
          "channel": 1, "kind": "light", "isOn": true, "online": true,
          "favorite": true, "busActivityAgoMs": 1200 }
      ],
      "dimmers": [], "shutters": [], "thermostats": [], "sequences": [], "inputs": []
    }
  ],
  "boards": [
    { "id": "board-1", "name": "Luci", "address": 1, "kind": "light", "online": true,
      "poll": { "outputMask": 5, "inputMask": 0, "dimmerLevel": 0,
                "temperature": 21.4, "setpoint": 21, "powerKw": 0.3 } }
  ],
  "refreshErrors": []
}
```

### `POST /api/poll`

Polling mirato di una scheda: `{"address": 1}` oppure `{"channelId": "board-1-c3"}`. Risponde con lo
stesso documento di `/api/status`.

### Comandi

| Rotta | Corpo | Note |
|---|---|---|
| `POST /api/lights/command` | `{"lightId":"board-1-c1","action":"on"}` | `on`, `off`, `toggle` |
| `POST /api/dimmers/command` | `{"dimmerId":"board-2-c1","action":"set","level":6}` | `on`, `off`, `toggle`, `set` (0–9) |
| `POST /api/shutters/command` | `{"shutterId":"board-3-c2","action":"up"}` | `up`, `down`, `stop` |
| `POST /api/thermostats/command` | `{"thermostatId":"board-4-c1","setpoint":21.5}` | `setpoint` (5–30, passo 0,5), `mode` (`winter`/`summer`), `power` (`on`/`off`) |
| `POST /api/frame` | `{"hex":"49 01 40 …"}` | frame grezzo, risposta in `responseHex` |

In alternativa all'id si possono usare `address` + `channel`. Risposta tipica:

```json
{ "ok": true,
  "frame": "49 01 51 41 00 00 00 00 00 00 00 00 00 46",
  "response": "49 01 51 41 00 00 00 00 00 00 00 00 00 46",
  "verification": { "acknowledged": true, "pollVerified": true } }
```

In caso di scheda muta la risposta è `502` con `{"ok": false, "error": "Nessuna risposta dal bus"}`.

## Preferiti e sequenze

| Rotta | Metodo | Descrizione |
|---|---|---|
| `/api/favorites` | POST / PUT | `{"id":"board-1-c1","favorite":true}` — accetta anche id di sequenze e ingressi (`input-0`) |
| `/api/sequences` | GET | elenco sequenze con stato di esecuzione |
| `/api/sequences/<id>/run` | POST | avvia la sequenza |
| `/api/sequences/run` | POST | `{"id":"buonanotte"}` |
| `/api/sequences/stop` | POST | `{"id":"…"}` interrompe una sequenza, senza corpo le interrompe tutte |
| `/api/inputs` | POST / PUT | `{"index":0,"name":"…","room":"…","favorite":true,"sequenceId":"…"}` |

`GET /api/status` include `sequencer` con lo stato del runner (`running`, `runCount`, `active[]`,
`lastError`), l'array `inputs` con lo stato dei contatti e, per ogni stanza, gli array `sequences` e
`inputs`. Ogni canale riporta `busActivityAgoMs` (millisecondi dall'ultimo scambio sul bus) quando
l'attività è recente: è il dato dietro il pallino verde dell'interfaccia.

## Configurazione

| Rotta | Metodo | Descrizione |
|---|---|---|
| `/api/config` | GET | configurazione completa (password oscurate); con `?secrets=1` include le password ma richiede il token di sistema |
| `/api/config` | PUT / POST | applica e salva; accetta anche documenti parziali |
| `/api/rooms/color` | PUT / POST | `{"room":"Cucina","color":"#d4e5f7"}` |

`device.id` è l'UUID del dispositivo: viene restituito ma **ignorato in scrittura**.

Il `PUT` accetta solo le sezioni presenti nel corpo: per cambiare il baudrate basta

```bash
curl -X PUT http://sheltr.local/api/config \
  -H 'Content-Type: application/json' \
  -d '{"bus":{"baud":19200}}'
```

## Sistema

Tutte queste rotte (tranne `/api/meta` e `/api/system/unlock`) richiedono il **token di sistema**,
ottenuto con la password della sezione Sistema e da passare nell'header `X-Sheltr-System`
(o come `?systemToken=`). Il token dura 30 minuti.

| Rotta | Metodo | Descrizione |
|---|---|---|
| `/api/meta` | GET | identità del dispositivo, stato login e rete (pubblica) |
| `/api/system/unlock` | POST | `{"password":"Algo1962"}` → `{token}` |
| `/api/system/lock` | POST | invalida il token di sistema |
| `/api/system` | GET | rete, MQTT, bus, memoria, uptime, orologio, filesystem, `performance` |
| `/api/system/restart` | POST | riavvio |
| `/api/system/factory-reset` | POST | `{"keepNetwork": true}` |
| `/api/system/ota` | POST | upload multipart del firmware (`firmware=@file.bin`) |
| `/api/frame` | POST | invio frame grezzo |
| `/api/system/update` | GET | stato del controllo aggiornamenti |
| `/api/system/update/check` | POST | interroga le release GitHub |
| `/api/system/update/install` | POST | scarica e installa l'ultima release |
| `/api/system/rtc` | POST | `{"action":"fromRtc"\|"toRtc"\|"set","time":"YYYY-MM-DDTHH:MM"}` |
| `/api/wifi/scan` | GET | reti WiFi visibili (libera in modalità hotspot) |
| `/api/wifi/connect` | POST | `{"ssid","password"}` (libera in modalità hotspot) |

## Compatibilità Sheltr Cloud

Le stesse rotte esistono anche nella forma `/api/instances/<id>/…` usata dal portale, così l'integrazione
[sheltr-homeassistant](https://github.com/filippobenozzi/sheltr-homeassistant) può puntare direttamente al
dispositivo locale:

```
GET  /api/instances                              GET  /api/instances/<id>
POST /api/instances/<id>/auth/login              GET  /api/instances/<id>/status
POST /api/instances/<id>/poll                    POST /api/instances/<id>/lights/command
POST /api/instances/<id>/dimmers/command         POST /api/instances/<id>/shutters/command
POST /api/instances/<id>/thermostats/command     PUT  /api/instances/<id>/rooms/color
```

`<id>` viene ignorato (il dispositivo gestisce una sola istanza): usa l'ID mostrato in *Sistema*.
Per l'integrazione Home Assistant inserisci come URL del portale `http://sheltr.local` e come ID impianto
l'ID del dispositivo; attiva il login se vuoi usare username e password.

## Codici di errore

| Codice | Significato |
|---|---|
| `400` | payload non valido o parametri mancanti |
| `401` | token assente o scaduto, oppure password errata |
| `403` | sezione Sistema bloccata: serve il token di sistema |
| `404` | dispositivo/rotta inesistente |
| `409` | una sequenza è già in esecuzione |
| `500` | salvataggio configurazione o OTA fallito |
| `502` | la scheda non ha risposto sul bus |
