# Mind Zoom — Contesto di sessione (handoff per un'altra istanza di Claude Code)

> File generato per trasferire il contesto di una conversazione precedente su un'altra macchina.
> All'apertura, chiedi a Claude Code di leggere questo file per riprendere da qui.

## Cos'è il progetto

**Mind Zoom**: esperienza web interattiva che simula un microscopio elettronico a scansione (SEM).
Il livello di zoom è controllato dalla concentrazione mentale dell'utente, letta da una fascia EEG
**Muse** via Web Bluetooth (o, in alternativa, simulata con la rotellina del mouse).

Stack: HTML/JS vanilla, nessun build step, PixiJS (rendering immagini) + Tailwind (CDN) per la UI.
Tutta l'elaborazione (decodifica pacchetti BLE, filtri, FFT, calcolo focus) avviene **client-side**,
niente backend.

## File principali

- `index.html` — UI: schermata landing + schermata immersione (canvas PixiJS), pannello debug/telemetria, modali connessione BLE.
- `MuseBluetooth.js` — driver Web Bluetooth per il Muse: connessione GATT, decodifica pacchetti EEG (14 bit), filtro notch + passa-basso, buffering per canale (TP9/AF7/AF8/TP10), FFT radix-2 su finestre di 256 campioni.
- `app.js` — logica applicativa: calcolo focus scientifico (rapporto Beta/(Alpha+Theta) su canali frontali AF7/AF8) con calibrazione automatica a baseline (5s) e adattamento continuo (z-score → sigmoide); traduzione focus → velocità di zoom (rate control con dead zone); animazione crossfade tra le 12 immagini (`images/1.webp`…`12.webp`, da 32x a 41.000x) via PixiJS; modalità simulazione via rotellina mouse (accelerazione/attrito) per testare senza hardware.
- `debug-muse-fft.html` — pagina standalone di debug per visualizzare i dati FFT del Muse (Chart.js via CDN).
- `README.md` — documenta l'interfaccia di input disaccoppiata (`updateFocusFromSensor`/`window.setSensorValue`).
- `robots.txt` + meta `<meta name="robots" content="noindex, nofollow">` in entrambi gli HTML — aggiunti per poter fare deploy pubblici di test senza essere indicizzati dai motori di ricerca.

## Stato Git

- Branch di lavoro: `feature/muse-ble-testing` (il più aggiornato tra tutti i branch: `dev`, `master`, `test-option-b`).
- Ultimo commit pushato su origin: `39792c9` "chore: aggiunge meta noindex e robots.txt per deploy di test non indicizzato".
- Repo: `https://github.com/marceloesposito/MindZoom.git`.
- Nota: esiste anche un branch `dev` con un'architettura diversa ("Option A with public directory assets and capped reconnection") non ancora esplorata a fondo — potrebbe contenere idee utili, da valutare se serve.

## Obiettivo concordato (non ancora implementato)

L'utente vuole un sito **plug and play**, utilizzabile con due modalità:

1. **Modalità Schermo** (esistente, invariata): tutto in un'unica finestra come oggi.
2. **Modalità Proiettore** (da costruire): se collegato un monitor esterno, dev'essere possibile
   aprire una **seconda finestra a schermo intero** su quel monitor che mostri **solo le immagini**
   (esperienza immersiva pura), mentre la finestra madre sul monitor principale mostra i controlli,
   la telemetria, i LED di stato, ecc.

### Decisione architetturale presa

Approccio **browser-nativo** (scartata l'opzione Electron, per restare un "sito" senza packaging):

- **Window Management API** (`getScreenDetails()`, Chrome/Edge) per rilevare il monitor esterno e
  posizionarci sopra una `window.open()` in fullscreen.
- **Fallback manuale** se il permesso viene negato o l'API non è supportata: apri comunque la finestra
  con un pulsante "Fullscreen" e istruzioni per trascinarla a mano sul secondo monitor + F11.
- **`BroadcastChannel`** (nativo, stesso-origine, zero config) per sincronizzare in tempo reale lo
  stato (focus/zoom/immagine attiva) tra le due finestre, senza server.
- Scartata la Presentation API (pensata per cast wireless/Chromecast, meno adatta a un monitor via cavo).

### Piano di refactor (Fase 1 — non ancora iniziato)

- Estrarre la logica di rendering PixiJS (sprite pool, crossfade, calcolo immagine attiva) da `app.js`
  in un modulo condiviso `renderer.js`, riusabile sia da `index.html` sia da una nuova `projector.html`.
- Creare `projector.html`: pagina minimale, solo canvas PixiJS fullscreen, nessun pannello/debug/LED,
  riceve lo stato via `BroadcastChannel`.
- In `app.js`: pulsante "Modalità Proiettore" che rileva schermi, apre/posiziona `projector.html`,
  gestisce fallback.
- Quando la modalità proiettore è attiva, la finestra madre smette di renderizzare le immagini a
  schermo pieno e mostra solo controlli/telemetria.

### Deployment (Fase 3)

- Assunzione: la location ha internet → **non serve** vendorizzare offline le librerie CDN
  (Tailwind/PixiJS/Chart.js), quella fase è stata scartata.
- Confronto Vercel vs Render → **Vercel scelto**: deploy diretto da cartella locale via CLI
  (`npx vercel`) senza dover prima pushare su GitHub, ideale per link di test rapidi e usa-e-getta;
  Render è più adatto a backend persistenti e ha un flusso di deploy git-centrico, meno comodo per
  iterazioni rapide.
- Protezione già in atto: `robots.txt` (Disallow globale) + meta `noindex, nofollow` su entrambi gli
  HTML (già committati e pushati).
- **L'utente ha detto che si occuperà del deploy manualmente** — non è stato ancora fatto da Claude.

## Test locale già fatto in questa sessione

Per testare la connessione Muse reale serve un secure context (`https://` o `localhost`, requisito di
Web Bluetooth). Comando usato per servire il sito in locale:

```bash
npx --yes serve -l 5500 .
```

poi aperto Chrome su `http://localhost:5500/`. Nessun problema riscontrato con la versione del codice
servita (era effettivamente la più aggiornata disponibile nel repo).

## Prossimi passi suggeriti

1. Implementare la Fase 1 (refactor `renderer.js` + `projector.html` + modalità proiettore in `app.js`).
2. Testare la modalità proiettore in locale (anche senza hardware Muse, con la simulazione mouse-wheel).
3. Deploy di test su Vercel (a cura dell'utente) per condividere un link non indicizzato con altri tester.
