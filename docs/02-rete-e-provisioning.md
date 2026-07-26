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

I profili orari hanno bisogno dell'ora esatta: il firmware la prende via **NTP** appena la rete è attiva
(`pool.ntp.org`, fuso `CET-1CEST,M3.5.0,M10.5.0/3` = Europa/Roma, con cambio ora automatico).
Server e fuso si cambiano in *Configurazione → Ora e sicurezza*. Senza rete internet l'orologio non si
sincronizza e i profili restano fermi: lo stato è visibile in *Sistema → Orologio*.
