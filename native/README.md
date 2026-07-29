# Mind Zoom — port nativo C++

Stato del port. Il brief architetturale completo (spec BLE/DSP/controllo estratta 1:1 dal JS)
resta in [`PHASE2-KICKOFF.md`](PHASE2-KICKOFF.md); qui c'è solo cosa è fatto e come si compila.

## Compilare

```bat
native\build.bat
```

Configura con CMake, compila e lancia i test. Non serve il *Developer Command Prompt*:
lo script chiama `vcvars64.bat` da sé.

## Toolchain (verificata su questa macchina)

Il brief ipotizzava Visual Studio 2022; l'installazione reale è più recente e va bene lo stesso:

| Componente | Trovato |
|---|---|
| Compilatore | MSVC 14.50.35717 (`cl` 19.50) — VS Build Tools 18 |
| Windows SDK | 10.0.26100.0 |
| C++/WinRT | presente nell'SDK (`cppwinrt/winrt/base.h`) — serve al BLE |
| CMake | 4.2.3 (incluso nei Build Tools) |
| Generatore | Ninja |

Nessuna dipendenza esterna finora: **vcpkg non serve ancora**. Servirà per il rendering
(SDL2, Dear ImGui, libwebp) e non prima.

## Cosa è fatto

Il core **deterministico** della pipeline: tutto ciò che trasforma campioni in una velocità di
zoom, cioè la parte che si può verificare numericamente senza hardware né finestra.

```
src/config.hpp              costanti 1:1 da config.js (single source of truth)
src/dsp/decode.hpp          unpack 14 bit, centratura ADC, conversione µV
src/dsp/filters.hpp         predittore DC + biquad (notch, passa-basso)
src/dsp/stft.{hpp,cpp}      ring per canale, Hann, FFT radix-2, indice di Pope
src/dsp/gating.{hpp,cpp}    artefatti (relativo) + proxy di contatto
src/control/calibration.*   calibrazione attiva + legge di controllo a estremi
src/control/zoom.*          rate control, detent/isteresi/dwell, crossfade
tests/test_main.cpp         62 asserzioni, rispecchiano i .test.js
```

I test sono la garanzia del port: replicano le attese della suite JS sugli stessi valori, quindi
il C++ si valida contro la pipeline già tarata sul campo e non contro sé stesso.

## Cosa manca

In ordine di dipendenza:

1. **BLE (WinRT GATT)** — connessione, sequenza di avvio, notifiche, riaggancio.
   La spec è §3 del brief; da recepire anche il fix del riaggancio singolo (commit `45d19bb`):
   un solo tentativo alla volta, mai due `connect()` concorrenti.
2. **Rendering** — SDL2 + OpenGL, 12 texture `.webp` in crossfade, HUD ImGui. Serve vcpkg.
3. **Thread split** — BLE su thread 1, DSP su thread 2, render sul main; ring SPSC lock-free
   fra BLE e DSP, double-buffer atomico fra DSP e render.
4. **Packaging** — `.exe` + `assets/`, runtime static-linked.

Il core attuale è già thread-agnostico: nessuno stato globale, nessuna allocazione nel percorso
caldo, quindi lo split è un lavoro di cablaggio e non una riscrittura.
