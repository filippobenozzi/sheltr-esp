# 05 · API HTTP

Base URL: `http://sheltr.local` (oppure l'IP del dispositivo). Tutte le risposte sono JSON.

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
          "channel": 1, "kind": "light", "isOn": true, "online": true }
      ],
      "dimmers": [], "shutters": [], "thermostats": []
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

## Configurazione

| Rotta | Metodo | Descrizione |
|---|---|---|
| `/api/config` | GET | configurazione completa (password oscurate) |
| `/api/config` | PUT / POST | applica e salva; accetta anche documenti parziali |
| `/api/rooms/color` | PUT / POST | `{"room":"Cucina","color":"#d4e5f7"}` |

Il `PUT` accetta solo le sezioni presenti nel corpo: per cambiare il baudrate basta

```bash
curl -X PUT http://sheltr.local/api/config \
  -H 'Content-Type: application/json' \
  -d '{"bus":{"baud":19200}}'
```

## Sistema

| Rotta | Metodo | Descrizione |
|---|---|---|
| `/api/meta` | GET | identità del dispositivo, stato login (pubblica) |
| `/api/system` | GET | rete, MQTT, bus, memoria, uptime, orologio |
| `/api/system/restart` | POST | riavvio |
| `/api/system/factory-reset` | POST | `{"keepNetwork": true}` |
| `/api/system/ota` | POST | upload multipart del firmware (`firmware=@file.bin`) |
| `/api/wifi/scan` | GET | reti WiFi visibili |
| `/api/wifi/connect` | POST | `{"ssid","password"}`, salva e connette |

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
| `401` | token assente o scaduto |
| `404` | dispositivo/rotta inesistente |
| `500` | salvataggio configurazione o OTA fallito |
| `502` | la scheda non ha risposto sul bus |
