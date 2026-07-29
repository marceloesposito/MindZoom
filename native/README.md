# Mind Zoom — port nativo C++

Applicazione Windows nativa: `MindZoom.exe` + cartella `assets/`, nessun installer, nessun
runtime da installare. Il brief architetturale con la spec estratta dal JS resta in
[`PHASE2-KICKOFF.md`](PHASE2-KICKOFF.md).

## Uso

```bat
native\build.bat      REM configura, compila, lancia i test
native\package.bat    REM produce dist\MindZoom\ pronto da copiare altrove
```

Eseguibili prodotti:

| Binario | A cosa serve |
|---|---|
| `MindZoom.exe` | l'esperienza completa |
| `MindZoom.exe --selftest` | verifica headless della catena grafica, esito nel codice d'uscita |
| `mz_probe.exe` | sonda da console: connette il Muse e stampa la diagnostica del DSP |
| `mz_tests.exe` | 62 test numerici del core |

Comandi nell'app: **INVIO** avvia la calibrazione (e riprova se fallisce), **H** mostra o
nasconde il pannello diagnostico, **ESC** esce.

## Architettura

Tre thread, comunicazione senza lock:

```
[1: BLE]     GATT WinRT -> decodifica 14 bit -> ring SPSC lock-free
                                                     |
[2: DSP]     STFT scorrevole -> Pope -> gating -> calibrazione / velocita'
                              -> double buffer atomico
                                                     |
[3: Render]  vsync -> rate control -> crossfade Direct2D + HUD
```

Il thread di render non attende mai il segnale: legge l'ultimo stato completo pubblicato. Il
callback GATT non alloca e non blocca, altrimenti si perdono pacchetti.

## Dipendenze: nessuna

Tutto viene dal Windows SDK — è stata una scelta, non un caso: il carico grafico è due quad
texturati in crossfade, per cui Direct2D basta e avanza, e questo elimina SDL2, Dear ImGui,
libwebp e vcpkg che il brief ipotizzava.

| Serve | Fornito da |
|---|---|
| BLE | C++/WinRT (`windowsapp.lib`) |
| Finestra e rendering | Win32 + Direct2D (`d2d1`) |
| Testo | DirectWrite (`dwrite`) |
| Immagini, WebP incluso | WIC (`windowscodecs`) |
| FFT | implementazione propria, port 1:1 di quella JS |

Toolchain verificata: MSVC 14.50 (VS Build Tools 18), Windows SDK 10.0.26100, CMake 4.2.3, Ninja.
Il CRT è linkato staticamente: sulla macchina di destinazione **non** serve il redistributable
di Visual C++. Il pacchetto pesa ~5.5 MB, quasi tutto immagini.

## Struttura

```
src/config.hpp              costanti 1:1 da config.js (single source of truth)
src/dsp/decode.hpp          unpack 14 bit, centratura ADC, µV
src/dsp/filters.hpp         predittore DC + biquad notch/passa-basso
src/dsp/stft.{hpp,cpp}      ring per canale, Hann, FFT radix-2, indice di Pope
src/dsp/gating.{hpp,cpp}    artefatti (soglia relativa) + proxy di contatto
src/control/calibration.*   calibrazione attiva + legge di controllo a estremi
src/control/zoom.*          rate control, detent/isteresi/dwell, crossfade
src/ble/muse.{hpp,cpp}      GATT WinRT, sequenza di avvio, riaggancio
src/render/renderer.*       Direct2D + DirectWrite + WIC
src/app/main.cpp            finestra, thread, FSM, disegno della scena
src/util/spsc_ring.hpp      ring lock-free BLE -> DSP
src/util/double_buffer.hpp  pubblicazione atomica DSP -> render
tests/test_main.cpp         62 asserzioni, rispecchiano i .test.js
tools/muse_probe.cpp        sonda da console
```

## Verifica

- **62/62** test numerici verdi, compilazione pulita con `/W4 /permissive-`.
- I test replicano le attese di `control-pipeline.test.js` e `stft.test.js` sugli stessi valori:
  il port si valida contro la pipeline JS già tarata sul campo, non contro sé stesso.
- `--selftest` esercita Direct2D, WIC (12 immagini) e DirectWrite disegnando un frame completo
  su finestra mai mostrata. `package.bat` lo esegue e non produce il pacchetto se fallisce.

**Quello che i test non coprono:** il percorso BLE. Serve la Muse accesa. `mz_probe.exe` esiste
per questo — stampa indice, bande e diagnostica dei pacchetti nello stesso formato dei log JS,
così i due porti si confrontano sullo stesso segnale. Da fare alla prima sessione con la fascia
carica, insieme alla taratura di `LOCAL_DECAY`.

## Distribuzione su un altro PC

Il pacchetto è una cartella da copiare: nessun installer, nessun runtime da installare.

**Requisiti sulla macchina di destinazione:**

| Serve | Perché | Rischio |
|---|---|---|
| Windows 10 (1809+) o 11, **64 bit** | le API BLE di WinRT | build solo x64 |
| Bluetooth LE | connessione alla Muse | un PC fisso senza BLE non funziona |
| — | il CRT è statico, Direct2D/WIC sono di sistema | nessuno |

**Il codec WebP non serve.** `package.bat` converte gli asset in JPEG (`mz_convert`), formato
che ogni Windows decodifica da sempre. Il caricatore accetta `.jpg`, `.webp` e `.png` in
quest'ordine: in sviluppo si lavora direttamente sui `.webp` del progetto web, la distribuzione
usa i JPEG. Costa meno spazio dell'originale: pacchetto **3.6 MB** in tutto.

Alla prima esecuzione di un `.exe` non firmato scaricato da internet, Windows mostra l'avviso
SmartScreen ("Windows ha protetto il PC"): serve *Ulteriori informazioni → Esegui comunque*.
Si evita solo firmando l'eseguibile con un certificato.
