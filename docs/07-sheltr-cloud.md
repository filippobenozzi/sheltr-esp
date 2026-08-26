# 07 · Integrazione con Sheltr Cloud

Il gateway può registrarsi su un portale [Sheltr Cloud](https://github.com/filippobenozzi/sheltr-cloud)
comportandosi come un dispositivo con profilo **Sheltr Mini**: pubblica la propria configurazione, riceve
i frame del protocollo e restituisce le risposte delle schede.

Il funzionamento locale resta indipendente: se il cloud non è raggiungibile, interfaccia web, MQTT e
profili orari continuano a funzionare.

## Associazione rapida con codice (consigliato)

Invece di inserire a mano broker, credenziali e ID istanza, usa un **codice di associazione**:

1. Nel portale crea un'istanza di tipo **Sheltr ESP** e, in *Config → istanza → Associazione dispositivo*,
   premi **Genera codice** (`SHLTR-XXXXX-XXXXX-XXXXX`).
2. Sul dispositivo, *Configurazione → Sheltr Cloud*: inserisci l'**URL del portale** e il **codice**, poi
   premi **Abbina al portale**.
3. Il dispositivo chiama `POST <portale>/api/provision/claim`, riceve broker, credenziali MQTT e ID
   istanza, si configura da solo e si collega. La sua configurazione viene importata dal portale.

L'endpoint interno del dispositivo è `POST /api/cloud/claim` con `{portalUrl, code}`.

## Collegamento tramite modulo USR DR154 (senza rete propria)

Se il gateway non ha né Ethernet né WiFi utilizzabili, la connettività gliela può dare un
**USR DR154** collegato in RS485 alla seriale (di default **GPIO1 = TX**, **GPIO3 = RX**): il gateway
scrive sulla seriale, il modulo pubblica quei byte su un topic MQTT del portale e gli consegna sulla
seriale quello che arriva dall'altro topic.

In questa configurazione il gateway resta **la fonte di verità**: profili orari, sequenze, preferiti e
ingressi li salva su filesystem e li esegue lui, anche quando il modulo non è raggiungibile. Il DR154
fa solo da trasporto.

### Cosa impostare, una volta sola

1. Nel portale: *Config → istanza → Collegamento del gateway* → **Modulo USR DR154**. La pagina mostra
   pronti da copiare broker, porta, utente, password, client id e i **due topic dell'istanza**
   (`<istanza>/link/up` e `<istanza>/link/down`). Sono di quella casa soltanto: ogni casa ha i suoi,
   così i dispositivi restano separati anche sullo stesso broker.
2. Nel modulo USR: modalità **MQTT con trasmissione trasparente**, quei valori, e la seriale a
   **115200 8N1** (o il baud che imposti anche sul gateway).
3. Sul gateway: *Configurazione → Sheltr Cloud* → collegamento **Modulo USR DR154**, incolla il
   **codice di associazione** e premi *Abbina al portale*.

Il codice non può viaggiare via HTTPS (il gateway non naviga): parte sul collegamento seriale e il
gateway insiste finché il portale non risponde, perché il modulo potrebbe non essere ancora collegato
al broker. Alla risposta il gateway impara ID e nome istanza e pubblica la propria configurazione.

### Come viaggiano i messaggi

Sulla seriale c'è un solo canale, mentre il protocollo del portale usa più topic: ogni messaggio viene
quindi incapsulato con il nome del **sotto-topic**, che è lo stesso del collegamento diretto
(`cmd`, `pub`, `config`, `settings`, `action`, `bridge/status`).

```
!S1|<sub>|<msgId>|<idx>|<cnt>|<base64>|<crc16>#
```

- il contenuto viaggia in **base64**: nessun byte può essere scambiato per un delimitatore;
- **CRC-16/CCITT-FALSE** su tutta la trama: una trama corrotta viene scartata, mai applicata a metà;
- i messaggi grandi (la configurazione supera i 5 KB) vengono **spezzati** in trame da 192 byte, perché
  il modulo ha un buffer seriale limitato; chi riceve li rimette insieme e si risincronizza da solo
  cercando il marcatore di inizio, anche se il modulo taglia o unisce i pacchetti.

Il codec sta in [`src/link_codec.cpp`](../src/link_codec.cpp) e ha una gemella identica nel portale
(`webapp/link_codec.py`): le due implementazioni producono le stesse trame byte per byte. Si verifica
sull'host, senza hardware:

```bash
g++ -std=c++17 -O2 -I src test/link_codec_host_test.cpp src/link_codec.cpp -o /tmp/link_test && /tmp/link_test
```

> Il portale conferma ogni messaggio ricevuto (sotto-topic `ack`): è l'unico modo che ha il gateway di
> sapere che il ponte funziona davvero, perché la seriale da sola non dice se il modulo sia collegato
> al broker. In *Sistema → Sheltr Cloud* trovi trame inviate/ricevute, errori CRC e quanto tempo fa è
> arrivata l'ultima risposta.

## Configurazione manuale sul dispositivo

*Configurazione → Sheltr Cloud → Configurazione manuale (avanzata)*:

| Campo | Esempio | Note |
|---|---|---|
| Broker cloud | `mqtt.miodominio.it` | broker del portale |
| Porta | `1883` | |
| Utente / Password | `filippo` / … | credenziali MQTT del portale |
| ID istanza | `casa-demo` | deve coincidere con l'istanza creata nel portale |
| Nome istanza | `Casa Demo` | mostrato nel portale |
| Formato payload | `frame_hex_space_crlf` | formato dei frame in uscita |

## Topic usati

| Topic | Direzione | Contenuto |
|---|---|---|
| `<istanza>/config` | dispositivo → portale | JSON retained con schede, canali, stanze, sequenze, ingressi e topic |
| `<istanza>/cmd` | portale → dispositivo | frame protocollo 1.6 (binario o esadecimale) |
| `<istanza>/settings` | portale → dispositivo | JSON retained con preferiti e profili orari impostati dal cloud |
| `<istanza>/pub` | dispositivo → portale | frame di risposta letto dal bus |
| `<istanza>/event` | dispositivo → portale | eventi (es. scatto di un ingresso) |
| `<istanza>/action` | portale → dispositivo | azioni immediate (avvio/stop sequenze) |
| `<istanza>/bridge/status` | dispositivo → portale | `online` / `offline` (Last Will) |
| `<istanza>/link/up` · `<istanza>/link/down` | solo con modulo USR DR154 | gli stessi messaggi qui sopra, incapsulati col nome del sotto-topic |

Il payload di configurazione ha la stessa forma del firmware Sheltr Mini:

```json
{
  "id": "casa-demo",
  "name": "Casa Demo",
  "deviceType": "sheltr_mini",
  "device": { "type": "sheltr_esp", "board": "T-ETH-Lite-ESP32S3", "firmware": "0.1.0" },
  "protocolVersion": "1.6",
  "boards": [ { "id": "board-1", "name": "Luci", "address": 1, "kind": "light",
                "channelStart": 1, "channelEnd": 8,
                "channels": [ { "channel": 1, "name": "Faretti", "room": "Cucina" } ] } ],
  "devices": [ { "id": "board-1-c1", "kind": "light", "boardId": "board-1", "address": 1,
                 "channel": 1, "name": "Faretti", "room": "Cucina" } ],
  "mqtt": { "baseTopic": "casa-demo", "configTopic": "casa-demo/config",
            "lightCommandTopic": "casa-demo/cmd", "lightResponseTopic": "casa-demo/pub",
            "lightPayloadFormat": "frame_hex_space_crlf" }
}
```

## Configurazione sul portale

1. Crea una nuova istanza con lo stesso ID (`casa-demo`).
2. Seleziona il tipo dispositivo **Sheltr Mini**: il portale imposta da solo i topic
   `casa-demo/config`, `casa-demo/cmd`, `casa-demo/pub` e il formato `frame_hex_space_crlf`.
3. Premi **Sincronizza Sheltr Mini**: il portale legge il retained pubblicato dal gateway e importa
   schede, canali e stanze.
4. Da quel momento i comandi del portale arrivano come frame su `casa-demo/cmd` e il gateway risponde su
   `casa-demo/pub`.

## Polling automatico delle schede

Il gateway interroga da solo le schede sul bus ogni *N* secondi (default **60**) per accorgersi dei
comandi dati fuori dal portale: pulsanti a muro, altri sistemi, scenari locali. Si imposta
indifferentemente in *Sistema → Bus → Polling automatico* sul dispositivo oppure in
*Config → istanza → Polling automatico delle schede* sul portale: il valore è uno solo e si
sincronizza nei due sensi (0 = disattivato).

## Preferiti e profili orari impostati dal cloud

Il portale pubblica preferiti e profili orari su **`<istanza>/settings`** (retained, QoS 1) con un
numero di **revisione**. Il gateway li applica alla configurazione locale, **salva su filesystem** e
ripubblica la propria configurazione, così il portale resta allineato.

- Le programmazioni le esegue **il gateway**: continuano a funzionare anche se il cloud è
  irraggiungibile (il portale, per queste istanze, non le esegue apposta, per non farle partire due volte).
- Oltre a preferiti e profili viaggiano anche la **stanza** di canali, ingressi e sequenze, il **testo
  delle notifiche** degli ingressi e l'**intervallo di polling**: si impostano da una parte o dall'altra
  e restano allineati.
- La revisione viene memorizzata (`cloud.settingsRevision`) e sopravvive al riavvio: un messaggio
  retained più vecchio dell'ultima revisione applicata viene ignorato, così non sovrascrive modifiche
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

> Nel portale imposta anche *Config → istanza → Heartbeat dispositivo* con un valore
> **non inferiore** all'intervallo qui: è quello che il portale usa per decidere se
> mostrare il banner "Dispositivo offline".

Alla riconnessione il gateway pubblica `online` (retained) su `<istanza>/bridge/status` **prima** di
ripubblicare configurazione e stati. Il portale usa quel messaggio, insieme al Last Will `offline`, per
capire che c'è stata un'interruzione: lo stato che arriva subito dopo lo adotta in **silenzio**, senza
inviare notifiche, perché è un riallineamento e non un elenco di eventi appena accaduti (dopo un
blackout, per esempio, il modulo riparte con i relè a riposo).

## Come vengono trattati i frame

Il bridge cloud è volutamente **trasparente**: il frame ricevuto viene inoltrato verbatim sul bus e la
risposta della scheda viene ripubblicata senza reinterpretazioni. In questo modo qualsiasi comando
supportato dal portale (anche futuro) funziona senza aggiornare il firmware. Se il frame è un polling
`0x40`, il gateway ne approfitta per aggiornare anche il proprio stato interno, così interfaccia locale e
Home Assistant restano allineati ai comandi arrivati dal cloud.

## Diagnostica

- *Sistema → Sheltr Cloud* mostra stato della connessione, ultimo errore e se la configurazione retained
  è stata pubblicata.
- Sul broker:

```bash
mosquitto_sub -h mqtt.miodominio.it -u utente -P password -t 'casa-demo/#' -v
```

- Se il portale non vede il dispositivo: controlla che l'ID istanza coincida, che le credenziali MQTT
  siano corrette e che il broker sia raggiungibile dalla rete del gateway (porta 1883 in uscita).
