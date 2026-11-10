# Playmaker Wi-Fi LAN

Branch `wifi`. Il firmware serve tabellone e configurazione via HTTP; il controller
Bluetooth e NimBLE sono disabilitati. Non serve un server sul PC, né Web Bluetooth.

## Prima configurazione

1. Accendi o risveglia il wearable.
2. Dal PC o telefono collegati al Wi-Fi `PLAYMAKER_XXXX`, password `playmaker`.
3. Apri `http://192.168.4.1/settings`.
4. Seleziona una delle reti rilevate a **2,4 GHz**, inserisci la password e premi
   **Collega e memorizza**. L'AP rimane disponibile anche durante il collegamento.
5. Attendi la conferma e annota l'indirizzo LAN mostrato dalla pagina.
6. Ricollega il PC alla stessa LAN e apri quell'indirizzo: il tabellone si collega
   automaticamente. Non usare più la vecchia pagina su `localhost:8000`.

L'AP di configurazione rimane disponibile, anche se il router non è raggiungibile.
UNDO lungo mantiene sveglio il dispositivo e dà una conferma sonora; non occorre
aprire finestre di pairing o cancellare credenziali Bluetooth.

## Rete preferita e fallback

- All'avvio viene provata l'ultima rete configurata e collegata con successo.
- Se non funziona, viene provata la rete `Se`, password `ciao1234`.
- Senza configurazione si prova direttamente `Se`.
- Una nuova rete viene salvata in NVS soltanto dopo aver ottenuto un IP.
- Una password errata non sostituisce la precedente rete valida.
- Il fallback non cancella la rete preferita: al prossimo avvio si riprova quella.
- Se nessuna rete è raggiungibile, il firmware riprova ogni 30 secondi; l'AP
  consente sempre di scegliere una nuova rete.
- Reti enterprise con nome utente/certificati e captive portal non sono supportate.
- Finché un dispositivo è collegato all'AP (tabellone in modalità diretta) la
  ricerca periodica della LAN è sospesa: la radio è una sola e ogni ricerca
  toglie l'AP dal suo canale per alcuni secondi, facendo scadere i comandi.
  Riprende entro 30 secondi da quando l'AP resta libero. Una rete inviata dalla
  pagina di configurazione viene comunque provata subito.
- Al risveglio, se l'ultima volta l'AP era in uso, la prima ricerca LAN attende
  fino a 5 secondi che il tabellone si ricolleghi all'AP.

L'indirizzo LAN dipende dal DHCP del router. È visibile nella configurazione e nel
log USB (`LAN ready: http://.../`). AP e LAN condividono le stesse pagine e API.
Se il router isola i client della rete ospiti, usare una LAN che consenta ai
dispositivi di comunicare tra loro.

## Comandi e affidabilità

Il punteggio resta autorevole nel browser. Il wearable mantiene una coda FIFO di
16 azioni e invia lo stesso identificativo `(session_id, sequence)` al massimo
tre volte, aspettando un secondo fra un tentativo e l'altro. Dopo l'ultimo
secondo senza ACK il comando viene scartato e il buzzer segnala l'errore: una
nuova pressione crea un nuovo comando. Senza polling dal PC, un comando in
testa scade comunque dopo tre secondi. Un ACK già ricevuto ferma subito i retry.
I pacchetti binari ACTION/ACK riutilizzano il formato v6 descritto
in `ble-protocol.md`, ora trasportato su HTTP.

- `POST /api/session`: prende il controllo del wearable.
- `GET /api/action`: restituisce la prima azione da confermare, oppure HTTP 204.
- `POST /api/ack`: conferma l'azione e attiva il feedback sonoro.
- `GET /api/network`: nome, stato, SSID e indirizzo LAN, senza password.
- `GET /api/networks`: scansione delle reti disponibili.
- `POST /api/network`: SSID NUL-terminated di 33 byte + password di 65 byte.
- `GET/POST /api/config`: timing, gesture e suoni (protocollo binario esistente).

### Canale push (UDP) — per il mini PC

Le rotte HTTP qui sopra restano per la pagina nel browser e per il ricevitore USB.
Il mini PC usa invece un canale dove è il wearable a parlare per primo: niente
polling, ogni tasto parte appena premuto.

- Porta UDP **37021** sul controller. Il wearable non fa `bind`: il controller
  risponde all'indirizzo e alla porta da cui arriva il datagramma.
- **Heartbeat** ogni secondo. Finché nessun controller risponde va in broadcast
  su tutte le reti in cui il wearable si trova (il suo AP e la LAN); poi va dritto
  al controller che ha risposto. Tre secondi senza risposte = controller perso,
  si torna al broadcast. Il controller non deve conoscere l'indirizzo del wearable.
- **Comando**: parte subito e viene ripetuto ogni 100 ms finché arriva il suo ACK.
  Scade, come prima, tre secondi dopo essere arrivato in testa alla coda (buzzer
  d'errore, una nuova pressione crea un nuovo comando).
- `GET /api/status` contiene `"push":1`: un controller che parla push non deve
  chiamare `POST /api/session`, altrimenti il canale push aspetta 10 secondi.
- Un solo controller alla volta, HTTP o push: mentre un tabellone HTTP tiene la
  sessione le risposte push sono ignorate, e viceversa `POST /api/session` dà 409.

Datagrammi, interi little-endian:

| Verso | Byte | Contenuto |
|---|---|---|
| wearable → controller | 0-1 | `"PM"` |
| | 2 | versione del canale = 1 |
| | 3 | `'H'` heartbeat, `'A'` comando |
| | 4-9 | id del dispositivo (gli stessi 6 byte di `device_id` in `/api/network`) |
| | 10-13 | `session_id` corrente |
| | 14-17 | `sequence` dell'ultimo comando confermato in questa sessione (0 = nessuno) |
| | 18-19 | batteria in mV (0 = non misurata) |
| | 20-31 | solo `'A'`: pacchetto ACTION v6 (12 byte, `ble-protocol.md`) |
| controller → wearable | 0-3 | `"PM"`, 1, `'h'` (risposta all'heartbeat) |
| | 0-3 + 20 | `"PM"`, 1, `'a'` + pacchetto ACK v6 (20 byte) |

Il campo 14-17 serve al controller per accorgersi che un comando che lui ha
confermato era già scaduto sul wearable (ACK perso per tre secondi): in quel caso
il punto va tolto, perché chi ha sentito l'errore ripreme.

Il canale non è autenticato: chi sta nella stessa rete può fingersi wearable o
controller, come già per le rotte HTTP.

### Ricerca della LAN

Senza LAN il wearable la cerca subito, poi dopo 2, 4, 8 e 16 secondi, poi ogni
16. Una pressione di un tasto fa ripartire la scala da capo; persa la LAN la
ricerca riparte subito. Da connesso non cerca nulla. Mentre un controller è
agganciato all'AP la ricerca aspetta (la radio è una sola: cercando, l'AP tace).

Un solo tabellone alla volta controlla il dispositivo. La sessione usa un ID
casuale nell'header `X-Playmaker-Client`; dopo 10 secondi senza richieste un altro
tabellone può collegarsi. Il polling avviene ogni 150 ms senza sovrapporre le
richieste. Non si accodano nuovi punti quando nessun tabellone è collegato.

Stato della partita, cronologia Undo e ACK già applicati vengono salvati nel
localStorage **prima** di inviare l'ACK: un retry non aggiunge punti duplicati.
Usare lo stesso browser e indirizzo durante una partita: AP e LAN hanno origini
diverse e quindi archivi distinti. Non esiste sincronizzazione delle partite tra
browser o PC diversi. Le API sono destinate a una LAN fidata, non a Internet.

## Pulsanti e gesture (schema configurazione 7)

- Tasto alto A (GPIO3): ogni click invia Punto A.
- Tasto basso B (GPIO0): ogni click invia Punto B.
- Undo (GPIO2): ogni click invia Undo.
- Centrale Moment (GPIO1): click singolo invia Momento, doppio click invia VAR.
- A, B e Undo non raggruppano doppi/tripli click: cinque click rapidi sono cinque comandi.
- Solo il centrale attende la finestra di 280 ms per distinguere click singolo e doppio.
- Le gesture personalizzabili ammettono click e pressione lunga (anche gruppi e sequenze).
- Il doppio centrale VAR rimane un'associazione protetta; non ci sono tripli click.
- I rilasci dopo una pressione lunga di A/B rimangono eventi interni per fermare
  avanzamento/riavvolgimento; non sono una nuova scelta nell'editor delle gesture.

All'aggiornamento da uno schema precedente vengono applicate le nuove associazioni
predefinite e conservate le gesture extra compatibili e i suoni. Le vecchie
gesture con doppio/triplo click non compatibili vengono escluse. Le credenziali
Wi-Fi non sono coinvolte nella migrazione. Per A/B/Undo immediati, evitare gesture
composte che condividano un prefisso con il loro click singolo.

Le richieste periodiche della pagina tengono sveglio il wearable; senza utilizzo
resta il timeout di deep sleep di cinque minuti. Un pulsante lo risveglia.

## Verifica

```sh
idf.py build
idf.py -p COM26 flash
node test/host/test_wifi_web.cjs
```

Il test browser `test/host/test_wifi_ui.cjs` richiede Playwright installato in
`build/qa` e Chrome nel percorso indicato dal test. Verifica desktop, mobile,
scansione e invio credenziali con API simulate; non sostituisce il test radio.

Grafica ispirata a https://playmakeritalia.it/: fondo `#07080b`, superfici scure,
accento `#c8f231`, grandi numeri leggibili e pagine responsive. Tutte le risorse
sono integrate nel firmware; l'interfaccia funziona senza accesso a Internet.
