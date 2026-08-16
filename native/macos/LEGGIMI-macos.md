# Mind Zoom su macOS — stato del port

Aggiornato il 16 agosto 2026.

**Nessuna riga di questo port è mai stata compilata.** È stata scritta su
Windows, dove non esistono né un toolchain Objective-C né i framework Apple.
La logica è quella verificata sul campo sul backend Windows; la sintassi
Objective-C e le chiamate ai framework non sono state provate. Trattalo come un
punto di partenza serio, non come software funzionante.

## Cosa è pronto

| pezzo | stato |
|---|---|
| `mz_core` — DSP, controllo, calibrazione, decodifica | **portabile**, gira già su entrambe |
| `mz_render` — rendering | interfaccia neutra + `renderer_macos.mm` (Core Graphics) |
| `mz_platform` — schermi, preferenze | interfaccia neutra + `platform_macos.mm` (Cocoa) |
| `mz_ble` — Muse | interfaccia già isolata + `muse_macos.mm` (CoreBluetooth) |
| `mz_tests` — 175 asserzioni | **portabile**, dipende solo da `mz_core` |
| **l'eseguibile** | **manca** — vedi sotto |

Il lavoro strutturale è quello che è stato fatto: prima i tipi Direct2D
(`D2D1_RECT_F`, `HWND`) stavano nelle firme pubbliche del renderer, e quindi in
sessanta punti di disegno sparsi per l'applicazione. Ora l'interfaccia grafica
non nomina nessuna API di sistema, e il backend si sceglie nel CMakeLists.

## Cosa manca

**`src/app/main.cpp`.** Contiene due cose mescolate:

- la **logica dell'esperienza**: macchina a stati, thread DSP, disegno di HUD,
  telemetria, scheda di calibrazione, gestione dei tasti. Tutto già scritto in
  tipi portabili dopo il refactor.
- lo **shell Win32**: `WNDCLASSEXW`, `CreateWindowExW`, message loop,
  `GetOpenFileNameW`, `AttachConsole`, `WM_DISPLAYCHANGE`.

Vanno separati in:

```
src/app/experience.{hpp,cpp}   logica, portabile, nessuna API di sistema
src/app/shell_win32.cpp        finestre + loop + dialoghi Win32   (esiste, da estrarre)
src/app/shell_macos.mm         NSWindow + NSApplication + NSOpenPanel
```

Lo shell deve fornire alla logica quattro cose e basta: creare due finestre,
consegnare gli eventi di tastiera, chiedere un file all'utente, e chiamare un
frame a ogni vsync. È un'interfaccia piccola; il lavoro è nell'estrazione, non
nel design.

Stima onesta: una giornata per l'estrazione, una per lo shell Cocoa, e poi il
tempo che serve a far combaciare quello che qui non si può provare.

## Come compilare quando ci sarai sopra

```sh
cd native
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-macos
ctest --test-dir build-macos --output-on-failure
```

Oggi questo compila **core, rendering, schermi, BLE e i test**, e non produce
l'eseguibile — il CMakeLists lo dice esplicitamente invece di fallire con un
errore di link incomprensibile.

I 175 test sono la cosa da far girare per prima: dipendono solo da `mz_core` e
verificano DSP, legge di controllo, calibrazione statistica, formato dei
pacchetti Athena e notch di rete. Se passano su macOS, la parte che conta del
programma è portata.

## Trappole già note

**Bluetooth.** Da macOS 11 serve `NSBluetoothAlwaysUsageDescription`
nell'`Info.plist` (c'è, accanto a questo file). Senza, il processo non viene
negato: viene **terminato** al primo uso di `CBCentralManager`. E l'eseguibile
deve stare dentro un bundle `.app`, altrimenti l'Info.plist non viene letto.

**CoreBluetooth non ha nulla di sincrono.** Il backend Windows scandiva la
sequenza di avvio con `sleep` e attese sulla risposta di controllo. Su macOS
bloccare la coda del delegate impedisce di ricevere proprio le risposte che si
stanno aspettando: la sequenza è espressa come catena di `dispatch_after`. I
ritardi sono gli stessi, la forma no.

**Coordinate ribaltate, due volte.** Cocoa e Core Graphics hanno l'origine in
basso a sinistra; tutto il resto del programma ragiona con y verso il basso come
Win32. Il ribaltamento si fa una volta sola nel contesto (`renderer_macos.mm`),
e va disfatto localmente in due punti: nel disegno delle immagini e in quello
del testo. Sono i due posti dove aspettarsi immagini capovolte se qualcosa non
torna.

**`wchar_t` è a 32 bit.** Su Windows è UTF-16, su macOS UTF-32. Le conversioni
verso `CFString`/`NSString` passano per `kCFStringEncodingUTF32LE` esplicito.

**Nome degli schermi.** Su Windows è `\\.\DISPLAY1`, stabile. Su macOS ho usato
`"Display 1"` per indice invece di `localizedName`, che cambia con la lingua di
sistema e col monitor collegato: è la chiave con cui si ritrova la scelta
memorizzata, quindi deve essere stabile, non bella.
