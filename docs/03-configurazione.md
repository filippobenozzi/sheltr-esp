# 03 · Configurazione

Tutta la configurazione vive in un unico file JSON su LittleFS (`/config.json`) ed è modificabile
dall'interfaccia web o via API (`GET`/`PUT /api/config`).

Al primo avvio **non c'è nessuna scheda preconfigurata**: l'impianto si costruisce da zero.

## Identità del dispositivo

Al primo avvio il firmware genera un **UUID** (versione 4) che identifica in modo univoco la scheda.
È visibile in *Configurazione → Dispositivo* e in *Sistema*, non è modificabile e sopravvive al
ripristino di fabbrica; le API lo ignorano se provi a cambiarlo. Il nome dell'impianto, invece, è libero.

## Sezioni dell'interfaccia

| Sezione | Contenuto |
|---|---|
| **Controllo** | preferiti e stanze; entrando in una stanza si vedono i suoi dispositivi e le sue sequenze |
| **Configurazione** | dispositivo, schede e canali, sequencer, MQTT Home Assistant, Sheltr Cloud |
| **Sistema** *(protetta da password)* | stato, bus seriale, rete e WiFi, ora, sicurezza, diagnostica frame, OTA, manutenzione |

## Preferiti

La stella su ogni tile (o la colonna *Preferito* nella tabella dei canali) aggiunge il dispositivo alla
sezione **Preferiti**, mostrata in cima al controllo prima delle stanze. Anche le sequenze possono essere
messe tra i preferiti. Il flag è salvato nella configurazione, quindi vale per tutti i browser.

```bash
curl -X POST http://sheltr.local/api/favorites \
  -H 'Content-Type: application/json' \
  -d '{"id":"board-1-c1","favorite":true}'
```

## Schede e canali

Una **scheda** rappresenta un modulo fisico sul bus:

| Campo | Significato |
|---|---|
| `id` | UUID generato alla creazione della scheda, non modificabile; è la base degli id dei canali |
| `name` | nome mostrato nell'interfaccia e in Home Assistant |
| `kind` | `light`, `shutter`, `dimmer`, `thermostat` |
| `address` | indirizzo sul bus (0–254), quello impostato con i dip-switch |
| `channelStart` / `channelEnd` | intervallo di canali attivi |
| `channels[]` | nome e stanza di ogni canale, più l'eventuale profilo orario |

Numero massimo di canali per tipo: **luci 8**, **tapparelle 4**, **dimmer 1**, **termostato 1** — gli
stessi limiti del portale Sheltr Cloud.

L'id di un canale è sempre `<board.id>-c<channel>`: è la chiave usata da API, MQTT e Home Assistant.

Gli id delle schede (come quelli delle sequenze) sono **UUID generati automaticamente** alla creazione e
mostrati in sola lettura: restano stabili anche se rinomini la scheda o cambi indirizzo, così preferiti,
sequenze ed entità Home Assistant non si rompono mai. Gli impianti configurati con firmware precedenti
mantengono i loro id storici (`board-1`, …): non vengono riscritti.

Esempio:

```json
{
  "boards": [
    {
      "id": "3f8a1c22-6d41-4b0e-9a77-1e2b3c4d5e6f",
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
ognuna un colore della palette pastello (la stessa di Sheltr Cloud).

Il colore si cambia da *Configurazione → Colori stanze*: per ogni stanza compaiono i dieci campioni della
palette e basta un clic per applicarlo (viene salvato subito, senza premere «Salva»). Da API:

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

## Sequencer

Un **pulsante virtuale** che esegue una serie di azioni in ordine. Si crea da *Configurazione →
Sequencer*: nome, stanza di appartenenza e l'elenco dei passi.

Ogni passo è un'azione su un canale (oppure una **sola attesa**) seguita da un tempo di attesa prima del
passo successivo:

```json
{
  "sequences": [
    {
      "id": "buonanotte",
      "name": "Buonanotte",
      "room": "Sala",
      "favorite": true,
      "steps": [
        { "channelId": "board-1-c2", "action": "off", "delaySec": 2 },
        { "channelId": "board-3-c1", "action": "down", "delaySec": 10 },
        { "channelId": "board-4-c1", "action": "setpoint", "value": 18, "delaySec": 0 }
      ]
    }
  ]
}
```

Azioni disponibili per tipo di canale:

| Tipo | `action` | `value` / `mode` |
|---|---|---|
| `light` | `on`, `off`, `toggle` | — |
| `dimmer` | `level`, `on`, `off` | `value` 0–9 con `level` |
| `shutter` | `up`, `down`, `stop` | — |
| `thermostat` | `setpoint`, `mode`, `off` | `value` 5–30 oppure `mode` `winter`/`summer` |

La sequenza appare come **tile nella stanza assegnata** (con il pulsante ESEGUI) e, se il client MQTT è
attivo, come **pulsante in Home Assistant**. L'esecuzione è a passi e non blocca il dispositivo: durante
le attese interfaccia, MQTT e polling continuano a funzionare.

Fino a **8 sequenze possono girare contemporaneamente**, ognuna con il proprio passo e il proprio timer:
se due partono nello stesso minuto proseguono in parallelo, senza aspettarsi. Sul bus i comandi restano
comunque serializzati, quindi non si accavallano mai due frame. La stessa sequenza non può essere avviata
due volte finché è in corso.

### Come si avvia una sequenza

| Sorgente | Come |
|---|---|
| Interfaccia | pulsante ESEGUI sulla tile o in *Configurazione → Sequencer* |
| API | `POST /api/sequences/<id>/run` |
| Home Assistant | pulsante creato dalla discovery (`<base>/sequence/<id>/set` ← `RUN`) |
| **Orario** | uno o più orari di avvio con i giorni della settimana (`schedule`) |
| **Ingresso digitale** | uno degli 8 contatti configurati (vedi sotto) |
| **Bus** | comando `AAnn` ricevuto dalle pulsantiere (vedi sotto) |

### Avvio a orario

Nell'editor della sequenza, *Avvio automatico* → uno o più orari con i giorni attivi:

```json
"schedule": { "enabled": true, "entries": [ { "time": "23:30", "days": [1,2,3,4,5] } ] }
```

Ogni sequenza ha solo l'ora di avvio: la durata dipende dalle attese dei suoi passi. Orari sovrapposti tra
sequenze diverse sono ammessi.

### Avvio dal bus (`AAnn`)

Nel campo **Comando bus** si assegna un numero (1–255). Il gateway ascolta il bus quando è a riposo e
avvia la sequenza quando riceve:

- un **frame protocollo** con comando `0xAA` e il numero nel primo byte dati
  (`49 <addr> AA 01 00 … 46` → scenario 1);
- oppure il **testo ASCII** `AA01` (due cifre esadecimali dopo `AA`).

Lo stato *Sistema* riporta quanti comandi scenario sono arrivati e qual è stato l'ultimo.

```bash
# avvio manuale
curl -X POST http://sheltr.local/api/sequences/buonanotte/run
```

## Ingressi digitali

Otto contatti fisici possono avviare una sequenza.

- In *Sistema → Ingressi digitali* si scelgono **GPIO, pull-up, verso del contatto e antirimbalzo**.
  I GPIO predefiniti sono `1, 2, 21, 38, 39, 40, 41, 47`, tutti liberi sui connettori della scheda.
- In *Configurazione → Ingressi* si assegnano **nome, stanza, preferito e sequenza da avviare**.

Un ingresso abilitato compare nel controllo come tile della sua stanza (e tra i preferiti, se marcato):
mostra lo stato del contatto, la sequenza collegata e il numero di attivazioni.

Il default è contatto verso GND con pull-up interno: basta un pulsante tra il GPIO e la massa.
L'avvio avviene sul fronte di chiusura, con antirimbalzo configurabile (60 ms di fabbrica).

```bash
curl -X POST http://sheltr.local/api/inputs \
  -H 'Content-Type: application/json' \
  -d '{"index":0,"name":"Pulsante ingresso","room":"Sala","favorite":true,"sequenceId":"buonanotte"}'
```

## Regolazione del setpoint

I pulsanti `+` e `−` del termostato non inviano un comando a ogni pressione: la tile mostra subito il
valore che stai impostando (in azzurro, con l'etichetta «Invio tra poco») e il comando parte **dopo 3
secondi di inattività**, una volta sola con il valore finale. Portare la temperatura da 20 a 23 gradi
manda quindi un solo frame sul bus invece di sei.

## Indicatore di attività sul bus

A destra del nome di ogni dispositivo compare un **pallino verde** mentre il canale sta scambiando
comandi sul protocollo. Si accende quando:

- il gateway invia un comando per quel canale (anche da MQTT, sequenza o profilo orario);
- il polling `0x40` rileva che lo stato del canale è **cambiato** senza un nostro comando — tipicamente
  qualcuno ha premuto un pulsante fisico sull'impianto.

Il pallino resta visibile per pochi secondi e poi sparisce: serve a vedere a colpo d'occhio *chi* sta
parlando sul bus. Nel controllo lo stato viene riletto ogni 4 secondi, così l'indicazione è tempestiva.

## Bus seriale

Si configura da *Sistema → Bus seriale* (sezione protetta da password).

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

### Password della sezione Sistema

La sezione **Sistema** è sempre protetta, anche senza login: contiene bus, rete, aggiornamento firmware,
ripristino e console frame. Password di fabbrica **`Algo1962`**, modificabile in *Sistema → Ora e
sicurezza*. Lo sblocco vale 30 minuti e protegge anche le API corrispondenti:

```bash
TOKEN=$(curl -s -X POST http://sheltr.local/api/system/unlock \
  -H 'Content-Type: application/json' -d '{"password":"Algo1962"}' | jq -r .token)

curl -H "X-Sheltr-System: $TOKEN" http://sheltr.local/api/system
```

Unica eccezione: quando il dispositivo è in **modalità hotspot**, `/api/wifi/scan` e `/api/wifi/connect`
restano liberi, altrimenti il primo provisioning sarebbe impossibile (l'access point ha già la sua
password).

### Login generale

Di fabbrica l'interfaccia è **aperta** sulla rete locale. Attivando *Richiedi login* in
*Sistema → Ora e sicurezza*:

- la UI mostra una schermata di accesso;
- tutte le rotte `/api/…` richiedono un token (`Authorization: Bearer …`, cookie o `?token=`);
- il token si ottiene da `POST /api/auth/login` e dura 12 ore.

Le password non vengono mai restituite dalle API: nei `GET` compaiono come `********` e si aggiornano solo
inviando un valore nuovo.

## Backup e ripristino

Da *Sistema → Backup configurazione*:

- **Esporta JSON** scarica l'intera configurazione (schede, canali, stanze, preferiti, profili orari,
  sequenze, MQTT, cloud, rete, bus) in un file `sheltr-<hostname>-<data>.json`. La spunta
  **«Includi le password»** decide se scriverle in chiaro nel file: senza spunta vengono esportate come
  `********` e, in fase di ripristino, quelle già presenti sul dispositivo restano invariate.
- **Importa JSON** carica un file e lo applica subito. L'UUID del dispositivo non viene sovrascritto,
  quindi lo stesso backup si può usare anche su una scheda diversa.

Da riga di comando:

```bash
# backup (le password sono oscurate)
curl http://sheltr.local/api/config > sheltr-config.json

# backup completo (richiede la sezione Sistema sbloccata)
curl -H "X-Sheltr-System: $TOKEN" 'http://sheltr.local/api/config?secrets=1' > sheltr-config.json

# ripristino
curl -X PUT http://sheltr.local/api/config \
  -H 'Content-Type: application/json' \
  --data-binary @sheltr-config.json
```

Il ripristino di fabbrica (*Sistema → Manutenzione*) rigenera la configurazione predefinita mantenendo,
se vuoi, le impostazioni di rete.
