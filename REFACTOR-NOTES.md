# Refactor: calibrazione adattiva, controllo BCI, performance

Branch: `feature/adaptive-calibration-testing` (da `feature/muse-ble-testing`).

Tutto il comportamento nuovo è dietro `CONFIG.USE_ADAPTIVE_PIPELINE` (`config.js`).
Con `false` si torna al percorso legacy (snapshot 5 s + z-score/sigmoide) per confronto A/B.

## AGGIORNAMENTO — Calibrazione attiva + controllo a estremi (no moving-average)

Il percorso adattivo è stato ri-progettato dopo i test sul campo. **Il ring buffer
percentile è stato rimosso dall'interazione** perché aveva due difetti bloccanti:

1. **Partenza lenta:** serviva ~13 s per riempire il ring prima che il percentile avesse
   autorità → l'interazione di fatto non partiva.
2. **Auto-cancellazione:** essendo una finestra scorrevole, la concentrazione sostenuta
   riempiva il buffer di valori alti e il percentile regrediva a 0.5 → lo zoom si fermava
   proprio quando l'utente spingeva.

**Nuovo modello.** Una **scheda di calibrazione attiva** sostituisce la semina passiva
mediana/MAD: un quadratino su un binario verticale che l'utente spinge in alto
concentrandosi (fase CONCENTRATE, registra il **picco** `absMax`) e lascia scendere
rilassandosi (fase RELAX, registra il **minimo** `absMin`). Da questi due estremi:

- neutro `M = (absMax + absMin) / 2` = zero (sopra = concentrazione/zoom in, sotto =
  distrazione/zoom out);
- una **banda locale di isteresi** (`localMin`/`localMax`) che decade verso il segnale a
  `LOCAL_DECAY` frazioni di span/s: la velocità va a 0 sotto il massimo locale (ferma lo
  zoom in) e diventa negativa sotto il minimo locale (zoom out) → controllo **fasico**;
- saturazione a `CALIB_CONC_FRACTION` (0.75) del tragitto `M`→estremo, così non serve
  ritoccare il picco assoluto per raggiungere la velocità piena.

Codice: `computeExtremaVelocity(c, dt)` sostituisce `computeAdaptiveVelocity` (percentile);
la scheda è la sotto-FSM `CALIB` (INTRO/CONCENTRATE/RELAX/DONE/FAILED) dentro
`PHASE.ONBOARDING`, gestita da `updateOnboardingModal` / `finalizeActiveCalibration`.
Il quadratino usa un feedback **diretto/proporzionale** (altezza ∝ indice smussato), con
una sola EMA di denoise sull'indice (`CALIB_INDEX_EMA`), non una normalizzazione a finestra.
`Normalizer` resta nel file ma non è più nel percorso interattivo (solo legacy).

L'interazione ora **parte subito** dopo la calibrazione (nessun warm-up). In SIMULATION la
rotella pilota il quadratino per testare la scheda senza hardware. Fallimenti (contatto mai
OK, span < `CALIB_MIN_SPAN_REL`) → schermata "Riprova".

## Mappatura nomi: documento -> codice reale

Il documento di handover usava nomi indicativi. Mappature effettive:

| Documento              | Codice reale                                            |
|------------------------|---------------------------------------------------------|
| indice / engagement    | `computePopeIndex(fftData)` -> `AppState.rawRatio`      |
| `targetVelocity` (EEG) | `AppState.eegVelocity` (`AppState.targetVelocity` resta la rotella) |
| ring buffer percentile | oggetto `Normalizer` in `app.js`                        |
| `currentFocus` (detent)| `AppState.targetFocus` (vedi nota sotto)                |
| baseline / scale       | `Normalizer.seedMedian` / `Normalizer.seedMAD`          |
| gating artefatti       | `MuseBluetooth._assessQuality()` -> `payload.quality`   |
| FSM                    | `AppState.phase` + `updatePhase()` / `enterPhase()`     |
| log sensore            | `logSensorState()` (`CONFIG.DEBUG_SENSOR_LOG`)          |
| crossfade autorità     | `resolveAuthorityVelocity()`                            |
| hold/select            | `applyHoldSelect()`                                     |

**Nota sul detent.** Lo pseudo-codice del documento applica lo snap a `currentFocus`.
Nel codice è applicato a `targetFocus`: `currentFocus` è il valore *renderizzato*
(già smorzato da `FOCUS_EASING`), mentre `targetFocus` è il livello di controllo.
Snappare il valore renderizzato avrebbe combattuto contro l'easing.

## Scelte prese

- **Normalizzazione: percentile** (Opzione A). `NORM_MODE` esiste in config, ma il ramo
  `"ema"` **non è implementato**: la semina mediana/MAD è raccolta e loggata ed è il gancio
  pronto per implementarlo, ma oggi non alimenta il controllo.
- **Dispersion guard su IQR *relativo*** (`IQR_MIN_REL`), non assoluto: l'indice di Pope ha
  scala molto diversa fra persone, una soglia assoluta sarebbe stata person-dependent —
  esattamente ciò che il documento vieta.
- **Finestra di Hann** aggiunta prima della FFT: con finestre sovrapposte lo spectral leakage
  di una finestra rettangolare falsa le band power.
- **Artefatto vs contatto scadente sono trattati diversamente**, come da §7:
  artefatto d'ampiezza -> si *scarta la finestra* mantenendo la velocità precedente (niente
  scatto ad ogni blink); contatto scadente -> velocità forzata a 0 (freeze).
- **La finestra di calibrazione non alimenta nemmeno il ring percentile** fuori da
  `[CALIB_START_S, MODAL_UNLOCK_S - CALIB_END_OFFSET_S]`: le finestre sporche (transitorio
  iniziale, gesto di chiusura) non devono inquinare il riferimento.
- **Matematica di rate control invariata** (§11): `ZOOM_SPEED_FACTOR` e `FOCUS_EASING`
  restano per-frame come prima. Il `deltaMS` del ticker è usato **solo** per i timer della FSM.

## Bug trovati in test sul campo (primo giro con hardware)

### 1. Gating permanente -> zoom incollato (risolto)

Sintomo: dopo la calibrazione lo zoom entrava da solo e non rispondeva più a nulla.

Causa: il gating d'ampiezza leggeva `uv`, cioè il valore ADC **prima** della rimozione
della DC. `_get14BitSigned` interpreta come signed dei valori che il Muse manda centrati
in alto, quindi il segnale a riposo vale ~-725 µV: sopra la soglia di 100 µV **sempre**.
Ogni finestra veniva marcata come artefatto, `processAdaptiveIndex` usciva subito e
`eegVelocity` restava congelata sull'ultimo valore -> zoom bloccato in una direzione.

Fix: il gating guarda `dcFreeBuffers`, cioè il segnale dopo la rimozione della DC e prima
del filtraggio in banda. Aggiunto anche un **timeout di gating** (`ARTIFACT_HOLD_MAX_S`):
oltre 1 s di gating continuo la velocità decade verso 0 invece di restare congelata, così
nessun errore di questo tipo può più incollare lo zoom.

Il test sintetico non l'aveva preso perché generava una sinusoide centrata su zero, cioè
senza offset DC: irrealistico. Ora c'è la regressione `[2b]` con offset esplicito.

### 2. Transitorio del predittore DC all'avvio (risolto)

`dcPredictor` partiva da 0 e impiegava ~0.5 s a raggiungere l'offset reale. In quel
transitorio il segnale sembra enorme. Ora si inizializza col primo campione visto.

### 3. Sensibilità: saturazione del mapping (risolto)

Con `ZOOM_GAIN = 1.0` si attraversavano tutti i 12 livelli in ~5.5 s, e il percentile
satura facilmente (p = 1.0 ogni volta che il campione corrente è il massimo del ring).
Gain portato a **0.25** (~22 s a velocità piena) e dead-zone allargata a 0.35–0.65.

Le soglie di hold sono ora **frazioni** della velocità massima di input invece che valori
assoluti: con soglie assolute, abbassare il gain avrebbe reso i detent inescapabili
(`BREAK_HOLD * LOCK_DWELL_MULT` = 0.35 > velocità massima 0.25). C'è anche un clamp
esplicito che garantisce lo sgancio a velocità piena.

### 4. PUNTO APERTO: interpretazione signed del campione a 14 bit

`_get14BitSigned` fa il flip del segno sopra 8191. Se i valori reali del Muse oscillano
**a cavallo** di quella soglia, il segnale ricostruito diventa un'onda quadra da ±700 µV
invece che EEG — il che falserebbe completamente le band power. Non è verificabile senza
dati reali e **non è stato modificato**: è la decodifica preesistente.

**Come diagnosticarlo:** nei log `[EEG]` il campo `ampiezza=` mostra il picco µV dopo la
rimozione della DC. Su segnale sano ci si aspetta indicativamente **10–80 µV**. Se mostra
stabilmente **centinaia di µV** con la cuffia ferma e ben posizionata, il problema è la
decodifica, non il gating.

## NON implementato in questo passaggio

1. **DSP in Web Worker (§8.1).** Rimandato di proposito, come da ordine suggerito in §12:
   ha senso spostare la catena una volta che la logica di controllo è stabile. Nota: le
   notifiche GATT possono arrivare solo sul main thread (vincolo Web Bluetooth), quindi la
   migrazione comporta un `postMessage` per pacchetto (~128/s).
2. **HSI reale del Muse (§7).** `quality.contactOk` è un **proxy** basato sulla deviazione
   standard per canale (piatto = elettrodo staccato, enorme = contatto instabile). Leggere
   l'Horseshoe Indicator vero richiede di sottoscrivere una characteristic dedicata,
   attualmente non gestita dal driver.
3. **Audit VRAM / texture KTX2 (§8.3).** Fatti solo i due interventi a costo zero: preload
   parallelo e `renderable = false` sugli sprite non visibili (compreso quello con alpha ~0).

## Fase HOOK disattivata

`CONFIG.ENABLE_HOOK_PHASE = false`: chiusa la modale si va **dritti all'interazione**.
HOOK (auto-zoom di aggancio) e HANDOVER (crossfade di autorità) restano implementati e
testati, riattivabili col flag. Con hook disattivo `PHASE_INTERACTIVE_S` sale a 80 s,
mantenendo il totale a ~104 s.

## Log di diagnostica

Con `CONFIG.DEBUG_SENSOR_LOG` (default true) la console stampa a `DEBUG_LOG_HZ` (2 Hz):

```
[EEG] INTERACTIVE idx=1.234 c=1.180 v=+0.180 | θ=0.41 α=0.33 β=0.61 | M=1.050 lo=0.900 hi=1.240 [0.700..1.400] | ampiezza=42µV gate=OK
```

`idx` indice di Pope grezzo, `c` indice smussato (denoise), `v` velocità, `θ/α/β` band power
frontali, `M` neutro, `lo/hi` estremi **locali** (banda di isteresi), `[..]` estremi
**assoluti** dalla calibrazione, `ampiezza` picco µV, `gate` motivo dell'eventuale blocco.
Durante la calibrazione il blocco centrale mostra invece `calib=STAGE peak=.. trough=..`.
Il log avviene **prima** delle decisioni di gating, così si vede perché il controllo è fermo.

## Costanti ancora da tarare

Marcate `// TUNE` in `config.js`. Per il controllo a estremi, le più sensibili:

- **`LOCAL_DECAY`** — manopola principale del *feel*. Piccolo → un plateau ferma lo zoom e
  bisogna *spingere ancora* per proseguire (fasico); grande → controllo più continuo.
- **`EXTREMA_GAIN`** — velocità normalizzata massima (ex `ZOOM_GAIN`).
- **`CALIB_CONC_FRACTION`** — a quale frazione del tragitto `M`→estremo si satura (0.75).
- **`CALIB_INDEX_EMA`** — denoise sull'indice: più basso = più fluido ma più lento.
- **`CALIB_MIN_SPAN_REL`** — soglia di fallimento della calibrazione (span troppo debole).

Legacy/percentile (`P_LOW`/`P_HIGH`, `IQR_MIN_REL`, `ZOOM_GAIN`) restano solo per il ramo
`USE_ADAPTIVE_PIPELINE = false`.

## Test

Nessun hardware richiesto: verificano la logica di controllo e la catena DSP con segnali
sintetici e pacchetti BLE costruiti a mano.

```bash
node tests/control-pipeline.test.js   # controllo a estremi, banda locale, calibrazione, FSM, detent, budget
node tests/stft.test.js               # rate STFT, gating artefatti/contatto, picco spettrale
```

Coprono i criteri di accettazione §10 verificabili senza un cervello umano collegato.
Restano da validare sul campo: aggancio attenzione, sforzo percepito nel "tenere" una scala,
blink/mascella su segnale reale, frame rate su GPU integrata.
