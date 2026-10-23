# 07 · Integrazione con Sheltr Cloud

Il gateway si collega al portale [Sheltr Cloud](https://github.com/filippobenozzi/sheltr-cloud) con una
connessione TCP verso il suo gateway dispositivi (o attraverso un modulo USR DR154): pubblica la propria
configurazione, riceve i frame del protocollo e restituisce le risposte delle schede.

Il funzionamento locale resta indipendente: se il cloud non è raggiungibile, interfaccia web, MQTT e
profili orari continuano a funzionare.

## Collegamento e abbinamento

Niente codici, broker o credenziali da copiare:

1. Sul gateway, *Configurazione → Sheltr Cloud*: scrivi l'**indirizzo del portale** (es.
   `cloud.miodominio.it`) e premi **Collega al portale**.
2. Il gateway apre una connessione TCP verso il gateway dispositivi del portale (porta **7700**) e si
   presenta col suo identificativo hardware. La pagina mostra il **codice** del gateway, per esempio
   `ESP-A1B2C3`, e lo stato *In attesa*.
3. Nel portale: casa → *Gestisci → Dispositivi → Aggiungi dispositivo*: il gateway è nell'elenco dei
   dispositivi in attesa col suo codice. Lo si sceglie e basta.
4. Il portale risponde al gateway con l'identificativo della casa e una **chiave** che il gateway salva e
   presenta a ogni connessione successiva: nessuno può spacciarsi per lui conoscendone il codice.

Se il gateway viene tolto dalla casa nel portale torna *in attesa* e dimentica la chiave vecchia. Se il
portale lo **rifiuta** (chiave diversa, per esempio dopo un ripristino di fabbrica del gateway) basta
toglierlo dalla casa nel portale e aggiungerlo di nuovo.

## Collegamento tramite modulo USR DR154 (senza rete propria)

Se il gateway non ha né Ethernet né WiFi utilizzabili, la connettività gliela può dare un
**USR DR154** collegato in RS485 alla seriale (di default **GPIO1 = TX**, **GPIO3 = RX**): il gateway
scrive sulla seriale e il modulo porta quei byte, in TCP, allo stesso gateway del portale.

1. Nel modulo USR: la **configurazione unica** che il portale mostra in *Installazione*, variante
   **Con Sheltr ESP** (seriale **115200 8N1**, o il baud che imposti anche qui). È la stessa per tutti i
   moduli.
2. Sul gateway: *Come raggiunge il portale* → **Modulo USR DR154**, poi **Collega al portale**.
3. Nel portale il gateway compare fra i dispositivi in attesa con l'**IMEI del modulo**.

In questa configurazione il gateway resta **la fonte di verità**: profili orari, sequenze, preferiti e
ingressi li salva su filesystem e li esegue lui, anche quando il modulo non è raggiungibile. Il DR154
fa solo da trasporto.

### Come viaggiano i messaggi

C'è un solo canale (la connessione TCP o la seriale), mentre il protocollo usa più tipi di messaggio:
ognuno viene quindi incapsulato con il nome del **sotto-topic** (`hello`, `claimed`, `cmd`, `pub`,
`config`, `settings`, `action`, `event`, `bridge/status`, `ping`, `ack`). Le due strade usano lo stesso
identico formato.

```
!S1|<sub>|<msgId>|<idx>|<cnt>|<base64>|<crc16>#
```

- il contenuto viaggia in **base64**: nessun byte può essere scambiato per un delimitatore;
- **CRC-16/CCITT-FALSE** su tutta la trama: una trama corrotta viene scartata, mai applicata a metà;
- i messaggi grandi (la configurazione supera i 5 KB) vengono **spezzati** in trame da 192 byte, perché
  il modulo ha un buffer seriale limitato; chi riceve li rimette insieme e si risincronizza da solo
  cercando il marcatore di inizio, anche se il modulo taglia o unisce i pacchetti.

Il codec sta in [`src/link_codec.cpp`](../src/link_codec.cpp) e ha una gemella identica nel portale
(`backend/src/Protocol/LinkCodec.php`): le due implementazioni producono le stesse trame byte per byte. Si verifica
sull'host, senza hardware:

```bash
g++ -std=c++17 -O2 -I src test/link_codec_host_test.cpp src/link_codec.cpp -o /tmp/link_test && /tmp/link_test
```

> Il portale conferma ogni messaggio ricevuto (sotto-topic `ack`), anche il `ping` che il gateway manda
> ogni minuto a linea ferma: è così che il gateway sa che il portale lo sente davvero (con il DR154 la
> seriale da sola non dice se il modulo sia collegato). In *Sistema → Sheltr Cloud* trovi trame inviate/ricevute, errori CRC e quanto tempo fa è
> arrivata l'ultima risposta.

## Messaggi

| Sotto-topic | Direzione | Contenuto |
|---|---|---|
| `hello` | dispositivo → portale | identificativo hardware, chiave (se abbinato), firmware |
| `claimed` | portale → dispositivo | abbinato (id e nome della casa, chiave la prima volta), in attesa, oppure rifiutato |
| `ping` / `ack` | entrambe | presenza della linea / conferma di ricezione |
| `config` | dispositivo → portale | JSON con schede, canali, stanze, sequenze, ingressi e revisione delle impostazioni |
| `cmd` | portale → dispositivo | frame protocollo 1.6 (binario o esadecimale) |
| `settings` | portale → dispositivo | JSON con preferiti, notifiche, profili, stanze e sequenze impostati dal cloud (rimandato a ogni ritorno online) |
| `pub` | dispositivo → portale | frame di risposta letto dal bus |
| `event` | dispositivo → portale | eventi (es. scatto di un ingresso) |
| `action` | portale → dispositivo | azioni immediate (avvio/stop sequenze) |
| `bridge/status` | dispositivo → portale | `online` dopo ogni abbinamento confermato (l'uscita la rileva il portale dalla connessione) |

Il payload di configurazione:

```json
{
  "id": "sx2y201mcw",
  "name": "Casa Demo",
  "deviceType": "sheltr_mini",
  "device": { "type": "sheltr_esp", "board": "T-ETH-Lite-ESP32S3", "firmware": "0.1.0" },
  "protocolVersion": "1.6",
  "boards": [ { "id": "board-1", "name": "Luci", "address": 1, "kind": "light",
                "channelStart": 1, "channelEnd": 8,
                "channels": [ { "channel": 1, "name": "Faretti", "room": "Cucina" } ] } ],
  "devices": [ { "id": "board-1-c1", "kind": "light", "boardId": "board-1", "address": 1,
                 "channel": 1, "name": "Faretti", "room": "Cucina" } ],
  "pollIntervalSec": 60,
  "settingsRevision": 1792771017,
  "payloadFormat": "frame_hex_space_crlf"
}
```

## Configurazione sul portale

Nessuna: il portale importa la configurazione pubblicata dal gateway (schede, canali, stanze, colori,
sequenze, ingressi, intervallo di polling) e la aggiorna a ogni nuova pubblicazione. Nomi, schede e
sequenze si cambiano qui sul gateway; dal portale si cambiano preferiti, notifiche, profili orari,
stanze e colori, che vengono rimandati al gateway.

## Polling automatico delle schede

Il gateway interroga da solo le schede sul bus ogni *N* secondi (default **60**) per accorgersi dei
comandi dati fuori dal portale: pulsanti a muro, altri sistemi, scenari locali. Si imposta
indifferentemente in *Sistema → Bus → Polling automatico* sul dispositivo oppure sul portale
(*Gestisci → Dispositivi → il gateway → Avanzate → Lettura automatica delle schede*): il valore è uno
solo e si sincronizza nei due sensi (0 = disattivato).

## Preferiti e profili orari impostati dal cloud

Il portale pubblica preferiti e profili orari su **`settings`** (QoS 1, rimandati a ogni
ritorno online del gateway) con un numero di **revisione**. Il gateway li applica alla configurazione locale, **salva su filesystem** e
ripubblica la propria configurazione, così il portale resta allineato.

- Le programmazioni le esegue **il gateway**: continuano a funzionare anche se il cloud è
  irraggiungibile (il portale, per queste istanze, non le esegue apposta, per non farle partire due volte).
- Oltre a preferiti e profili viaggiano anche la **stanza** di canali, ingressi e sequenze, il **testo
  delle notifiche** degli ingressi e l'**intervallo di polling**: si impostano da una parte o dall'altra
  e restano allineati.
- La revisione viene memorizzata (`cloud.settingsRevision`) e sopravvive al riavvio: un messaggio
  più vecchio dell'ultima revisione applicata viene ignorato, così non sovrascrive modifiche
  fatte nel frattempo sul dispositivo.
- Vengono toccati solo i canali indicati: schede, nomi e stanze restano gestiti in locale.

## Heartbeat e aggiornamento del portale

Il gateway ripubblica lo stato reale delle schede sul topic di risposta (interroga il
bus con un frame `0x40` e inoltra la risposta), che è esattamente ciò che il portale
interpreta. Serve a due cose:

- **A ogni cambiamento locale** (comando dall'interfaccia, ingresso, sequenza, profilo
  orario) il portale viene avvisato subito, così aggiorna le card e, se il canale ha la
  notifica attiva, la invia. Gli invii ravvicinati vengono accorpati (minimo 3 secondi).
- **Heartbeat periodico**, regolabile in *Sistema → Heartbeat Sheltr Cloud*
  (`cloud.heartbeatSec`, default 300 s, 0 = disattivato): il portale sa che il gateway
  è vivo anche quando non cambia nulla.

> Nel portale (*Gestisci → Dispositivi → il gateway → Avanzate → Heartbeat atteso*) il valore
> deve essere **non inferiore** all'intervallo qui: senza segnali per il doppio di quel tempo il
> portale considera il gateway offline.

A ogni nuova connessione il gateway si ripresenta (`hello`) e, confermato dal portale, manda `online`
su `bridge/status` **prima** di ripubblicare configurazione e stati. Il portale usa quel messaggio, insieme
alla caduta della connessione (che rileva da sé), per capire che c'è stata un'interruzione: lo stato che arriva subito dopo lo adotta in **silenzio**, senza
inviare notifiche, perché è un riallineamento e non un elenco di eventi appena accaduti (dopo un
blackout, per esempio, il modulo riparte con i relè a riposo).

## Come vengono trattati i frame

Il bridge cloud è volutamente **trasparente**: il frame ricevuto viene inoltrato verbatim sul bus e la
risposta della scheda viene ripubblicata senza reinterpretazioni. In questo modo qualsiasi comando
supportato dal portale (anche futuro) funziona senza aggiornare il firmware. Se il frame è un polling
`0x40`, il gateway ne approfitta per aggiornare anche il proprio stato interno, così interfaccia locale e
Home Assistant restano allineati ai comandi arrivati dal cloud.

## Notifiche quando il portale non c'e'

Le notifiche le manda **il portale**: conosce gli indirizzi e le preferenze di ogni utente e le
recapita via push, email e Telegram. Se pero' il gateway lavora da solo — cloud non configurato,
oppure configurato ma irraggiungibile — e ha comunque una connessione a internet, puo' mandarle lui
**via email**, agli indirizzi impostati in *Sistema → Notifiche email*.

La regola e' una sola, cosi' non arrivano doppioni e non si resta scoperti:

| Situazione | Chi manda la notifica |
|---|---|
| Gateway collegato a Sheltr Cloud | il portale (push, email, Telegram) |
| Portale non configurato o irraggiungibile, ma c'e' internet | il gateway, via email |
| Nessuna connessione a internet | nessuno (il gateway continua comunque a funzionare in locale) |

Cosa fa scattare una email dal gateway: il cambio di stato di un canale con **Notifica cambio stato**
attiva (lo stesso flag che si sincronizza col portale) e lo scatto di un **ingresso** con la notifica
attiva, col suo testo personalizzato. I cambiamenti ravvicinati finiscono in un'unica email.

### Cosa impostare

*Sistema → Notifiche email*: server SMTP, porta, utente e password, indirizzo del mittente e i
destinatari (uno o piu', separati da virgola). Il pulsante **Invia prova** salva e prova subito,
riportando per intero l'errore del server di posta.

> **Serve SSL/TLS diretto** (di solito porta 465). STARTTLS non c'e': vorrebbe dire cifrare una
> connessione gia' aperta, e la libreria TLS dell'ESP32 non lo permette. Il certificato del server non
> viene verificato (sul dispositivo non c'e' un archivio di autorita' da aggiornare): il traffico e'
> cifrato, ma conviene usare credenziali dedicate a questo gateway.

L'invio avviene in un task separato con una piccola coda: una sessione SMTP con TLS puo' durare
qualche secondo e il loop principale deve restare libero per bus, web e MQTT. Se la coda si riempie la
notifica viene scartata (meglio perderne una che bloccare il gateway) e il contatore lo segnala in
*Sistema → Notifiche email*.

Il dialogo SMTP sta in [`src/smtp_session.cpp`](../src/smtp_session.cpp), scritto senza dipendenze
Arduino apposta: si compila sull'host e si prova contro un vero server di posta.

```bash
python3 test/fake_smtp_server.py --auth &
g++ -std=c++17 -O2 -I src test/smtp_session_host_test.cpp src/smtp_session.cpp -o /tmp/smtp_test && /tmp/smtp_test
```

## Diagnostica

- *Configurazione → Sheltr Cloud* mostra il codice del gateway e lo stato: non collegato, collegamento
  in corso (con l'ultimo errore), in attesa di essere aggiunto, collegato alla casa, rifiutato.
- *Sistema* riporta trame inviate e ricevute, errori CRC e da quanto tempo è arrivata l'ultima risposta
  del portale.
- Se il portale non vede il gateway: controlla l'indirizzo e che la porta **7700** in uscita sia aperta
  sulla rete del gateway.
