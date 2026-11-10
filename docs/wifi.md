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
