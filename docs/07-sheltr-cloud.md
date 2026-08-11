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
| `<istanza>/bridge/status` | dispositivo → portale | `online` / `offline` (Last Will) |

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

## Preferiti e profili orari impostati dal cloud

Il portale pubblica preferiti e profili orari su **`<istanza>/settings`** (retained, QoS 1) con un
numero di **revisione**. Il gateway li applica alla configurazione locale, **salva su filesystem** e
ripubblica la propria configurazione, così il portale resta allineato.

- Le programmazioni le esegue **il gateway**: continuano a funzionare anche se il cloud è
  irraggiungibile (il portale, per queste istanze, non le esegue apposta, per non farle partire due volte).
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
