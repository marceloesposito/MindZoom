# Archeologia del repository

Note prese il 16 agosto 2026, prima di ridurre otto branch a due. Servono a
non dover ricostruire per deduzione cose che erano già state decise.

Tutti i commit citati restano nella storia: niente è stato riscritto e niente è
irraggiungibile. Quando qui si dice «abbandonato» significa «non è più nel
codice attuale», non «cancellato».

---

## Com'era organizzato prima

| branch | ultimo commit | cosa esplorava |
|---|---|---|
| `master` | 24/05/2026 | primo prototipo web, mouse + interfaccia sensore |
| `dev` | 24/05/2026 | asset in `public/`, riconnessione WebSocket limitata |
| `test-option-b` | 24/05/2026 | variante di architettura, poi scartata |
| `feature/muse-ble-testing` | 20/07/2026 | Web Bluetooth diretto verso il Muse |
| `feature/adaptive-calibration-testing` | 29/07/2026 | calibrazione adattiva, percentili |
| `feature/native-cpp-port` | 29/07/2026 | inizio del port C++ |
| `feature/control-smoothing` | 07/08/2026 | filtraggio dell'uscita, registrazione sessioni |
| `feature/dual-screen` | 16/08/2026 | doppio schermo, e tutto il resto |

**Ogni branch tranne `dev` aveva zero commit non contenuti in
`feature/dual-screen`**: erano tappe della stessa linea, non alternative vive.
Il commit unico di `dev` è conservato nel tag `archivio/dev-public-images`.

---

## L'architettura a bridge WebSocket — abbandonata

Fino a maggio 2026 la versione web **non parlava con la fascia**. Riceveva un
valore di concentrazione da un server:

```js
const WS_SERVER_URL = 'wss://il-tuo-bridge.onrender.com';
```

C'era un bridge ospitato su Render che stava fra il dispositivo e il browser.
Oggi non esiste più: `index.html` carica `MuseBluetooth.js` e parla al Muse
direttamente via Web Bluetooth.

**Perché vale la pena saperlo.** Se un giorno servisse separare il computer che
indossa la fascia da quello che proietta — per esempio in una sala dove il
partecipante sta lontano dallo schermo — quella strada era già stata percorsa.
Il codice sta in `archivio/dev-public-images` e nella storia di `master`.

Con quel bridge era stata presa anche una decisione che oggi non si applica ma
che è giusta e andrebbe rifatta uguale: **la riconnessione era limitata a tre
tentativi**, poi si arrendeva e passava alla simulazione, invece di ritentare
all'infinito ogni tre secondi. Un ciclo di riconnessione senza tetto, in
un'installazione lasciata accesa, è un modo silenzioso di consumare batteria e
riempire i log.

---

## Dettagli del web che non sono ovvi guardando il codice

**`IMAGE_PATH = '/images/'` è un percorso ASSOLUTO.** Aprire `index.html` con
doppio clic dal disco non funziona: le immagini non vengono trovate. Va servito
da una radice web (`python -m http.server` dalla cartella del progetto basta).
L'esperimento `public/images/` di maggio serviva proprio agli hosting statici
che pubblicano solo una sottocartella.

**Le immagini sono passate da 10 a 12.** `SCALE_LABELS` ha dodici voci e la
dodicesima è 41000×. Chi tocca il numero di immagini deve toccare entrambe.

**`debug-muse-fft.html`** è una pagina di diagnostica dello spettro, separata
dall'esperienza. Non è collegata da `index.html`.

---

## File spariti dal codice, e perché

**`native/tools/muse_probe.cpp`** — sonda da riga di comando per il Bluetooth.
Rimossa di proposito ad agosto: chiedeva di imparare un secondo programma per
ottenere quello che l'applicazione fa già da sola. Le sue funzioni sono dentro
`MindZoom.exe`, fra il pannello `H`, la registrazione automatica e
`--controlla`.

**`native/src/render/renderer.cpp`** — non è sparito, è diventato
`renderer_win32.cpp` quando il rendering è stato diviso per piattaforma. Il
gemello è `renderer_macos.mm`.

---

## Documenti che restano e cosa contengono

- **`REFACTOR-NOTES.md`** — le decisioni della pipeline adattiva, di luglio. Il
  §4 aveva previsto per iscritto il difetto del decodificatore EEG, con il
  criterio diagnostico giusto, settimane prima che venisse trovato sul campo.
  Vale la pena rileggerlo prima di sospettare qualcosa: potrebbe già esserci.
- **`HANDOFF.md`** — contesto di sessione, scritto per riprendere il lavoro da
  un'altra macchina.
- **`README.md`** — descrive un'interfaccia di simulazione (`isSimulationMode`)
  del prototipo di maggio. **È vecchio** rispetto al codice attuale.
- **`native/macos/LEGGIMI-macos.md`** — stato del port macOS e cosa manca.
- **`native/dist-template/LEGGIMI.txt`** — il manuale che viaggia col pacchetto.

---

## Come è organizzato adesso

Due branch, perché il codice ha due nature e non tre:

- **`nativo`** — l'applicazione C++. Un solo codice, due backend: Windows
  (Direct2D, WinRT) e macOS (Core Graphics, CoreBluetooth). Dividerli in due
  branch avrebbe significato riapplicare ogni correzione due volte.
- **`web`** — la versione browser.

`master` è allineato al lavoro attuale ed è ciò che si vede aprendo il
repository.
