# 08 · Hardware e cablaggio

## Scheda

**LilyGO T-ETH-Lite ESP32S3** — ESP32-S3-WROOM-1 (N16R8: 16 MB flash, 8 MB PSRAM OPI) con controller
Ethernet **W5500** su SPI e slot microSD.

| Funzione | GPIO |
|---|---|
| W5500 SCLK | 10 |
| W5500 MISO | 11 |
| W5500 MOSI | 12 |
| W5500 CS | 9 |
| W5500 INT | 13 |
| W5500 RST | 14 |
| microSD (MISO/MOSI/SCK/CS) | 5 / 6 / 7 / 42 |
| UART0 di debug (TXD/RXD) | 43 / 44 |
| USB nativo (D-/D+) | 19 / 20 |

Il firmware non usa la microSD: i pin restano liberi.

## Pin disponibili per il bus

I connettori P2/P3 espongono, tra gli altri, `IO1`, `IO2`, `IO3`, `IO8`, `IO15`, `IO16`, `IO17`, `IO18`,
`IO21`, `IO38`, `IO39`, `IO40`, `IO41`, `IO45`, `IO46`, `IO47`, `IO48`.

Default del firmware (modificabili da *Configurazione → Bus seriale*):

| Segnale | GPIO | Note |
|---|---|---|
| TX verso le schede | 17 | uscita |
| RX dalle schede | 18 | ingresso |
| DE/RE | -1 | disattivato; impostalo se usi RS485 |

## Collegamento TTL (bus diretto)

```
ESP32-S3            Scheda Sheltr
GPIO17  ─────────►  RX
GPIO18  ◄─────────  TX
GND     ───────────  GND
```

Da usare quando il bus è già a livello logico 3,3 V. **Non collegare mai direttamente segnali a 5 V o
RS232**: i GPIO dell'ESP32-S3 non sono tolleranti oltre 3,6 V — servono un partitore o un level shifter.

## Collegamento RS485 (half duplex)

```
ESP32-S3          MAX3485 / SP3485          Bus
GPIO17  ────────► DI
GPIO18  ◄──────── RO
GPIO16  ────────► DE + RE (uniti)
3V3     ────────► VCC                       A ──── A (linea +)
GND     ────────► GND                       B ──── B (linea −)
```

Imposta «GPIO DE/RE» a `16` nella configurazione: il firmware alza il pin durante la trasmissione e lo
riabbassa subito dopo lo svuotamento del buffer, con un margine di 50 µs per lato.

Consigli pratici:

- terminazione 120 Ω alle due estremità della linea;
- massa di riferimento comune tra gateway e schede;
- cavo schermato twistato per tratte lunghe;
- una sola coppia A/B per tutto il bus (topologia a margherita, niente stelle).

## Alimentazione

- Alimenta la scheda dal connettore USB-C (5 V) oppure dai pin `VCC5V`/`GND` di P3.
- Con lo shield PoE opzionale LilyGO puoi alimentarla direttamente dal cavo di rete.
- Prevedi almeno 500 mA: durante l'uso di WiFi e Ethernet insieme i picchi superano i 300 mA.

## Indirizzi delle schede

Ogni scheda del bus ha un indirizzo (0–254) impostato via dip-switch o programmazione. Nell'interfaccia,
l'indirizzo va indicato per ogni scheda configurata: due schede con lo stesso indirizzo generano
collisioni sul bus e risposte incoerenti.

Per scoprire quali indirizzi rispondono puoi usare la diagnostica frame:

```bash
for ADDR in $(seq 1 8); do
  HEX=$(printf '49 %02X 40 00 00 00 00 00 00 00 00 00 00 46' "$ADDR")
  echo -n "indirizzo $ADDR: "
  curl -s -X POST http://sheltr.local/api/frame \
    -H 'Content-Type: application/json' -d "{\"hex\":\"$HEX\"}" | jq -r '.responseHex // .error'
done
```
