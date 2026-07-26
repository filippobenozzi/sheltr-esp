# 10 · Diagnostica

## Log seriale

Collega un adattatore USB-seriale ai pin `TXD` (GPIO43) e `RXD` (GPIO44) del connettore P3, oppure usa
l'USB nativo, e apri il monitor a **115200 baud**:

```bash
pio device monitor -b 115200
```

All'avvio il firmware stampa versione, ID dispositivo, stato di rete e indirizzo dell'interfaccia.

## Pannello Sistema

`http://sheltr.local` → **Sistema** riassume tutto quello che serve per capire dov'è il problema:

- stato di Ethernet, WiFi, hotspot e orologio;
- stato dei due client MQTT con l'ultimo errore riportato;
- statistiche del bus: frame inviati, risposte valide, errori, ultimo errore;
- tabella delle schede con stato online, maschera delle uscite e temperatura letta;
- console per inviare frame grezzi.

## Problemi comuni

### Il dispositivo non risponde su `sheltr.local`

1. Verifica il led del connettore Ethernet e l'IP assegnato dal router.
2. Prova con l'indirizzo IP: `http://192.168.1.x`.
3. Android e alcune reti aziendali bloccano mDNS: usa l'IP o assegna un indirizzo statico.
4. Se hai cambiato hostname, il nome è `<hostname>.local`.

### L'hotspot non compare

L'hotspot parte solo se **non** c'è connettività dopo 20 secondi e se «Apri l'hotspot se non c'è
connettività» è attivo. Con il cavo Ethernet collegato non si accende: scollegalo per farlo apparire, o
riattivalo dall'interfaccia.

### «Nessuna risposta dal bus»

In ordine di probabilità:

1. **Cablaggio invertito**: il TX del gateway va sull'RX della scheda.
2. **Indirizzo sbagliato**: controlla i dip-switch e il campo *Indirizzo* nella configurazione.
3. **Baudrate diverso**: il protocollo standard è 9600 8N1.
4. **RS485 senza pin DE/RE**: se il bus è RS485 il transceiver resta muto finché non abiliti DE/RE.
5. **Massa non comune** tra alimentatore del gateway e delle schede.

Prova con il polling grezzo (indirizzo 1):

```bash
curl -X POST http://sheltr.local/api/frame -H 'Content-Type: application/json' \
  -d '{"hex":"49 01 40 00 00 00 00 00 00 00 00 00 00 46"}'
```

Se `responseHex` è vuoto il problema è fisico o di indirizzo; se arriva una risposta ma i comandi
falliscono, controlla tipo di scheda e numero di canale.

### Lo stato nell'interfaccia non cambia

- Il polling ciclico è a 60 s: premi **Aggiorna** per forzarlo.
- Se il polling è impostato a 0 il gateway aggiorna lo stato solo dopo i comandi.
- Se una scheda risulta `offline` le sue tile restano all'ultimo valore noto.

### Home Assistant non vede le entità

1. In *Sistema* verifica «MQTT locale: connesso».
2. Controlla che il prefisso discovery coincida con quello dell'integrazione MQTT (default
   `homeassistant`).
3. Ascolta i topic: `mosquitto_sub -t 'homeassistant/#' -v`.
4. Se avevi entità vecchie, cancella i retained di discovery non più validi.

### Il portale Sheltr Cloud non sincronizza

- ID istanza sul portale e sul dispositivo devono coincidere.
- Il portale deve usare il profilo **Sheltr Mini**.
- Verifica il retained: `mosquitto_sub -t '<istanza>/config' -v`.

### OTA fallito

- Carica `sheltr-esp-firmware.bin` (applicazione), non `sheltr-esp-merged.bin`.
- Controlla lo spazio libero della partizione OTA in *Sistema*.
- Se il dispositivo si riavvia senza aggiornarsi, riflasha da seriale: la configurazione resta salvata su
  LittleFS.

### Ripartire da zero

*Sistema → Manutenzione → Ripristino configurazione* riporta ai valori di fabbrica mantenendo la rete.
Per cancellare davvero tutto (inclusa la configurazione salvata):

```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 erase_flash
```

## Segnalare un problema

Apri una issue su <https://github.com/filippobenozzi/sheltr-esp/issues> allegando:

- versione firmware (in *Sistema*),
- output di `curl http://sheltr.local/api/system`,
- log seriale dell'avvio,
- descrizione del bus (TTL o RS485, numero e tipo di schede).
