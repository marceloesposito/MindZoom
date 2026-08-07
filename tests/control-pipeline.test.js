// Harness headless: carica app.js in un contesto stubbato e verifica la legge di
// controllo a estremi (post-calibrazione) e la scheda di calibrazione attiva.
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
    Infinity,
    document: {
        addEventListener: (ev, cb) => { if (ev === 'DOMContentLoaded') domReady = cb; },
        getElementById: () => null
    },
    window: { addEventListener: () => {} },
    PIXI: {},
    MuseBluetooth: class {
        onEEGData(cb) { this._cb = cb; }
        onDisconnect(cb) { this._onDisc = cb; }
        onReconnect(cb) { this._onRecon = cb; }
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
globalThis.__x = { AppState, PHASE, CALIB, CONFIG,
    computeExtremaVelocity, updateSmoothedIndex, updateCalibrationSample,
    startCalibration, enterCalibStage, finalizeActiveCalibration, updateOnboardingModal,
    applyHoldSelect, updatePhase, enterPhase, resolveAuthorityVelocity,
    phaseAfterOnboarding, clamp01 };
`, sandbox);
domReady();

const S = sandbox.__x;
const CONFIG = S.CONFIG;
const { AppState, PHASE, CALIB } = S;
const DT = 1 / (256 / CONFIG.STFT_HOP);   // dt di un tick di controllo
const GAIN = CONFIG.EXTREMA_GAIN;

let pass = 0, fail = 0;
function check(name, cond, detail = '') {
    if (cond) { pass++; console.log(`  PASS  ${name}`); }
    else { fail++; console.log(`  FAIL  ${name}  ${detail}`); }
}

/** Imposta una calibrazione valida con estremi dati e banda locale al neutro. */
function calibrate(absMin, absMax) {
    AppState.absMin = absMin;
    AppState.absMax = absMax;
    AppState.neutralM = (absMin + absMax) / 2;
    AppState.localMin = AppState.neutralM;
    AppState.localMax = AppState.neutralM;
    AppState.calibValid = true;
}
function resetBand() { AppState.localMin = AppState.localMax = AppState.neutralM; }

// ---------------------------------------------------------------
console.log('\n[1] Neutro e direzione (media dei due estremi = zero)');
calibrate(0, 1);   // M = 0.5

resetBand();
const vNeutral = S.computeExtremaVelocity(0.5, DT);
check('concentrazione = neutro -> velocità nulla', Math.abs(vNeutral) < 1e-9, `v=${vNeutral}`);

resetBand();
const vUp = S.computeExtremaVelocity(0.9, DT);
check('sopra il neutro -> zoom in (v>0)', vUp > 0, `v=${vUp.toFixed(3)}`);

resetBand();
const vDown = S.computeExtremaVelocity(0.1, DT);
check('sotto il neutro -> zoom out (v<0)', vDown < 0, `v=${vDown.toFixed(3)}`);

// ---------------------------------------------------------------
console.log('\n[2] Banda locale di isteresi: fra i due estremi locali si tiene fermo');
calibrate(0, 1);
resetBand();
S.computeExtremaVelocity(0.95, DT);            // spinge in alto il max locale
S.computeExtremaVelocity(0.05, DT);            // spinge in basso il min locale
const vMid = S.computeExtremaVelocity(0.5, DT); // valore medio -> dentro la banda
check('valore centrale dentro la banda locale -> hold (v=0)', Math.abs(vMid) < 1e-9,
      `v=${vMid} lo=${AppState.localMin.toFixed(3)} hi=${AppState.localMax.toFixed(3)}`);

// ---------------------------------------------------------------
console.log('\n[3] Saturazione al CALIB_CONC_FRACTION del tragitto M->estremo');
calibrate(0, 1);   // M=0.5, satFrac*(absMax-M) = 0.75*0.5 = 0.375
resetBand();
const cSat = 0.5 + CONFIG.CALIB_CONC_FRACTION * 0.5;   // 0.875
const vSat = S.computeExtremaVelocity(cSat, DT);
check('a satFrac la velocità satura a EXTREMA_GAIN', Math.abs(vSat - GAIN) < 1e-9,
      `v=${vSat.toFixed(4)} gain=${GAIN}`);

resetBand();
const vBeyond = S.computeExtremaVelocity(1.0, DT);   // oltre la saturazione
check('oltre satFrac resta clampata a EXTREMA_GAIN', Math.abs(vBeyond - GAIN) < 1e-9,
      `v=${vBeyond.toFixed(4)}`);

// Simmetria in zoom out.
resetBand();
const vSatDown = S.computeExtremaVelocity(0.5 - CONFIG.CALIB_CONC_FRACTION * 0.5, DT);
check('saturazione simmetrica in zoom out', Math.abs(vSatDown + GAIN) < 1e-9,
      `v=${vSatDown.toFixed(4)}`);

// ---------------------------------------------------------------
console.log('\n[4] Nessun warm-up: velocità utile al primo tick dopo la calibrazione');
calibrate(0.2, 0.8);
resetBand();
const vFirst = S.computeExtremaVelocity(0.75, DT);   // primo tick in assoluto
check('il primo campione dopo la calibrazione produce già velocità', vFirst > 0,
      `v=${vFirst.toFixed(3)}`);

// Senza calibrazione valida la velocità è sempre nulla.
AppState.calibValid = false;
const vNoCalib = S.computeExtremaVelocity(0.9, DT);
check('senza calibrazione -> velocità nulla', vNoCalib === 0, `v=${vNoCalib}`);

// ---------------------------------------------------------------
console.log('\n[5] Registrazione estremi durante la calibrazione (dopo il lead-in)');
S.startCalibration();   // stage CONCENTRATE, picco=-Inf, minimo=+Inf
AppState.calibStageElapsed = CONFIG.CALIB_LEADIN_S + 0.1;
[0.3, 0.5, 0.9, 0.6].forEach(c => S.updateCalibrationSample(c));
check('CONCENTRATE registra il picco', AppState.calibPeak === 0.9, `peak=${AppState.calibPeak}`);

S.enterCalibStage(CALIB.RELAX);
AppState.calibStageElapsed = CONFIG.CALIB_LEADIN_S + 0.1;
[0.4, 0.15, 0.3].forEach(c => S.updateCalibrationSample(c));
check('RELAX registra il minimo', AppState.calibTrough === 0.15, `trough=${AppState.calibTrough}`);

// Il lead-in scarta il transitorio: campioni prima di CALIB_LEADIN_S non registrano.
S.startCalibration();
AppState.calibStageElapsed = 0.0;
S.updateCalibrationSample(2.0);   // spike durante il lead-in
check('durante il lead-in non si registra il picco', !isFinite(AppState.calibPeak),
      `peak=${AppState.calibPeak}`);

// ---------------------------------------------------------------
console.log('\n[6] Finalizzazione: estremi -> legge di controllo');
S.startCalibration();
AppState.calibStage = CALIB.RELAX;
AppState.calibPeak = 0.9;
AppState.calibTrough = 0.1;
S.finalizeActiveCalibration();
check('calibrazione valida', AppState.calibValid === true);
check('absMax/absMin salvati', AppState.absMax === 0.9 && AppState.absMin === 0.1);
check('neutro = media dei due estremi', Math.abs(AppState.neutralM - 0.5) < 1e-9,
      `M=${AppState.neutralM}`);
check('stage -> DONE', AppState.calibStage === CALIB.DONE);

// ---------------------------------------------------------------
console.log('\n[7] Fallimenti della calibrazione');
S.startCalibration();
AppState.calibStage = CALIB.RELAX;
AppState.calibPeak = 0.50;
AppState.calibTrough = 0.49;   // span ~0.01, sotto CALIB_MIN_SPAN_REL
S.finalizeActiveCalibration();
check('span troppo stretta -> FAILED', !AppState.calibValid && AppState.calibStage === CALIB.FAILED);

S.startCalibration();   // picco/minimo restano ±Infinity: segnale assente
S.finalizeActiveCalibration();
check('segnale assente -> FAILED', !AppState.calibValid && AppState.calibStage === CALIB.FAILED);

// ---------------------------------------------------------------
console.log('\n[8] FSM: ONBOARDING -> INTERACTIVE via calibrazione, durata illimitata');
AppState.phase = PHASE.ONBOARDING;
AppState.sessionElapsed = 0.0;
AppState.phaseElapsed = 0.0;
S.startCalibration();
AppState.calibStage = CALIB.RELAX;
AppState.calibPeak = 0.9;
AppState.calibTrough = 0.1;
S.finalizeActiveCalibration();   // -> DONE

let guard = 0;
while (AppState.phase === PHASE.ONBOARDING && guard++ < 100000) S.updatePhase(1 / 60);
check('dopo la conferma si passa a INTERACTIVE', AppState.phase === PHASE.INTERACTIVE,
      `phase=${AppState.phase}`);

CONFIG.ENABLE_TIME_LIMIT = false;
for (let i = 0; i < 60 * 60 * 10; i++) S.updatePhase(1 / 60);   // 10 minuti simulati
check('senza limite di tempo si resta in INTERACTIVE', AppState.phase === PHASE.INTERACTIVE,
      `phase=${AppState.phase} dopo ${AppState.phaseElapsed.toFixed(0)}s`);

// ---------------------------------------------------------------
console.log('\n[9] FSM: con il limite attivo la conclusione è garantita entro il budget');
CONFIG.ENABLE_TIME_LIMIT = true;
AppState.phase = PHASE.INTERACTIVE;
AppState.phaseElapsed = 0.0;
AppState.sessionElapsed = 0.0;
const phasesSeen = [];
guard = 0;
while (AppState.phase !== PHASE.DONE && guard++ < 1000000) {
    const before = AppState.phase;
    S.updatePhase(1 / 60);
    if (AppState.phase !== before) phasesSeen.push(AppState.phase);
}
const total = AppState.sessionElapsed;
check('sequenza fasi corretta', phasesSeen.join(',') === 'OUTRO,DONE', phasesSeen.join(','));
const budget = CONFIG.PHASE_INTERACTIVE_S + CONFIG.PHASE_OUTRO_S;
check(`durata totale ~ budget (${total.toFixed(1)}s vs ${budget}s)`,
      total >= budget - 2 && total <= budget + 5, `total=${total.toFixed(1)}`);
CONFIG.ENABLE_TIME_LIMIT = false;

// ---------------------------------------------------------------
console.log('\n[10] Hold / select: detent + isteresi (soglie relative a EXTREMA_GAIN)');
AppState.targetFocus = 0.46;
AppState.locked = false;
AppState.lockTimer = 0;
const step = 1 / 11;
const maxInput = CONFIG.EXTREMA_GAIN;
const enterHold = CONFIG.ENTER_HOLD_FRAC * maxInput;
const breakHold = CONFIG.BREAK_HOLD_FRAC * maxInput;

AppState.lockQuietTimer = 0;
AppState.lockRefractory = 0;

// L'aggancio richiede PERMANENZA: un singolo frame sotto soglia non è un utente
// fermo, è un attraversamento dello zero. Agganciando subito, un controllo che
// passa per zero fra un impulso e l'altro si riagganciava ad ogni frame e
// annullava il movimento appena ottenuto - era la ragione principale per cui lo
// zoom sembrava non partire.
let out = S.applyHoldSelect(enterHold * 0.2, 1 / 60);
check('un solo frame sotto ENTER_HOLD non aggancia', !AppState.locked,
      `locked=${AppState.locked} out=${out}`);

const framesToLock = Math.ceil(CONFIG.ENTER_HOLD_DWELL_S * 60) + 1;
for (let i = 0; i < framesToLock; i++) out = S.applyHoldSelect(enterHold * 0.2, 1 / 60);
check('dopo ENTER_HOLD_DWELL_S il detent aggancia e sopprime la velocità',
      AppState.locked && out === 0, `locked=${AppState.locked} out=${out}`);

for (let i = 0; i < 400; i++) S.applyHoldSelect(breakHold * 0.8, 1 / 60);
const nearest = Math.round(AppState.targetFocus / step) * step;
check('resta agganciato sotto BREAK_HOLD', AppState.locked === true);
check('converge sul livello (nessun jitter)', Math.abs(AppState.targetFocus - nearest) < 1e-3,
      `focus=${AppState.targetFocus.toFixed(5)} nearest=${nearest.toFixed(5)}`);

out = S.applyHoldSelect(maxInput, 1 / 60);
check('sopra BREAK_HOLD -> sgancio e velocità passa', !AppState.locked && out === maxInput,
      `locked=${AppState.locked} out=${out}`);

// Refrattarietà: dopo lo sgancio si ottiene una finestra di movimento vera. Senza,
// la velocità che ricade sotto soglia al frame dopo riagganciava subito e lo
// sforzo appena fatto veniva sprecato.
check('lo sgancio apre una finestra refrattaria', AppState.lockRefractory > 0,
      `refr=${AppState.lockRefractory.toFixed(2)}`);
for (let i = 0; i < framesToLock; i++) S.applyHoldSelect(enterHold * 0.2, 1 / 60);
check('durante la refrattarietà il lock non torna', !AppState.locked,
      `locked=${AppState.locked} refr=${AppState.lockRefractory.toFixed(2)}`);

AppState.lockRefractory = 0;
AppState.locked = true;
AppState.lockedLevel = 0.5;
AppState.lockTimer = CONFIG.LOCK_DWELL_S + 1;
out = S.applyHoldSelect(maxInput, 1 / 60);
check('alla velocità massima il detent è sempre sganciabile', !AppState.locked,
      `locked=${AppState.locked}`);

// ---------------------------------------------------------------
console.log('\n[10b] Tolleranza di attivazione e continuità dell\'uscita');

// È IL comportamento nuovo. Col bordo netto di prima, un indice che scendeva
// anche di poco sotto il massimo locale dava esattamente 0: l'uscita era un
// segnale commutato a 5.3 Hz, non un controllo.
calibrate(0, 1);   // M = 0.5
resetBand();
const tolAbs = CONFIG.LOCAL_TOLERANCE * 0.5;      // semi-span verso l'alto
S.computeExtremaVelocity(0.9, DT);                // fissa il massimo locale
const cProbe = 0.9 - 0.4 * tolAbs;                // dentro la rampa
const vPartial = S.computeExtremaVelocity(cProbe, DT);
check('dentro la rampa di tolleranza -> autorità PARZIALE, non zero', vPartial > 0,
      `v=${vPartial.toFixed(4)} gate=${AppState.ctrlGate.toFixed(3)}`);
check('l\'autorità parziale resta sotto il gain pieno', vPartial < GAIN,
      `v=${vPartial.toFixed(4)}`);

// Con tolleranza nulla, lo STESSO ingresso dà zero: è la prova che a cambiare le
// cose è la tolleranza e nient'altro.
const savedTol = CONFIG.LOCAL_TOLERANCE;
CONFIG.LOCAL_TOLERANCE = 0;
resetBand();
S.computeExtremaVelocity(0.9, DT);
const vHard = S.computeExtremaVelocity(cProbe, DT);
check('tolleranza 0 sullo stesso ingresso -> ritorna il gradino netto',
      Math.abs(vHard) < 1e-12, `v=${vHard}`);
CONFIG.LOCAL_TOLERANCE = savedTol;

// Zona morta: sul neutro esatto, fermo, senza deriva.
resetBand();
check('sul neutro -> velocità esattamente nulla (zona morta)',
      S.computeExtremaVelocity(0.5, DT) === 0);

// Continuità su segnale oscillante: è il caso in cui la legge precedente
// produceva il pettine, alternando zero e velocità piena.
calibrate(0, 1);
resetBand();
let zeros = 0, maxJump = 0, prevV = 0;
for (let i = 0; i < 120; i++) {
    const c = 0.78 + 0.05 * Math.sin(2 * Math.PI * i / 9);
    const v = S.computeExtremaVelocity(c, DT);
    if (i > 12) {
        if (v === 0) zeros++;
        maxJump = Math.max(maxJump, Math.abs(v - prevV));
    }
    prevV = v;
}
check('su segnale oscillante l\'uscita non collassa mai a zero', zeros === 0,
      `zeri=${zeros}`);
check('nessuno scalino oltre il 25% del gain fra campioni adiacenti',
      maxJump < 0.25 * GAIN, `max=${maxJump.toFixed(4)} soglia=${(0.25 * GAIN).toFixed(4)}`);

// ---------------------------------------------------------------
console.log('\n[10c] Denoise dell\'indice: la mediana precede l\'EMA');

// Un EMA da solo attenua un campione anomalo ma poi lo spalma sulla coda; la
// mediana lo elimina del tutto.
AppState.smoothedInit = false;
AppState.indexMedianBuf = [];
for (let i = 0; i < CONFIG.INDEX_MEDIAN_TAPS; i++) S.updateSmoothedIndex(2.0);
const beforeSpike = AppState.smoothedIndex;
const afterSpike = S.updateSmoothedIndex(50.0);
check('un singolo campione anomalo non passa la mediana',
      Math.abs(afterSpike - beforeSpike) < 1e-9,
      `prima=${beforeSpike.toFixed(6)} dopo=${afterSpike.toFixed(6)}`);

AppState.smoothedInit = false;
AppState.indexMedianBuf = [];
check('il primo campione inizializza senza transitorio',
      Math.abs(S.updateSmoothedIndex(3.0) - 3.0) < 1e-12);

// ---------------------------------------------------------------
console.log('\n[11] Crossfade di autorità in HANDOVER');
AppState.phase = PHASE.HANDOVER;
AppState.eegVelocity = 0.0;
AppState.phaseElapsed = 0;
const vStart = S.resolveAuthorityVelocity();
AppState.phaseElapsed = CONFIG.PHASE_HANDOVER_S;
const vEnd = S.resolveAuthorityVelocity();
check('a t=0 comanda l\'auto-zoom', Math.abs(vStart - CONFIG.HOOK_AUTO_VELOCITY) < 1e-9, `v=${vStart}`);
check('a t=fine comanda l\'EEG', Math.abs(vEnd - 0.0) < 1e-9, `v=${vEnd}`);

// ---------------------------------------------------------------
console.log('\n[12] Rate di controllo');
const updateHz = 256 / CONFIG.STFT_HOP;
check(`update del controllo 4-8 Hz (${updateHz.toFixed(1)} Hz)`, updateHz >= 4 && updateHz <= 8);
const latencyMs = (CONFIG.STFT_HOP / 256) * 1000;
check(`latenza di hop < 300 ms (${latencyMs.toFixed(0)} ms)`, latencyMs < 300);

console.log(`\n=== ${pass} passed, ${fail} failed ===`);
process.exit(fail > 0 ? 1 : 0);
