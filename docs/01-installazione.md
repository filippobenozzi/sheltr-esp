# 01 · Installazione

## Flash dal browser (consigliato)

1. Apri <https://filippobenozzi.github.io/sheltr-esp/> con **Chrome**, **Edge** o **Opera** su desktop
   (Web Serial non è disponibile su Safari, Firefox e browser mobili).
2. Collega la T-ETH-Lite ESP32S3 al computer.
3. Premi **Installa Sheltr ESP**, scegli la porta seriale e conferma.
4. Al termine la scheda si riavvia da sola: la pagina mostra anche il log seriale se lasci la porta aperta.

La pagina scrive l'immagine completa (`bootloader` + tabella partizioni + applicazione) a offset `0x0`,
quindi funziona anche su una scheda vergine.

## La scheda non compare tra le porte

La T-ETH-Lite ESP32S3 **non ha un convertitore USB-seriale a bordo**. Hai due strade:

| Metodo | Come |
|---|---|
| **USB nativo dell'ESP32-S3** | Collega `USB_D+` / `USB_D-` / `GND` / `5V` del connettore P2 a una presa USB. L'S3 espone il proprio USB-Serial/JTAG e appare come porta seriale. |
| **Programmatore esterno** | CH340 / CP2102 / FT232 sui pin `TXD` (GPIO43), `RXD` (GPIO44), `GND`, `5V` del connettore P3. |

Se il chip non entra in modalità download: tieni premuto `IO0`, premi e rilascia `RST`, poi rilascia `IO0`.

## Flash da riga di comando

```bash
pip install esptool
esptool.py --chip esp32s3 --port /dev/tty.usbserial-0001 --baud 921600 write_flash 0x0 sheltr-esp-merged.bin
```

Il file `sheltr-esp-merged.bin` si scarica dalla pagina di flash
(`/firmware/sheltr-esp-merged.bin`) o dalle [release](https://github.com/filippobenozzi/sheltr-esp/releases).

Per partire da una flash pulita:

```bash
esptool.py --chip esp32s3 --port /dev/tty.usbserial-0001 erase_flash
```

## Aggiornamento dalle release GitHub (consigliato)

Ogni push su `main` pubblica una release con i binari compilati. Se il gateway raggiunge internet, può
aggiornarsi da solo: *Sistema → Aggiornamento firmware* mostra la versione installata e il pulsante
**Controlla aggiornamenti**; se la release più recente è diversa da quella installata compare
**Scarica e aggiorna**, con barra di avanzamento e riavvio automatico al termine.

- Il confronto è sul **tag della release**: la CI lo incide nel binario (`SHELTR_FW_RELEASE`), quindi il
  dispositivo sa esattamente da quale release proviene. Un firmware compilato in locale si dichiara `dev`
  e vedrà quindi sempre un aggiornamento disponibile.
- Il controllo automatico gira ogni 12 ore (configurabile, 0 = solo manuale) e non fa nulla se il
  dispositivo è offline.
- Il download scrive nella partizione OTA inattiva da un task dedicato: l'interfaccia resta raggiungibile
  e mostra i byte scaricati. La configurazione salvata non viene toccata.
- Il repository controllato è `filippobenozzi/sheltr-esp` ed è modificabile (campo `update.repo`), utile
  se lavori su un fork.

> La connessione a GitHub è HTTPS **senza verifica del certificato**: senza orologio sincronizzato la
> validazione fallirebbe al primo avvio. L'integrità del binario è comunque verificata dall'ESP32 in
> scrittura (magic byte e checksum dell'immagine), quindi un file corrotto o incompleto non viene mai
> avviato. Se la rete non è affidabile, usa l'upload manuale del file scaricato a mano.

## Aggiornamento OTA manuale

Dall'interfaccia web: **Sistema → Aggiornamento firmware**, seleziona `sheltr-esp-firmware.bin`
(**non** il file `merged`) e premi *Aggiorna*. Il dispositivo scrive la partizione OTA inattiva e si
riavvia; la configurazione salvata resta intatta.

> **Nessuna sequenza di tasti**: BOOT+RST serve solo al flash da seriale. L'aggiornamento OTA passa dalla
> rete e non tocca la modalità download. La pagina mostra la **barra di avanzamento**, poi «scrittura
> della flash», quindi attende il riavvio e **si ricarica da sola** sulla nuova versione.

Se invece stai flashando **da seriale** e vuoi evitare la sequenza manuale a ogni upload, l'unica strada è
cablare le due linee di controllo del convertitore USB-seriale: **DTR → IO0** e **RTS → RST**. Con quelle
collegate, `esptool` e la pagina di flash mettono la scheda in download mode da sole. Senza, la sequenza va
fatta a mano perché la T-ETH-Lite non ha il circuito di auto-reset a bordo.

Da riga di comando:

```bash
curl -F "firmware=@sheltr-esp-firmware.bin" http://sheltr.local/api/system/ota
```

Con autenticazione attiva aggiungi `-H "Authorization: Bearer <token>"`.

## Layout della flash

| Partizione | Offset | Dimensione | Uso |
|---|---|---|---|
| `nvs` | `0x9000` | 20 kB | dati di sistema |
| `otadata` | `0xe000` | 8 kB | selezione partizione attiva |
| `app0` | `0x10000` | 4 MB | applicazione |
| `app1` | `0x410000` | 4 MB | applicazione OTA |
| `littlefs` | `0x810000` | ~7,9 MB | `config.json` |
| `coredump` | `0xFF0000` | 64 kB | dump in caso di crash |

L'interfaccia web non occupa spazio nel filesystem: viene compressa in gzip e incorporata nel binario
dell'applicazione, così un solo file basta per avere firmware e UI aggiornati insieme.
