// Harness headless: carica app.js in un contesto stubbato e verifica
// i criteri di accettazione della pipeline di controllo.
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const ROOT = path.join(__dirname, '..');

const configSrc = fs.readFileSync(path.join(ROOT, 'config.js'), 'utf8');
const appSrc = fs.readFileSync(path.join(ROOT, 'app.js'), 'utf8');

let domReady = null;
const sandbox = {
    console,
    Math,
    Date,
    Float32Array,
    Promise,
    document: {
        addEventListener: (ev, cb) => { if (ev === 'DOMContentLoaded') domReady = cb; },
        getElementById: () => null
    },
    window: { addEventListener: () => {} },
    PIXI: {},
    MuseBluetooth: class {
        onEEGData(cb) { this._cb = cb; }
        onDisconnect(cb) { this._onDisc = cb; }
        sendControlCommand() { return Promise.resolve(null); }
        connect() {}
    }
};
sandbox.window.CONFIG = null;
vm.createContext(sandbox);

vm.runInContext(configSrc, sandbox);
// I `const` top-level restano nello scope lessicale dello script, non su globalThis:
// li esporto esplicitamente per il test.
vm.runInContext(appSrc + `
globalThis.__x = { AppState, PHASE, Normalizer, CONFIG,
    computeAdaptiveVelocity, applyHoldSelect, updatePhase, enterPhase,
    resolveAuthorityVelocity, median, medianAbsoluteDeviation, phaseAfterOnboarding };
`, sandbox);
domReady();

const S = sandbox.__x;
const CONFIG = S.CONFIG;
const { AppState, PHASE, Normalizer } = S;

let pass = 0, fail = 0;
function check(name, cond, detail = '') {
    if (cond) { pass++; console.log(`  PASS  ${name}`); }
    else { fail++; console.log(`  FAIL  ${name}  ${detail}`); }
}

function feed(values) {
    for (const v of values) Normalizer.push(v);
}

// In produzione processAdaptiveIndex assegna il risultato ad AppState.eegVelocity,
// che è lo stato su cui si appoggia lo smoothing. Qui si replica lo stesso ciclo.
function drive(index, iterations) {
    for (let i = 0; i < iterations; i++) {
        AppState.eegVelocity = S.computeAdaptiveVelocity(index);
    }
    return AppState.eegVelocity;
}

// ---------------------------------------------------------------
console.log('\n[1] Mapping percentile -> velocità (dead-zone)');
Normalizer.reset();
// Distribuzione ampia e regolare: IQR relativo ben sopra la guardia.
const spread = [];
for (let i = 0; i < 60; i++) spread.push(0.5 + i * 0.02);
feed(spread);

AppState.eegVelocity = 0;
const vMid = S.computeAdaptiveVelocity(spread[30]);   // ~mediana -> dead zone
check('valore centrale -> velocità nulla', Math.abs(vMid) < 1e-6, `v=${vMid}`);

// Il mapping satura a ZOOM_GAIN, non a 1.0: le attese sono relative al gain.
const GAIN = CONFIG.ZOOM_GAIN;

AppState.eegVelocity = 0;
const vHigh = drive(spread[59], 40);   // converge lo smoothing
check('valore alto -> velocità positiva (zoom in)', vHigh > GAIN * 0.9,
      `v=${vHigh.toFixed(3)} gain=${GAIN}`);

AppState.eegVelocity = 0;
const vLow = drive(spread[0], 40);
check('valore basso -> velocità negativa', vLow < -GAIN * 0.9,
      `v=${vLow.toFixed(3)} gain=${GAIN}`);

// ---------------------------------------------------------------
console.log('\n[2] Dispersion guard (buffer piatto)');
Normalizer.reset();
feed(new Array(60).fill(1.0));
AppState.eegVelocity = 0;
const vFlat = S.computeAdaptiveVelocity(1.0001);
check('buffer piatto -> congelamento verso 0', Math.abs(vFlat) < 1e-6, `v=${vFlat}`);

// ---------------------------------------------------------------
console.log('\n[3] Partenza "stressata" non blocca lo zoom in alto');
Normalizer.reset();
// L'utente parte alto (ansia) per ~4s, poi si assesta su valori più bassi.
for (let i = 0; i < 20; i++) Normalizer.push(3.0 + Math.random() * 0.3);
AppState.eegVelocity = 0;
drive(3.1, 20);

// Ora l'utente si calma: valori sistematicamente più bassi entrano nel ring.
for (let i = 0; i < 60; i++) Normalizer.push(1.0 + Math.random() * 0.3);
AppState.eegVelocity = 0;
const vAfter = drive(1.15, 40);
check('dopo l\'assestamento il riferimento si è spostato (non pinnato in alto)',
      Math.abs(vAfter) < GAIN * 0.5, `v=${vAfter.toFixed(3)}`);

AppState.eegVelocity = 0;
const vSpikeAfter = drive(1.45, 40);
check('la modulazione fasica torna a pilotare', vSpikeAfter > GAIN * 0.5,
      `v=${vSpikeAfter.toFixed(3)}`);

// ---------------------------------------------------------------
console.log('\n[4] Hold / select: detent + isteresi');
AppState.targetFocus = 0.46;   // fra due livelli (step = 1/11 = 0.0909)
AppState.locked = false;
AppState.lockTimer = 0;
AppState.inputMode = 'BCI';
const step = 1 / 11;
// Le soglie sono frazioni della velocità massima di input (ZOOM_GAIN in BCI).
const maxInput = CONFIG.ZOOM_GAIN;
const enterHold = CONFIG.ENTER_HOLD_FRAC * maxInput;
const breakHold = CONFIG.BREAK_HOLD_FRAC * maxInput;

// Velocità bassa -> aggancio
let out = S.applyHoldSelect(enterHold * 0.2, 1 / 60);
check('velocità sotto ENTER_HOLD -> aggancio e velocità soppressa',
      AppState.locked && out === 0, `locked=${AppState.locked} out=${out}`);

// Iterazioni con velocità sotto BREAK_HOLD -> resta agganciato e converge al livello
for (let i = 0; i < 400; i++) S.applyHoldSelect(breakHold * 0.8, 1 / 60);
const nearest = Math.round(AppState.targetFocus / step) * step;
check('resta agganciato sotto BREAK_HOLD', AppState.locked === true);
check('converge sul livello (nessun jitter)', Math.abs(AppState.targetFocus - nearest) < 1e-3,
      `focus=${AppState.targetFocus.toFixed(5)} nearest=${nearest.toFixed(5)}`);

const focusWhileHeld = AppState.targetFocus;
for (let i = 0; i < 200; i++) S.applyHoldSelect(breakHold * 0.5, 1 / 60);
check('nessuna deriva mentre è tenuto', Math.abs(AppState.targetFocus - focusWhileHeld) < 1e-4,
      `drift=${Math.abs(AppState.targetFocus - focusWhileHeld)}`);

// Superare BREAK_HOLD -> sgancio
out = S.applyHoldSelect(maxInput, 1 / 60);
check('sopra BREAK_HOLD -> sgancio e velocità passa', !AppState.locked && out === maxInput,
      `locked=${AppState.locked} out=${out}`);

// Dwell-to-lock: dopo LOCK_DWELL_S serve più sforzo
AppState.locked = true;
AppState.lockedLevel = 0.5;
AppState.lockTimer = CONFIG.LOCK_DWELL_S + 1;
const justAboveBreak = breakHold * 1.1;   // sopra BREAK, sotto BREAK*MULT
out = S.applyHoldSelect(justAboveBreak, 1 / 60);
check('dopo il dwell la soglia di sgancio è più alta', AppState.locked === true && out === 0,
      `locked=${AppState.locked} out=${out}`);

// Un detent deve restare sganciabile qualunque sia il gain: alla velocità massima
// che l'input può produrre lo sgancio deve sempre avvenire.
AppState.locked = true;
AppState.lockedLevel = 0.5;
AppState.lockTimer = CONFIG.LOCK_DWELL_S + 1;
out = S.applyHoldSelect(maxInput, 1 / 60);
check('alla velocità massima il detent è sempre sganciabile', !AppState.locked,
      `locked=${AppState.locked}`);

// ---------------------------------------------------------------
console.log('\n[5] FSM: durata illimitata e budget opzionale');
AppState.phase = PHASE.ONBOARDING;
AppState.phaseElapsed = 0;
AppState.sessionElapsed = 0;

const dt = 1 / 60;
// L'utente chiude la modale appena possibile.
let guard = 0;
while (AppState.phaseElapsed < CONFIG.MODAL_UNLOCK_S && guard++ < 100000) S.updatePhase(dt);
check("senza hook si va dritti all'interazione",
      S.phaseAfterOnboarding() === PHASE.INTERACTIVE, S.phaseAfterOnboarding());
S.enterPhase(S.phaseAfterOnboarding());

// Default: nessun limite di tempo, l'interazione non scade mai da sola.
CONFIG.ENABLE_TIME_LIMIT = false;
for (let i = 0; i < 60 * 60 * 10; i++) S.updatePhase(dt);   // 10 minuti simulati
check('senza limite di tempo si resta in INTERACTIVE',
      AppState.phase === PHASE.INTERACTIVE,
      `phase=${AppState.phase} dopo ${AppState.phaseElapsed.toFixed(0)}s`);

// Con il limite attivo la conclusione resta garantita entro il budget.
CONFIG.ENABLE_TIME_LIMIT = true;
AppState.phase = PHASE.ONBOARDING;
AppState.phaseElapsed = 0;
AppState.sessionElapsed = 0;
guard = 0;
while (AppState.phaseElapsed < CONFIG.MODAL_UNLOCK_S && guard++ < 100000) S.updatePhase(dt);
S.enterPhase(S.phaseAfterOnboarding());

const phasesSeen = [];
guard = 0;
while (AppState.phase !== PHASE.DONE && guard++ < 1000000) {
    const before = AppState.phase;
    S.updatePhase(dt);
    if (AppState.phase !== before) phasesSeen.push(AppState.phase);
}
const total = AppState.sessionElapsed;
check('sequenza fasi corretta', phasesSeen.join(',') === 'OUTRO,DONE', phasesSeen.join(','));
check(`durata totale entro 90-120s (${total.toFixed(1)}s)`, total >= 90 && total <= 120,
      `total=${total.toFixed(1)}`);

// ---------------------------------------------------------------
console.log('\n[6] Crossfade di autorità in HANDOVER');
AppState.phase = PHASE.HANDOVER;
AppState.inputMode = 'BCI';
AppState.eegVelocity = 0.0;
AppState.phaseElapsed = 0;
const vStart = S.resolveAuthorityVelocity();
AppState.phaseElapsed = CONFIG.PHASE_HANDOVER_S;
const vEnd = S.resolveAuthorityVelocity();
check('a t=0 comanda l\'auto-zoom', Math.abs(vStart - CONFIG.HOOK_AUTO_VELOCITY) < 1e-9,
      `v=${vStart}`);
check('a t=fine comanda l\'EEG', Math.abs(vEnd - 0.0) < 1e-9, `v=${vEnd}`);

// ---------------------------------------------------------------
console.log('\n[7] Rate di controllo');
const updateHz = 256 / CONFIG.STFT_HOP;
check(`update del controllo 4-8 Hz (${updateHz.toFixed(1)} Hz)`, updateHz >= 4 && updateHz <= 8);
const latencyMs = (CONFIG.STFT_HOP / 256) * 1000;
check(`latenza di hop < 300 ms (${latencyMs.toFixed(0)} ms)`, latencyMs < 300);
check(`ring buffer percentile dimensionato (${Normalizer.capacity} campioni)`,
      Normalizer.capacity >= 60 && Normalizer.capacity <= 120);

console.log(`\n=== ${pass} passed, ${fail} failed ===`);
process.exit(fail > 0 ? 1 : 0);
