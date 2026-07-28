# 02 · Rete e provisioning

Il gateway può stare in rete via **Ethernet**, via **WiFi**, o entrambi. In assenza di connettività apre
un **hotspot di configurazione**.

## Ethernet (W5500)

- Attiva di fabbrica, indirizzo in **DHCP**.
- Appena ottiene l'indirizzo, registra il nome mDNS: `http://sheltr.local`.
- Per usare un IP statico: *Configurazione → Rete*, togli la spunta «Indirizzo automatico (DHCP)» e
  compila IP, gateway, netmask e DNS. Il valore vale sia per Ethernet sia per il WiFi.

Se il cavo non è collegato o non c'è DHCP il led del connettore resta spento e la UI mostra
«Ethernet: non connessa».

## Hotspot e captive portal (primo avvio)

Se dopo **20 secondi** dall'accensione non c'è né Ethernet né WiFi:

1. Il dispositivo accende l'access point `Sheltr-XXXX` (XXXX = ultime cifre del MAC), password
   `sheltr1234`.
2. Un DNS interno risponde a qualunque dominio con `192.168.4.1`, quindi telefoni e computer aprono
   automaticamente il portale.
3. Da *Configurazione → Rete → WiFi* scegli la rete (`Cerca` elenca quelle visibili), inserisci la
   password e premi **Connetti ora**.
4. Appena la connessione riesce l'hotspot si spegne e il dispositivo resta raggiungibile su
   `http://sheltr.local` (o sull'IP mostrato nell'intestazione).

SSID, password e comportamento dell'hotspot si cambiano in *Configurazione → Rete → Hotspot*. Se
disattivi «Apri l'hotspot se non c'è connettività» il dispositivo resterà accessibile solo via cavo:
usalo soltanto su installazioni fisse.

## Dominio locale `sheltr.local`

Il nome è ricavato dal campo **Hostname** (default `sheltr`). Cambiandolo in `casa` il dispositivo
risponderà su `http://casa.local`.

- macOS, iOS e Linux con Avahi risolvono il nome senza configurazione.
- Windows 10/11 supporta mDNS; se non funziona usa direttamente l'indirizzo IP.
- Android non risolve `.local` in tutti i browser: in quel caso usa l'IP (lo trovi in *Sistema*).

Il servizio pubblicato è `_http._tcp` sulla porta 80, con TXT `device=sheltr-esp` e `version=<firmware>`.

## Comandi utili

```bash
# scoperta del dispositivo in rete
dns-sd -B _http._tcp                # macOS
avahi-browse -rt _http._tcp         # Linux

# stato rete completo
curl http://sheltr.local/api/system | jq .network

# elenco reti WiFi viste dal dispositivo
curl http://sheltr.local/api/wifi/scan | jq .

# connessione a una rete WiFi
curl -X POST http://sheltr.local/api/wifi/connect \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"CasaMia","password":"segretissima"}'
```

## Orologio

I profili orari e gli avvii a orario delle sequenze hanno bisogno dell'ora esatta: il firmware la prende
via **NTP** appena la rete è attiva (`pool.ntp.org`, fuso `CET-1CEST,M3.5.0,M10.5.0/3` = Europa/Roma, con
cambio ora automatico). Server e fuso si cambiano in *Sistema → Ora e sicurezza*.

### Orologio hardware DS3231

Con un RTC l'ora resta corretta anche senza internet e dopo un blackout. Si configura in
*Sistema → Orologio RTC (DS3231)*.

Collegamento (I2C, default modificabile):

| DS3231 | Scheda | Note |
|---|---|---|
| VCC | 3V3 | il modulo funziona anche a 3,3 V |
| GND | GND | |
| SDA | **GPIO15** | configurabile |
| SCL | **GPIO16** | configurabile |

Indirizzo I2C `0x68` (104 in decimale), fisso sul DS3231.

Come lavora il firmware:

1. **All'avvio** legge l'ora dall'RTC e la imposta come ora di sistema: i profili funzionano subito, anche
   senza rete.
2. **Quando NTP sincronizza**, riscrive l'ora sull'RTC (e poi la riallinea ogni ora), così il modulo resta
   sempre aggiornato.
3. Se l'RTC ha perso l'alimentazione, il flag interno lo segnala e l'ora viene ignorata finché non la
   imposti.

Dal pannello puoi anche forzare le due direzioni — **Leggi ora dall'RTC** e **Scrivi ora sull'RTC** — e
impostare data e ora **manualmente**, utile in un impianto senza internet: il valore viene scritto sia nel
sistema sia nel DS3231. Il pannello mostra stato del chip, ora conservata e temperatura interna.

```bash
TOKEN=$(curl -s -X POST http://sheltr.local/api/system/unlock \
  -H 'Content-Type: application/json' -d '{"password":"Algo1962"}' | jq -r .token)

# imposta manualmente data e ora
curl -X POST http://sheltr.local/api/system/rtc -H "X-Sheltr-System: $TOKEN" \
  -H 'Content-Type: application/json' -d '{"action":"set","time":"2026-07-26T21:35:00"}'
```
