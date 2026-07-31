# 04 · Protocollo Sheltr 1.6

Il firmware implementa il protocollo direttamente in C++ (`src/protocol.cpp`, `src/bus.cpp`,
`src/devices.cpp`). Nessun componente esterno traduce i comandi: l'ESP32 parla con le schede.

## Frame

Frame fisso di **14 byte**, seriale **9600 8N1**:

```
| 0x49 | ADDR | CMD | G1 G2 G3 G4 G5 G6 G7 G8 G9 G10 | 0x46 |
  start  addr   cmd             10 byte                 end
```

- `ADDR`: indirizzo della scheda (0–254)
- `CMD`: comando
- `G1..G10`: parametri, i byte non usati valgono `0x00`

## Comandi

| Funzione | CMD | Parametri |
|---|---|---|
| Polling stato | `0x40` | — |
| Relè luce 1–4 | `0x51`–`0x54` | G1 = `0x41` ON / `0x53` OFF |
| Relè luce 5–8 | `0x65`–`0x68` | G1 = `0x41` ON / `0x53` OFF |
| Tapparella | `0x5C` | G1 = canale (1–4), G2 = `0x55` su / `0x44` giù / `0x53` stop |
| Dimmer | `0x5B` | G1 = `0x53`, G2 = livello 0–9 |
| Termostato setpoint | `0x5A` | G1 = parte intera, G2 = decimo; `0,0` = spegnimento |
| Termostato stagione | `0x6B` | G1 = `0x00` inverno / `0x01` estate |
| Attivazione liste scenari | `0xAA` | G1 = numero lista (da 1); G2 = `0x41` attiva / `0x53` disattiva / `0x55` scenario multiplo |

Il comando `0xAA` è **broadcast**: tutte le schede lo ricevono e nessuna risponde. Il gateway lo usa in
**ricezione** per far partire le proprie sequenze (vedi
[03 · Sequencer](03-configurazione.md#avvio-dal-bus-aann)).

Le schede rispondono con un frame dello stesso indirizzo e comando; il firmware valida anche i byte G
attesi (per esempio il livello impostato sul dimmer) e ripete il comando se la risposta non arriva entro
il timeout.

## Risposta al polling `0x40`

| Byte | Significato |
|---|---|
| G1 | tipo scheda (nibble basso) + release (nibble alto) |
| G2 | maschera uscite (bit 0 = canale 1) |
| G3 | maschera ingressi |
| G4 | livello dimmer (0–9) |
| G5 + G6 | temperatura: interi + decimi |
| G7 | segno temperatura (`0x2D` = negativo) |
| G8 | potenza in decimi di kW |
| G9 | setpoint termostato |

Dalla maschera uscite il firmware ricava lo stato reale di ogni luce, dal livello dimmer la luminosità,
da G5–G7 la temperatura ambiente e da G9 il setpoint; per i termostati il bit corrispondente della
maschera uscite indica se il relè sta chiamando calore/freddo.

## Ciclo di un comando

1. Costruzione del frame e attesa del mutex del bus (una sola transazione per volta).
2. Se configurato, il pin DE/RE viene alzato per abilitare il driver RS485 in trasmissione.
3. Invio dei 14 byte e attesa della risposta entro il timeout, scartando eventuali frame non pertinenti.
4. In caso di silenzio la transazione viene ripetuta (`Tentativi extra`, default 2).
5. Dopo un comando andato a buon fine il firmware esegue un polling `0x40` della scheda per confermare lo
   stato reale — lo stesso comportamento del gateway Python.
6. Lo stato aggiornato viene propagato a interfaccia web, MQTT e Home Assistant.

## Frame grezzi

Per diagnosi (o per pilotare funzioni non ancora esposte) puoi inviare un frame arbitrario:

```bash
curl -X POST http://sheltr.local/api/frame \
  -H 'Content-Type: application/json' \
  -d '{"hex":"49 01 40 00 00 00 00 00 00 00 00 00 00 46"}'
```

```json
{
  "ok": true,
  "frame": "49 01 40 00 00 00 00 00 00 00 00 00 00 46",
  "responseHex": "49 01 40 11 05 00 00 15 03 00 00 00 00 46"
}
```

La stessa funzione è disponibile in *Sistema → Diagnostica protocollo* e viene usata dal bridge Sheltr
Cloud per inoltrare verbatim i frame ricevuti dal portale.

## Formati payload

Quando i frame viaggiano su MQTT vengono codificati con uno dei formati del portale:

| Formato | Esempio |
|---|---|
| `frame_hex_space` | `49 01 51 41 00 00 00 00 00 00 00 00 00 46` |
| `frame_hex_compact` | `4901514100000000000000000046` |
| `frame_hex_space_crlf` | come `frame_hex_space` + `\r\n` (default) |
| `frame_hex_compact_crlf` | come `frame_hex_compact` + `\r\n` |

In ricezione il firmware accetta indifferentemente payload binari o esadecimali: cerca la sequenza
`0x49 … 0x46` lunga 14 byte.
