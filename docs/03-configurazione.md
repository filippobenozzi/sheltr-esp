# 03 · Configurazione

Tutta la configurazione vive in un unico file JSON su LittleFS (`/config.json`) ed è modificabile
dall'interfaccia web o via API (`GET`/`PUT /api/config`).

## Schede e canali

Una **scheda** rappresenta un modulo fisico sul bus:

| Campo | Significato |
|---|---|
| `id` | identificatore stabile (`board-1`), usato per comporre gli id dei canali |
| `name` | nome mostrato nell'interfaccia e in Home Assistant |
| `kind` | `light`, `shutter`, `dimmer`, `thermostat` |
| `address` | indirizzo sul bus (0–254), quello impostato con i dip-switch |
| `channelStart` / `channelEnd` | intervallo di canali attivi |
| `channels[]` | nome e stanza di ogni canale, più l'eventuale profilo orario |

Numero massimo di canali per tipo: **luci 8**, **tapparelle 4**, **dimmer 1**, **termostato 1** — gli
stessi limiti del portale Sheltr Cloud.

L'id di un canale è sempre `<board.id>-c<channel>` (esempio `board-1-c3`): è la chiave usata da API,
MQTT e Home Assistant.

Esempio:

```json
{
  "boards": [
    {
      "id": "luci-piano-terra",
      "name": "Luci piano terra",
      "kind": "light",
      "address": 1,
      "channelStart": 1,
      "channelEnd": 8,
      "channels": [
        { "channel": 1, "name": "Faretti cucina", "room": "Cucina" },
        { "channel": 2, "name": "Lampadario sala", "room": "Sala" }
      ]
    }
  ]
}
```

## Stanze e colori

La **stanza** è un campo libero del canale: l'interfaccia raggruppa i dispositivi per stanza e assegna a
ognuna un colore della palette pastello (la stessa di Sheltr Cloud). Il colore si può fissare con:

```bash
curl -X PUT http://sheltr.local/api/rooms/color \
  -H 'Content-Type: application/json' \
  -d '{"room":"Cucina","color":"#d4e5f7"}'
```

Senza colore esplicito ne viene scelto uno stabile in base al nome della stanza.

## Profili orari

Ogni canale può avere un profilo. Si modifica dall'icona ◷ sulla tile o dalla tabella dei canali in
configurazione.

**Luci e tapparelle** — azione puntuale:

```json
{ "enabled": true,
  "entries": [ { "time": "07:30", "action": "on",  "days": [1,2,3,4,5] },
               { "time": "23:00", "action": "off", "days": [1,2,3,4,5,6,7] } ] }
```

`action` vale `on`/`off` per le luci, `up`/`down` per le tapparelle. `days` usa 1 = lunedì … 7 = domenica.

**Termostati** — fasce orarie con setpoint e stagione:

```json
{ "enabled": true,
  "entries": [ { "from": "06:30", "to": "08:30", "setpoint": 21,   "mode": "winter", "days": [1,2,3,4,5] },
               { "from": "17:00", "to": "22:30", "setpoint": 20.5, "mode": "winter", "days": [1,2,3,4,5] } ] }
```

Fuori da ogni fascia il termostato viene portato a **5 °C** (antigelo). Il motore controlla i profili ogni
20 secondi e riapplica il valore solo se lo stato corrente diverge: nessun frame inutile sul bus.

## Bus seriale

| Parametro | Default | Note |
|---|---|---|
| GPIO TX | 17 | verso le schede |
| GPIO RX | 18 | dalle schede |
| GPIO DE/RE | -1 | pin di abilitazione del transceiver RS485; -1 = collegamento TTL |
| Baudrate | 9600 | 8N1, come il gateway Python |
| Timeout risposta | 1200 ms | attesa massima del frame di risposta |
| Tentativi extra | 2 | ripetizioni in caso di mancata risposta |
| Polling automatico | 60 s | 0 disattiva il polling ciclico |

Il polling ciclico interroga tutte le schede con il comando `0x40` e tiene aggiornati interfaccia, MQTT e
Home Assistant anche quando i comandi arrivano da pulsanti fisici.

## Sicurezza

Di fabbrica l'interfaccia è **aperta** sulla rete locale. Attivando *Richiedi login* in
*Configurazione → Ora e sicurezza*:

- la UI mostra una schermata di accesso;
- tutte le rotte `/api/…` richiedono un token (`Authorization: Bearer …`, cookie o `?token=`);
- il token si ottiene da `POST /api/auth/login` e dura 12 ore.

Le password non vengono mai restituite dalle API: nei `GET` compaiono come `********` e si aggiornano solo
inviando un valore nuovo.

## Backup e ripristino

```bash
# backup (le password sono oscurate)
curl http://sheltr.local/api/config > sheltr-config.json

# ripristino
curl -X PUT http://sheltr.local/api/config \
  -H 'Content-Type: application/json' \
  --data-binary @sheltr-config.json
```

Il ripristino di fabbrica (*Sistema → Manutenzione*) rigenera la configurazione predefinita mantenendo,
se vuoi, le impostazioni di rete.
