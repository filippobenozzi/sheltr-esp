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

## Aggiornamento OTA

Dall'interfaccia web: **Sistema → Aggiornamento firmware**, seleziona `sheltr-esp-firmware.bin`
(**non** il file `merged`) e premi *Aggiorna*. Il dispositivo scrive la partizione OTA inattiva e si
riavvia; la configurazione salvata resta intatta.

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
