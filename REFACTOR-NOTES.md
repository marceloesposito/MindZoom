# Refactor: calibrazione adattiva, controllo BCI, performance

Branch: `feature/adaptive-calibration-testing` (da `feature/muse-ble-testing`).

Tutto il comportamento nuovo è dietro `CONFIG.USE_ADAPTIVE_PIPELINE` (`config.js`).
Con `false` si torna al percorso legacy (snapshot 5 s + z-score/sigmoide) per confronto A/B.

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

## Costanti ancora da tarare

Marcate `// TUNE` in `config.js`. Le più sensibili all'esperienza reale:
`ZOOM_GAIN`, `IQR_MIN_REL`, `ENTER_HOLD`/`BREAK_HOLD`, `HOOK_AUTO_VELOCITY`,
`CONTACT_STD_MIN`/`CONTACT_STD_MAX`.

`P_LOW`/`P_HIGH` a 0.40/0.60 danno una dead-zone del 20% del range: se l'utente fatica a
fermarsi, allargarla prima di toccare il gain.

## Test

Nessun hardware richiesto: verificano la logica di controllo e la catena DSP con segnali
sintetici e pacchetti BLE costruiti a mano.

```bash
node tests/control-pipeline.test.js   # FSM, percentile, dead-zone, detent/isteresi, budget durata
node tests/stft.test.js               # rate STFT, gating artefatti/contatto, picco spettrale
```

Coprono i criteri di accettazione §10 verificabili senza un cervello umano collegato.
Restano da validare sul campo: aggancio attenzione, sforzo percepito nel "tenere" una scala,
blink/mascella su segnale reale, frame rate su GPU integrata.
