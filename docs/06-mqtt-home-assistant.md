# 06 · MQTT e Home Assistant

Il client MQTT locale pubblica gli accessori con la **discovery di Home Assistant**: appena connesso al
broker, le entità compaiono da sole, senza scrivere YAML.

## Configurazione

*Configurazione → MQTT Home Assistant*:

| Campo | Default | Note |
|---|---|---|
| Broker / Porta | — / 1883 | indirizzo del broker (per esempio il Mosquitto add-on di Home Assistant) |
| Utente / Password | — | credenziali del broker |
| Base topic | `sheltr` | radice di tutti i topic del gateway |
| Prefisso discovery | `homeassistant` | deve coincidere con quello configurato in Home Assistant |
| Intervallo stati | 30 s | ripubblicazione periodica; gli aggiornamenti dopo un comando sono immediati |

## Topic

Con base topic `sheltr` e scheda `luci-piano-terra`:

```
sheltr/bridge/status                            online | offline (LWT del gateway)
sheltr/luci-piano-terra/availability            online | offline (per scheda)
sheltr/luci-piano-terra/ch1/state               ON | OFF
sheltr/luci-piano-terra/ch1/set                 comando: ON | OFF | TOGGLE
sheltr/luci-piano-terra/ch1/attributes          {"board_id","address","channel","room","kind"}
sheltr/luci-piano-terra/poll/set                POLL → interroga la scheda
sheltr/poll_all/set                             POLL → interroga tutte le schede
sheltr/service/restart/set                      RESTART → riavvia il gateway
sheltr/sequence/<id>/set                        RUN → esegue una sequenza del sequencer
```

Per tipo di scheda:

| Tipo | Topic aggiuntivi | Valori |
|---|---|---|
| `light` | — | `ON` / `OFF` |
| `dimmer` | `chN/brightness/state`, `chN/brightness/set` | 0–255 (convertiti in 0–9) |
| `shutter` | — | comandi `OPEN` / `CLOSE` / `STOP`, stato `OPENING` / `CLOSING` / `STOP` |
| `thermostat` | `chN/mode/{state,set}`, `chN/temperature/{state,set}`, `chN/setpoint/state`, `chN/action/state`, `chN/power/state` | modalità `off` / `heat` / `cool` |

## Entità create in Home Assistant

| Scheda | Entità | Funzioni |
|---|---|---|
| `light` | `light.*` | accensione/spegnimento |
| `dimmer` | `light.*` con luminosità | on/off e livello 0–9 mappato su 0–255 |
| `shutter` | `cover.*` | su / giù / stop (stato assunto) |
| `thermostat` | `climate.*` | setpoint 5–30 °C passo 0,5, inverno = heat, estate = cool, temperatura ambiente e stato del relè |
| ogni scheda | `button.*` Polling | forza una lettura `0x40` |
| ogni sequenza | `button.*` con il nome della scena | esegue la sequenza (`<base>/sequence/<id>/set` ← `RUN`) |
| gateway | `button.*` Polling tutte le schede, Riavvia gateway | manutenzione |

Ogni scheda diventa un **dispositivo** Home Assistant collegato al gateway (`via_device`), con nome,
modello e versione firmware. Gli attributi di ogni entità riportano `board_id`, `address`, `channel` e
`room`, così puoi ricreare le stanze anche lato Home Assistant.

## Verifica rapida

```bash
# stato pubblicato dal gateway
mosquitto_sub -h 192.168.1.10 -u utente -P password -t 'sheltr/#' -v

# accendi una luce
mosquitto_pub -h 192.168.1.10 -u utente -P password -t 'sheltr/luci-piano-terra/ch1/set' -m ON

# forza il polling di tutte le schede
mosquitto_pub -h 192.168.1.10 -u utente -P password -t 'sheltr/poll_all/set' -m POLL
```

## Rimuovere le entità

I messaggi di discovery sono retained: per cancellare una scheda eliminata pubblica un payload vuoto sul
topic di configurazione corrispondente, per esempio

```bash
mosquitto_pub -h 192.168.1.10 -r -n -t 'homeassistant/light/sheltr_luci-piano-terra_ch1/config'
```

## Due strade verso Home Assistant

- **MQTT (questo documento)**: nessuna dipendenza, entità native, funziona anche se il gateway è isolato
  da internet.
- **Integrazione [sheltr-homeassistant](https://github.com/filippobenozzi/sheltr-homeassistant)**: punta
  l'integrazione a `http://sheltr.local` usando le rotte `/api/instances/<id>/…` (vedi
  [05 · API](05-api.md)). Utile se usi già l'integrazione con il portale cloud e vuoi la stessa
  esperienza in locale.

Le due strade possono convivere, ma creano entità distinte: scegline una per evitare doppioni.
