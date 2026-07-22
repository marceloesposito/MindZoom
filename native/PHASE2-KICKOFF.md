# Fase 2 — Port nativo C++ (.exe Windows): brief di avvio

Branch: `feature/native-cpp-port` (da `feature/adaptive-calibration-testing`, che contiene
la pipeline JS di riferimento — Fase 1, commit `47bc39d`).

Questo documento è il punto di partenza della prossima sessione. Contiene: prerequisiti da
installare, architettura, **spec di riferimento estratta 1:1** dal codice JS (BLE, DSP,
legge di controllo, costanti), struttura di progetto proposta e checklist della prima sessione.

> **Principio guida.** La pipeline JS validata sul campo è la *spec*. Il C++ la replica 1:1
> (stessi bin, stessi coefficienti, stesse costanti) così l'`.exe` riproduce il feeling già
> tarato. Nessuna re-invenzione dell'algoritmo in fase di port.

---

## 0. Verdetto sul workload (perché così)

Il DSP EEG è minuscolo: 256 Hz × 4 canali, FFT da 256 punti ogni 48 campioni → ~5.3 FFT/s =
microsecondi. **La GPU è overkill per il DSP** (l'overhead di lancio kernel supera il calcolo):
"segnali come raster → GPU" aggiungerebbe latenza. I colli di bottiglia reali sono BLE
(latenza/throughput) e l'overhead browser/JS. Il nativo vince rimuovendo quelli. Il
multithreading serve a **separare i concern** (BLE / DSP / render), non a parallelizzare FFT.
La GPU si usa **solo per il rendering** (2 quad texturati in crossfade), come già fa PixiJS.

---

## 1. Prerequisiti da installare (farlo PRIMA della sessione)

- **Visual Studio 2022** con workload *"Sviluppo di applicazioni desktop con C++"* (MSVC v143)
  **+ Windows 11 SDK** (necessario per C++/WinRT e l'accesso BLE nativo).
- **CMake ≥ 3.25** (incluso in VS, o standalone).
- **vcpkg** per le dipendenze (`git clone https://github.com/microsoft/vcpkg` + `bootstrap-vcpkg.bat`).
- Dipendenze (via vcpkg, manifest `vcpkg.json`):
  - `pocketfft` (o `kissfft`) — FFT reale, header-only, sostituisce `_computeRadix2FFT`.
  - `sdl2` — finestra + input + contesto GL (alternativa: `bgfx`).
  - `imgui[sdl2-binding,opengl3-binding]` — HUD di diagnostica.
  - `libwebp` — le immagini sono `.webp` (`/images/1..12.webp`); serve per decodificarle come
    texture. In alternativa pre-convertire a PNG e usare `stb_image`.
  - `cppwinrt` (C++/WinRT) — proiezioni WinRT per il BLE; spesso già nell'SDK.

> Verifica rapida ambiente: `cl` (MSVC) e `cmake --version` dal *Developer Command Prompt*.

---

## 2. Architettura (3 thread)

```
[Thread 1: BLE I/O]  WinRT GATT  → decode 14bit → ring lock-free SPSC (campioni µV)
                                                        │
[Thread 2: DSP]  consuma ring → sliding STFT (Hann+FFT) → Pope index → gating
                 → calibrazione / controllo a estremi → pubblica `control` (double-buffer atomico)
                                                        │
[Thread 3: Render/main]  vsync loop → legge `control` (lock-free)
                 → rate-control (integratore+easing) → crossfade 2 quad (GPU) + HUD ImGui
```

- Comunicazione BLE→DSP: **SPSC lock-free ring** di campioni (o di pacchetti già decodificati).
- Comunicazione DSP→Render: **double-buffer atomico** di uno struct `ControlState`
  (`velocity`, `smoothedIndex`, `phase/calibStage`, `quality`, estremi) — il render legge sempre
  l'ultimo completo senza lock.
- Nessuna allocazione nei loop caldi (come in JS: buffer preallocati).

---

## 3. Spec BLE — Muse (estratta da `MuseBluetooth.js`)

- **Service UUID:** `0000fe8d-0000-1000-8000-00805f9b34fb`
- **Control char:** `273e0001-4c4d-454d-96be-f03bac821358` (write-without-response + notify)
- **EEG char:** `273e0013-4c4d-454d-96be-f03bac821358` (notify)
- **Comando (control):** payload = `[len][ascii...]` dove `ascii = cmd + "\n"` e `len = ascii.length`.
- **Sequenza di avvio streaming** (`_establishSession`, con piccoli sleep):
  `v6` → `s` → `h` → `p21` → `dc001` `L1` → `h` → `p1041` → `dc001` `L1` → `s`.
  Comando di **resume** (watchdog): `d`.
- **Formato pacchetto EEG** (`handleIncomingEEGPacket`):
  - `len < 10` → scarta. `dataType = getUint8(9) & 0x0F`; valido solo `1` (4 canali) o `2` (8).
  - `headerOffset = 14`; payload = byte dopo l'header.
  - `numChannels = (dataType==2) ? 8 : 4`. **`numSamples = floor(payload.length*8 / (14*numChannels))`**
    (NON hardcodare a 2 — bug già corretto: dimezzava/sestuplicava il rate).
  - Campioni packed a **14 bit**, little-endian bit order (vedi `_get14BitRaw`):
    `((b0 | b1<<8 | b2<<16) >> bitShift) & 0x3FFF`, `byteOffset=bitOffset>>3`, `bitShift=bitOffset&7`.
  - **Decodifica (`_centerSample`, mode `unsigned-centered`):** `centered = rawUnsigned - 8192`.
    (Mode `signed` = complemento a due: sbagliato per il Muse, produce onda quadra ±725 µV.)
  - **µV:** `uv = centered * (1450.0 / 16383.0)`.
- Canali: `['TP9','AF7','AF8','TP10']` → indici `0..3`. **Frontali per il controllo = [1,2] (AF7/AF8)**.
- **Auto-reconnect:** su `gattserverdisconnected`, backoff `min(500*2^(n-1), 4000) ms`, max 5.
- **Watchdog flusso:** se nessun dato per `EEG_WATCHDOG_S` (3 s) → azzera velocità + invia `d`
  (max 3 tentativi). Il BLE nativo dovrebbe ridurre questi stalli.

---

## 4. Spec DSP (estratta da `MuseBluetooth.js`)

Per canale, per campione (dopo decodifica in µV):
1. **DC removal (predittore):** init `dcPredictor[ch]=uv` al primo campione (no transitorio);
   `filtered = uv - dcPredictor[ch]`; `dcPredictor[ch] += 0.02 * filtered`.
   `dcFree = filtered` (segnale **prima** dei filtri in banda → usato dal gating d'ampiezza).
2. **Notch (biquad):** `b0=0.9391 b1=-0.4024 b2=0.9391 a1=-0.4024 a2=0.8782`.
3. **Low-pass (biquad):** `b0=0.2066 b1=0.4132 b2=0.2066 a1=-0.3695 a2=0.1958`.
   (Forma diretta I; segno degli `a` come nel codice: `y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2`.)
4. Push in ring per canale (`realBuffers` filtrato, `dcFreeBuffers`, `adcBuffers`).

**STFT scorrevole:** finestra `BUFFER_SIZE=256`; ogni `STFT_HOP=48` campioni (a buffer pieno)
si applica la **finestra di Hann** e si esegue una FFT reale (radix-2 in JS → pocketfft in C++);
`mags[i] = sqrt(re^2+im^2)/N` per `i in [0, N/2)`. Bin ↔ Hz: `1 bin = FS/N = 1 Hz`.

**Indice di Pope** (`computePopeIndex` in `app.js`), su AF7+AF8 sommati:
- theta = Σ bins **4..8**, alpha = Σ bins **8..12**, beta = Σ bins **13..30**.
- `index = beta / (alpha + theta)` (null se denom 0).

**Gating** (`_assessQuality`):
- `artifact = (peakCount>=8) && (maxAbsRaw > ARTIFACT_UV_FLOOR) && (maxAbsRaw > ARTIFACT_REL_MULT * medianPeak)`
  dove `maxAbsRaw` = max |dcFree| su AF7/AF8 nella finestra; `medianPeak` = mediana dei picchi
  degli ultimi `ARTIFACT_PEAK_HISTORY=48` frame.
- `contactOk = false` se lo std del segnale filtrato di un frontale `< CONTACT_STD_MIN` (canale
  piatto = elettrodo staccato). Nessun limite superiore. (HSI reale = characteristic dedicata, TODO.)

---

## 5. Spec legge di controllo (Fase 1, da `app.js` + `config.js`)

**Denoise indice:** EMA `c += (index - c) * CALIB_INDEX_EMA` (init col primo campione).

**Calibrazione attiva** (sotto-FSM in ONBOARDING): due fasi da 15 s.
- CONCENTRATE: registra `absMax = max(c)` (dopo `CALIB_LEADIN_S`).
- RELAX: registra `absMin = min(c)`.
- Validazione: fallisce se `!finite` o `span < CALIB_MIN_SPAN_REL * |M|` (→ Riprova).
- `M = (absMax+absMin)/2`; init banda locale `localMax=localMin=M`.

**Controllo a estremi** (`computeExtremaVelocity(c, dt)`, `dt = HOP/FS = 48/256`):
```
span = absMax - absMin
step = LOCAL_DECAY * dt * span
localMax = min(absMax, max(c, localMax - step))
localMin = max(absMin, min(c, localMin + step))
if      c >= localMax:  v = +EXTREMA_GAIN * clamp01((c - M) / (CALIB_CONC_FRACTION*(absMax - M)))
else if c <= localMin:  v = -EXTREMA_GAIN * clamp01((M - c) / (CALIB_CONC_FRACTION*(M - absMin)))
else:                   v = 0
```

**Rate-control (INVARIATO, per frame di render):** `targetFocus += v * ZOOM_SPEED_FACTOR`;
`clamp targetFocus [0,1]`; `currentFocus += (targetFocus - currentFocus) * FOCUS_EASING`;
`rawZoomIndex = currentFocus * (TOTAL_IMAGES-1)` → 2 sprite adiacenti in crossfade (alpha) con
scala `active: 1+progress*0.5`, `next: 0.66+progress*0.34`. Hold/select (`applyHoldSelect`):
dead-zone/detent/isteresi/dwell — vedi `app.js` (portare tale e quale in INTERACTIVE).

### Costanti tarate (da `config.js`) — portare 1:1
| Costante | Valore | Nota |
|---|---|---|
| `STFT_WINDOW` / `STFT_HOP` | 256 / 48 | → 5.33 Hz di controllo |
| `EEG_DECODE_MODE` | `unsigned-centered` | centro ADC = 8192 |
| `CALIB_INDEX_EMA` | 0.30 | denoise |
| `CALIB_CONCENTRATE_S` / `CALIB_RELAX_S` | 15 / 15 | fasi |
| `CALIB_LEADIN_S` | 1.5 | scarto transitorio |
| `CALIB_CONC_FRACTION` | 0.75 | saturazione |
| `CALIB_MIN_SPAN_REL` | 0.08 | soglia fallimento |
| `EXTREMA_GAIN` | 0.25 | velocità max |
| **`LOCAL_DECAY`** | **0.35** | **manopola di feel — da ri-tarare sul campo** |
| `ZOOM_SPEED_FACTOR` / `FOCUS_EASING` | 0.003 / 0.06 | rate-control |
| `ENTER_HOLD_FRAC` / `BREAK_HOLD_FRAC` | 0.25 / 0.55 | detent |
| `SNAP_STRENGTH` / `SNAP_VEL_FRAC` | 0.06 / 0.25 | snap |
| `LOCK_DWELL_S` / `LOCK_DWELL_MULT` | 3.0 / 1.5 | dwell-to-lock |
| `ARTIFACT_REL_MULT` / `ARTIFACT_UV_FLOOR` / `ARTIFACT_PEAK_HISTORY` | 2.5 / 150 / 48 | gating |
| `ARTIFACT_HOLD_MAX_S` | 1.0 | decadimento gating |
| `CONTACT_STD_MIN` | 0.5 | canale piatto |
| `EEG_WATCHDOG_S` | 3.0 | stallo flusso |
| `TOTAL_IMAGES` | 12 | `/images/1..12.webp` |

`SCALE_LABELS` (HUD magnification): `[32,32,100,220,700,1500,3000,6000,10000,17000,25000,41000]`.

---

## 6. Struttura di progetto proposta (`native/`)

```
native/
  CMakeLists.txt
  vcpkg.json
  assets/            # copia di /images/1..12.webp (o PNG pre-convertiti)
  src/
    main.cpp         # loop render (vsync), FSM di fase, HUD
    config.hpp       # tutte le costanti della tabella §5 (specchio di config.js)
    ble/muse.{hpp,cpp}      # WinRT GATT, decode 14bit, sequenza avvio, reconnect (§3)
    dsp/stft.{hpp,cpp}      # ring, Hann, FFT (pocketfft), band power, Pope index (§4)
    dsp/gating.{hpp,cpp}    # artifact/contact (§4)
    control/calibration.{hpp,cpp}  # scheda attiva + estremi (§5)
    control/zoom.{hpp,cpp}         # rate-control + hold/select (§5)
    render/crossfade.{hpp,cpp}     # 2 quad texturati, scala/alpha (§5)
    util/spsc_ring.hpp             # ring lock-free BLE→DSP
    util/double_buffer.hpp         # DSP→render
  tests/             # port dei test JS come check numerici (opzionale ma consigliato)
```

---

## 7. Checklist prima sessione (ordine consigliato)

1. **Scaffold build:** `CMakeLists.txt` + `vcpkg.json`, "hello window" SDL2+GL che apre e chiude.
2. **`config.hpp`:** trascrivere la tabella §5 (single source of truth).
3. **BLE spike:** WinRT GATT → connessione al Muse, sottoscrizione EEG char, log dei pacchetti
   grezzi. Verificare `dataType`/`numSamples`/`sps~256` come nei log JS.
4. **Decode + DSP:** 14bit→µV→DC→notch/LP→ring→STFT→Pope index. Confrontare i valori con i log
   JS a parità di segnale (o rigiocando pacchetti registrati).
5. **Controllo:** calibrazione a estremi + `computeExtremaVelocity` + rate-control.
6. **Render:** crossfade 12 texture .webp, HUD ImGui con gli stessi campi del pannello JS.
7. **Thread split:** spostare BLE su thread 1, DSP su thread 2 con i ring lock-free.
8. **Packaging:** `.exe` + `assets/`; runtime static-link per portabilità.

---

## 8. Questioni aperte da decidere al kickoff

- **Render backend:** SDL2+OpenGL (semplice) vs bgfx (più portabile/moderno). Default: SDL2+GL.
- **Asset .webp:** decodifica a runtime con `libwebp` vs pre-conversione a PNG (`stb_image`).
- **Test:** portare i test numerici (come i `.test.js`) o validare per confronto coi log JS?
- **HSI reale del Muse:** implementare la characteristic dedicata o restare sul proxy std?
- **Registrazione/replay pacchetti:** utile un dump BLE da rigiocare offline per sviluppare il
  DSP senza indossare la cuffia ogni volta.

---

## 9. Vincoli (invariati dalla Fase 1)

Niente merge su main; niente git distruttivo. Non alzare il sample rate hardware del Muse.
Niente soglie EEG assolute da letteratura come discriminante (gli estremi sono **personali**).
Non toccare i path immagine né l'ordine di stacking. Non cambiare la matematica base del
rate-control: cambia solo la *sorgente* della velocità (controllo a estremi).
