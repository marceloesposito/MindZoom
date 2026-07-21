/**
 * MIND ZOOM - SEM Microscope Interactive Experience
 *
 * Pipeline adattiva:
 *   FSM a fasi (onboarding -> hook -> handover -> interactive -> outro)
 *   + calibrazione seminata sotto la modale (mediana/MAD)
 *   + normalizzazione percentile su ring buffer scorrevole
 *   + hold/select (dead-zone, detent, isteresi, dwell-to-lock).
 *
 * Il percorso legacy (calibrazione a snapshot 5s, z-score/sigmoide) resta
 * selezionabile via CONFIG.USE_ADAPTIVE_PIPELINE = false per confronto A/B.
 */

const SCALE_LABELS = [32, 32, 100, 220, 700, 1500, 3000, 6000, 10000, 17000, 25000, 41000];
const TOTAL_IMAGES = 12;
const IMAGE_EXTENSION = '.webp';
const IMAGE_PATH = '/images/';

const FOCUS_EASING = 0.06;

// Parametri geometrici per il controllo di velocità (Rate Control) - invariati
const DEAD_ZONE = 0.08;
const ZOOM_SPEED_FACTOR = 0.003;

// Costanti specifiche per la simulazione via Mouse Wheel
const MOUSE_ACCELERATION = 0.15;
const MOUSE_FRICTION = 0.92;

// --- Stato calibrazione legacy (usato solo se USE_ADAPTIVE_PIPELINE = false) ---
const CALIBRATION_DURATION = 5;
let calibrationBuffer = [];
let isCalibrated = false;
let bciMean = 1.0;
let bciStdDev = 0.15;
const LEARNING_RATE = 0.02;

const PHASE = {
    IDLE: "IDLE",
    ONBOARDING: "ONBOARDING",
    HOOK: "HOOK",
    HANDOVER: "HANDOVER",
    INTERACTIVE: "INTERACTIVE",
    OUTRO: "OUTRO",
    DONE: "DONE"
};

const AppState = {
    currentView: "LANDING",
    inputMode: "BCI",
    targetFocus: 0.0,
    currentFocus: 0.0,
    isMuseConnected: false,
    rawRatio: 0.0,
    targetVelocity: 0.0,   // velocità della rotella (SIMULATION)

    // --- FSM ---
    phase: PHASE.IDLE,
    phaseElapsed: 0.0,
    sessionElapsed: 0.0,

    // --- Pipeline adattiva ---
    eegVelocity: 0.0,      // velocità derivata dall'EEG (autorità gestita dalla FSM)
    percentile: 0.5,
    isGated: false,        // artefatto d'ampiezza sulla finestra corrente
    contactOk: true,       // proxy qualità contatto
    gatedWindows: 0,       // finestre consecutive scartate
    seeded: false,         // semina mediana/MAD riuscita

    // --- Hold / select ---
    locked: false,
    lockedLevel: 0.0,
    lockTimer: 0.0
};

let lastRenderedMagnification = -1;
let pixiApp = null;
let spritePool = [];
let hudTimer = 0.0;

const DOM = {};

const muse = new MuseBluetooth();

/* ------------------------------------------------------------------ *
 *  Normalizzazione percentile su ring buffer scorrevole
 * ------------------------------------------------------------------ */

const Normalizer = {
    capacity: 0,
    values: null,
    scratch: null,
    count: 0,
    writeIdx: 0,
    seedMedian: 0,
    seedMAD: 0,
    hasSeed: false,

    init() {
        // Dimensione = secondi di finestra * rate di update (FS / hop)
        const updateHz = 256 / (CONFIG.STFT_HOP || 48);
        this.capacity = Math.max(24, Math.round(CONFIG.PCTL_BUFFER_S * updateHz));
        this.values = new Float32Array(this.capacity);
        this.scratch = new Float32Array(this.capacity);
        this.reset();
    },

    reset() {
        this.count = 0;
        this.writeIdx = 0;
        this.seedMedian = 0;
        this.seedMAD = 0;
        this.hasSeed = false;
    },

    seed(median, mad) {
        this.seedMedian = median;
        this.seedMAD = mad;
        this.hasSeed = true;
    },

    push(v) {
        this.values[this.writeIdx] = v;
        this.writeIdx = (this.writeIdx + 1) % this.capacity;
        if (this.count < this.capacity) this.count++;
    },

    /** Frazione di campioni del buffer <= v, in [0,1]. O(n), n ~70. */
    percentileOf(v) {
        if (this.count === 0) return 0.5;
        let below = 0;
        for (let i = 0; i < this.count; i++) {
            if (this.values[i] <= v) below++;
        }
        return below / this.count;
    },

    /** IQR relativo alla mediana del buffer: adimensionale, person-independent. */
    relativeIQR() {
        if (this.count < 8) return 0;
        const s = this.scratch.subarray(0, this.count);
        s.set(this.values.subarray(0, this.count));
        s.sort();
        const q1 = s[Math.floor(this.count * 0.25)];
        const q2 = s[Math.floor(this.count * 0.50)];
        const q3 = s[Math.floor(this.count * 0.75)];
        if (q2 <= 0) return 0;
        return (q3 - q1) / q2;
    }
};

/* ------------------------------------------------------------------ *
 *  Statistiche robuste
 * ------------------------------------------------------------------ */

function median(arr) {
    if (arr.length === 0) return 0;
    const s = arr.slice().sort((a, b) => a - b);
    const mid = s.length >> 1;
    return (s.length % 2 === 0) ? (s[mid - 1] + s[mid]) / 2 : s[mid];
}

/** Median Absolute Deviation: immune agli spike da blink/tensione. */
function medianAbsoluteDeviation(arr, med) {
    if (arr.length === 0) return 0;
    const deviations = arr.map(v => Math.abs(v - med));
    return median(deviations);
}

/* ------------------------------------------------------------------ *
 *  Bootstrap
 * ------------------------------------------------------------------ */

document.addEventListener("DOMContentLoaded", () => {
    DOM.landingContainer = document.getElementById('landing-container');
    DOM.immersionContainer = document.getElementById('immersion-container');
    DOM.pixiCanvas = document.getElementById('pixi-canvas');
    DOM.beginImmersionBtn = document.getElementById('begin-immersion-btn');
    DOM.endImmersionBtn = document.getElementById('end-immersion-btn');
    DOM.establishConnectionBtn = document.getElementById('establish-connection-btn');
    DOM.immersionMagLabel = document.getElementById('immersion-mag-label');
    DOM.modalFindDeviceBtn = document.getElementById('modal-find-device-btn');
    DOM.homeLed = document.getElementById('home-led');
    DOM.homeDeviceString = document.getElementById('home-device-string');
    DOM.immersionStatusLed = document.getElementById('immersion-status-led');
    DOM.connectionModal = document.getElementById('connection-modal');
    DOM.modalCloseConnBtn = document.getElementById('modal-close-conn-btn');
    DOM.hamburgerMenuBtn = document.getElementById('hamburger-menu-btn');
    DOM.hamburgerModal = document.getElementById('hamburger-modal');
    DOM.menuCloseBtn = document.getElementById('menu-close-btn');
    DOM.toggleInputModeBtn = document.getElementById('toggle-input-mode-btn');

    DOM.onboardingModal = document.getElementById('onboarding-modal');
    DOM.onboardingStartBtn = document.getElementById('onboarding-start-btn');
    DOM.onboardingHint = document.getElementById('onboarding-hint');

    DOM.debugInputSource = document.getElementById('debug-input-source');
    DOM.debugVelocity = document.getElementById('debug-velocity');
    DOM.debugRawRatio = document.getElementById('debug-raw-ratio');
    DOM.debugTarget = document.getElementById('debug-target');
    DOM.debugCurrent = document.getElementById('debug-current');
    DOM.debugBarTarget = document.getElementById('debug-bar-target');
    DOM.debugBarCurrent = document.getElementById('debug-bar-current');
    DOM.debugPhase = document.getElementById('debug-phase');
    DOM.debugPercentile = document.getElementById('debug-percentile');
    DOM.debugGate = document.getElementById('debug-gate');

    Normalizer.init();
    initUIEventListeners();
    initMouseWheelController();
    updateSynchronizedLEDs(false);
});

function initUIEventListeners() {
    if (DOM.beginImmersionBtn) DOM.beginImmersionBtn.addEventListener('click', () => switchView("IMMERSION"));
    if (DOM.endImmersionBtn) DOM.endImmersionBtn.addEventListener('click', () => switchView("LANDING"));
    if (DOM.establishConnectionBtn) DOM.establishConnectionBtn.addEventListener('click', () => DOM.connectionModal?.classList.remove('hidden'));
    if (DOM.modalCloseConnBtn) DOM.modalCloseConnBtn.addEventListener('click', () => DOM.connectionModal?.classList.add('hidden'));
    if (DOM.hamburgerMenuBtn) DOM.hamburgerMenuBtn.addEventListener('click', () => DOM.hamburgerModal?.classList.remove('hidden'));
    if (DOM.menuCloseBtn) DOM.menuCloseBtn.addEventListener('click', () => DOM.hamburgerModal?.classList.add('hidden'));

    if (DOM.onboardingStartBtn) {
        DOM.onboardingStartBtn.addEventListener('click', () => {
            if (AppState.phase !== PHASE.ONBOARDING) return;
            if (AppState.phaseElapsed < CONFIG.MODAL_UNLOCK_S) return;
            closeOnboardingModal();
            enterPhase(phaseAfterOnboarding());
        });
    }

    if (DOM.toggleInputModeBtn) {
        DOM.toggleInputModeBtn.addEventListener('click', () => {
            if (AppState.inputMode === "BCI") {
                AppState.inputMode = "SIMULATION";
                DOM.toggleInputModeBtn.innerText = "MODE: WHEEL SIM";
                DOM.toggleInputModeBtn.className = "bg-green-600/80 hover:bg-green-500 backdrop-blur-md border border-green-500 text-white font-mono text-xs uppercase tracking-wider px-4 py-2.5 rounded-xl transition-all shadow-md pointer-events-auto";
                if (DOM.debugInputSource) {
                    DOM.debugInputSource.innerText = "WHEEL";
                    DOM.debugInputSource.className = "font-bold text-green-400";
                }
                AppState.targetVelocity = 0.0;
            } else {
                AppState.inputMode = "BCI";
                DOM.toggleInputModeBtn.innerText = "MODE: BCI (MUSE)";
                DOM.toggleInputModeBtn.className = "bg-blue-600/80 hover:bg-blue-500 backdrop-blur-md border border-blue-500 text-white font-mono text-xs uppercase tracking-wider px-4 py-2.5 rounded-xl transition-all shadow-md pointer-events-auto";
                if (DOM.debugInputSource) {
                    DOM.debugInputSource.innerText = "BCI";
                    DOM.debugInputSource.className = "font-bold text-blue-400";
                }
                AppState.targetVelocity = 0.0;
            }
        });
    }

    if (DOM.modalFindDeviceBtn) {
        DOM.modalFindDeviceBtn.addEventListener('click', async () => {
            try {
                await muse.connect();
                updateSynchronizedLEDs(true, muse.device.name);
                DOM.connectionModal?.classList.add('hidden');
            } catch(e) {
                console.error("[ERROR] Connessione fallita:", e);
                updateSynchronizedLEDs(false);
            }
        });
    }

    muse.onEEGData((brainData) => {
        if (!brainData.fft || AppState.inputMode !== "BCI") return;
        if (CONFIG.USE_ADAPTIVE_PIPELINE) {
            processAdaptiveIndex(brainData);
        } else {
            calculateScientificFocusLegacy(brainData.fft);
        }
    });
}

function initMouseWheelController() {
    window.addEventListener('wheel', (event) => {
        if (AppState.currentView !== "IMMERSION" || AppState.inputMode !== "SIMULATION") return;
        event.preventDefault();

        const direction = event.deltaY < 0 ? 1 : -1;
        AppState.targetVelocity += direction * MOUSE_ACCELERATION;
        AppState.targetVelocity = Math.max(-1.0, Math.min(1.0, AppState.targetVelocity));
    }, { passive: false });
}

/* ------------------------------------------------------------------ *
 *  Indice di engagement (Pope 1995): beta / (alpha + theta)
 * ------------------------------------------------------------------ */

// Preallocato: popolato ad ogni finestra, usato dai log di debug (zero allocazioni).
const Bands = { theta: 0, alpha: 0, beta: 0 };

function computePopeIndex(fftData) {
    if (!fftData || !fftData.mags) return null;

    let totalTheta = 0;
    let totalAlpha = 0;
    let totalBeta = 0;
    const frontalChannels = [1, 2]; // AF7, AF8

    for (let k = 0; k < frontalChannels.length; k++) {
        const channelMags = fftData.mags[frontalChannels[k]];
        if (!channelMags) continue;

        for (let b = 4;  b <= 8;  b++) totalTheta += channelMags[b] || 0;
        for (let b = 8;  b <= 12; b++) totalAlpha += channelMags[b] || 0;
        for (let b = 13; b <= 30; b++) totalBeta  += channelMags[b] || 0;
    }

    Bands.theta = totalTheta;
    Bands.alpha = totalAlpha;
    Bands.beta = totalBeta;

    const denom = totalAlpha + totalTheta;
    if (denom === 0) return null;
    return totalBeta / denom;
}

/** Rate di aggiornamento del controllo, derivato dall'hop della STFT. */
function controlRateHz() {
    return 256 / (CONFIG.STFT_HOP || 48);
}

let debugLogCounter = 0;

/**
 * Log periodico di cosa sta effettivamente leggendo il sensore.
 * Viene chiamato PRIMA delle decisioni di gating, così si vede anche il motivo
 * per cui il controllo eventualmente non si muove.
 */
function logSensorState(quality, index) {
    if (!CONFIG.DEBUG_SENSOR_LOG) return;

    const every = Math.max(1, Math.round(controlRateHz() / CONFIG.DEBUG_LOG_HZ));
    if (++debugLogCounter % every !== 0) return;

    let gate = 'OK';
    if (!quality.contactOk) gate = 'CONTATTO SCARSO';
    else if (quality.artifact) gate = 'ARTEFATTO';

    const idx = (index === null) ? '  n/a' : index.toFixed(3);
    const pct = (Normalizer.count >= 16) ? AppState.percentile.toFixed(2) : ' --ph';
    const iqr = Normalizer.relativeIQR();

    console.log(
        `[EEG] ${AppState.phase.padEnd(11)} idx=${idx} p=${pct} v=${AppState.eegVelocity >= 0 ? '+' : ''}${AppState.eegVelocity.toFixed(3)}` +
        ` | θ=${Bands.theta.toFixed(2)} α=${Bands.alpha.toFixed(2)} β=${Bands.beta.toFixed(2)}` +
        ` | ring=${Normalizer.count}/${Normalizer.capacity} IQRrel=${iqr.toFixed(3)}` +
        ` | ampiezza=${quality.maxAbsRaw.toFixed(0)}µV gate=${gate}` +
        ` | adc=[${(quality.adcMin || 0).toFixed(0)}..${(quality.adcMax || 0).toFixed(0)}] µ=${(quality.adcMean || 0).toFixed(0)}`
    );
}

/* ------------------------------------------------------------------ *
 *  Pipeline adattiva
 * ------------------------------------------------------------------ */

let calibWindowBuffer = [];

function processAdaptiveIndex(brainData) {
    const quality = brainData.quality || { artifact: false, contactOk: true, maxAbsRaw: 0 };

    // L'indice si calcola sempre: serve anche solo per la diagnostica, così quando
    // il controllo è fermo si vede in console *perché* è fermo.
    const index = computePopeIndex(brainData.fft);
    if (index !== null) AppState.rawRatio = index;

    AppState.contactOk = quality.contactOk;
    AppState.isGated = quality.artifact || !quality.contactOk;
    logSensorState(quality, index);

    // Gating qualità contatto: meglio uno zoom fermo che uno impazzito.
    if (!quality.contactOk) {
        AppState.eegVelocity = 0.0;
        AppState.gatedWindows++;
        return;
    }

    // Gating artefatti: la finestra sporca non viene data in pasto all'indice.
    // Si mantiene la velocità precedente invece di azzerarla, per non introdurre
    // uno scatto ad ogni blink. Ma un gating che dura troppo non deve incollare lo
    // zoom: oltre ARTIFACT_HOLD_MAX_S la velocità decade verso 0.
    if (quality.artifact) {
        AppState.gatedWindows++;
        if (AppState.gatedWindows > CONFIG.ARTIFACT_HOLD_MAX_S * controlRateHz()) {
            AppState.eegVelocity *= 0.85;
        }
        return;
    }

    AppState.gatedWindows = 0;
    if (index === null) return;

    // Durante ONBOARDING conta solo la finestra pulita: si scartano il transitorio
    // iniziale e la zona del gesto di chiusura (saccade verso il pulsante, blink,
    // micro-movimento della testa). Quelle finestre non devono inquinare nemmeno
    // il riferimento percentile.
    if (AppState.phase === PHASE.ONBOARDING) {
        const start = CONFIG.CALIB_START_S;
        const end = CONFIG.MODAL_UNLOCK_S - CONFIG.CALIB_END_OFFSET_S;
        if (AppState.phaseElapsed < start || AppState.phaseElapsed > end) return;
        calibWindowBuffer.push(index);
    }

    // Il percentile si calcola PRIMA di inserire il campione corrente: altrimenti il
    // campione conterebbe sé stesso e p non potrebbe mai valere 0.
    AppState.eegVelocity = computeAdaptiveVelocity(index);

    // Il ring buffer percentile si riempie già da ONBOARDING (finestra pulita),
    // così alla chiusura della modale il sistema è già reattivo.
    Normalizer.push(index);
}

function computeAdaptiveVelocity(index) {
    // Serve un minimo di storia prima di dare autorità al percentile.
    if (Normalizer.count < 16) return 0.0;

    // Dispersion guard: buffer troppo piatto -> il percentile diventa nervoso
    // su micro-variazioni prive di significato. Meglio congelare.
    const relIQR = Normalizer.relativeIQR();
    if (relIQR < CONFIG.IQR_MIN_REL) {
        AppState.percentile = 0.5;
        return smoothVelocity(0.0);
    }

    const p = Normalizer.percentileOf(index);
    AppState.percentile = p;

    let target;
    if (p >= CONFIG.P_LOW && p <= CONFIG.P_HIGH) {
        target = 0.0;                                                     // DEAD ZONE -> hold
    } else if (p > CONFIG.P_HIGH) {
        target = CONFIG.ZOOM_GAIN * (p - CONFIG.P_HIGH) / (1 - CONFIG.P_HIGH);
    } else {
        target = -CONFIG.ZOOM_GAIN * (CONFIG.P_LOW - p) / CONFIG.P_LOW;
    }

    target = Math.max(-1.0, Math.min(1.0, target));
    return smoothVelocity(target);
}

function smoothVelocity(target) {
    const a = CONFIG.VEL_SMOOTHING;
    return AppState.eegVelocity + (target - AppState.eegVelocity) * a;
}

function finalizeCalibration() {
    if (calibWindowBuffer.length >= CONFIG.CALIB_MIN_SAMPLES) {
        const med = median(calibWindowBuffer);
        const mad = medianAbsoluteDeviation(calibWindowBuffer, med);
        Normalizer.seed(med, mad);
        AppState.seeded = true;
        console.log(`[BCI] Semina calibrazione: mediana=${med.toFixed(3)} MAD=${mad.toFixed(3)} ` +
                    `(${calibWindowBuffer.length} campioni, finestra ` +
                    `[${CONFIG.CALIB_START_S}, ${(CONFIG.MODAL_UNLOCK_S - CONFIG.CALIB_END_OFFSET_S)}] s)`);
    } else {
        AppState.seeded = false;
        console.warn(`[BCI] Semina insufficiente (${calibWindowBuffer.length} campioni): ` +
                     `il percentile prenderà autorità appena il ring buffer si riempie.`);
    }
    calibWindowBuffer.length = 0;
}

/* ------------------------------------------------------------------ *
 *  Macchina a stati a fasi
 * ------------------------------------------------------------------ */

/** Fase che segue l'onboarding: dritti all'interazione salvo hook riabilitato. */
function phaseAfterOnboarding() {
    return CONFIG.ENABLE_HOOK_PHASE ? PHASE.HOOK : PHASE.INTERACTIVE;
}

function enterPhase(phase) {
    if (AppState.phase === PHASE.ONBOARDING && phase !== PHASE.ONBOARDING) {
        finalizeCalibration();
    }
    AppState.phase = phase;
    AppState.phaseElapsed = 0.0;
    console.log(`[FSM] -> ${phase} (t=${AppState.sessionElapsed.toFixed(1)}s)`);
}

function updatePhase(dt) {
    if (AppState.phase === PHASE.IDLE || AppState.phase === PHASE.DONE) return;

    AppState.phaseElapsed += dt;
    AppState.sessionElapsed += dt;

    switch (AppState.phase) {
        case PHASE.ONBOARDING:
            // Avanza solo alla chiusura della modale (gesto esplicito dell'utente).
            updateOnboardingModal();
            break;
        case PHASE.HOOK:
            if (AppState.phaseElapsed >= CONFIG.PHASE_HOOK_S) enterPhase(PHASE.HANDOVER);
            break;

        case PHASE.HANDOVER:
            if (AppState.phaseElapsed >= CONFIG.PHASE_HANDOVER_S) enterPhase(PHASE.INTERACTIVE);
            break;
        case PHASE.INTERACTIVE:
            if (AppState.phaseElapsed >= CONFIG.PHASE_INTERACTIVE_S) enterPhase(PHASE.OUTRO);
            break;
        case PHASE.OUTRO:
            if (AppState.phaseElapsed >= CONFIG.PHASE_OUTRO_S) enterPhase(PHASE.DONE);
            break;
    }
}

/** Velocità di input corrente, indipendentemente dall'autorità della fase. */
function currentInputVelocity() {
    return (AppState.inputMode === "SIMULATION") ? AppState.targetVelocity : AppState.eegVelocity;
}

/** Risolve chi comanda lo zoom nella fase corrente. */
function resolveAuthorityVelocity() {
    switch (AppState.phase) {
        case PHASE.ONBOARDING:
            return 0.0;                        // fermo in superficie
        case PHASE.HOOK:
            return CONFIG.HOOK_AUTO_VELOCITY;  // auto-zoom scriptato, l'EEG osserva
        case PHASE.HANDOVER: {
            // Crossfade di autorità: evita lo scatto quando l'input prende il comando.
            const t = Math.min(1.0, AppState.phaseElapsed / CONFIG.PHASE_HANDOVER_S);
            return CONFIG.HOOK_AUTO_VELOCITY * (1 - t) + currentInputVelocity() * t;
        }
        case PHASE.INTERACTIVE:
            return currentInputVelocity();
        default:
            return 0.0;
    }
}

/* ------------------------------------------------------------------ *
 *  Hold / select: dead-zone (a monte) + detent + isteresi + dwell
 * ------------------------------------------------------------------ */

function applyHoldSelect(velocity, dt) {
    const absV = Math.abs(velocity);
    const step = 1 / (TOTAL_IMAGES - 1);
    const nearest = Math.round(AppState.targetFocus / step) * step;

    // Le soglie sono relative alla velocità massima che l'input può produrre:
    // in BCI il mapping percentile satura a ZOOM_GAIN, con la rotella a 1.0.
    const maxInput = (AppState.inputMode === "SIMULATION") ? 1.0 : CONFIG.ZOOM_GAIN;
    const enterHold = CONFIG.ENTER_HOLD_FRAC * maxInput;
    const snapThreshold = CONFIG.SNAP_VEL_FRAC * maxInput;

    // Dwell-to-lock: dopo LOCK_DWELL_S su un livello serve uno sforzo maggiore
    // per uscirne, così l'utente può rilassarsi del tutto senza scivolare.
    // Il clamp garantisce che un detent resti sempre sganciabile: senza, una
    // combinazione sfortunata di gain e moltiplicatore lo renderebbe definitivo.
    const rawBreak = (AppState.lockTimer >= CONFIG.LOCK_DWELL_S)
        ? CONFIG.BREAK_HOLD_FRAC * maxInput * CONFIG.LOCK_DWELL_MULT
        : CONFIG.BREAK_HOLD_FRAC * maxInput;
    const breakThreshold = Math.min(rawBreak, maxInput * 0.95);

    if (AppState.locked) {
        if (absV >= breakThreshold) {
            AppState.locked = false;
            AppState.lockTimer = 0.0;
            return velocity;
        }
        // Agganciato: attrattore verso il livello, nessuna deriva.
        AppState.lockTimer += dt;
        AppState.targetFocus += (AppState.lockedLevel - AppState.targetFocus) * CONFIG.SNAP_STRENGTH;
        return 0.0;
    }

    if (absV < enterHold) {
        AppState.locked = true;
        AppState.lockedLevel = nearest;
        AppState.lockTimer = 0.0;
        return 0.0;
    }

    if (absV < snapThreshold) {
        AppState.targetFocus += (nearest - AppState.targetFocus) * CONFIG.SNAP_STRENGTH;
    }

    return velocity;
}

/* ------------------------------------------------------------------ *
 *  Modale di onboarding
 * ------------------------------------------------------------------ */

function openOnboardingModal() {
    if (!DOM.onboardingModal) return;
    DOM.onboardingModal.classList.remove('hidden');
    if (DOM.onboardingStartBtn) {
        DOM.onboardingStartBtn.disabled = true;
        DOM.onboardingStartBtn.className = "w-full py-3 bg-slate-800 text-slate-500 font-semibold rounded-lg cursor-not-allowed transition-all";
    }
}

function closeOnboardingModal() {
    DOM.onboardingModal?.classList.add('hidden');
}

function updateOnboardingModal() {
    if (!DOM.onboardingStartBtn) return;
    const remaining = CONFIG.MODAL_UNLOCK_S - AppState.phaseElapsed;

    if (remaining > 0) {
        DOM.onboardingStartBtn.innerText = `Attendi... ${Math.ceil(remaining)}s`;
    } else if (DOM.onboardingStartBtn.disabled) {
        DOM.onboardingStartBtn.disabled = false;
        DOM.onboardingStartBtn.innerText = "Inizia";
        DOM.onboardingStartBtn.className = "w-full py-3 bg-blue-600 hover:bg-blue-500 text-white font-semibold rounded-lg transition-all";
    }
}

/* ------------------------------------------------------------------ *
 *  Rendering
 * ------------------------------------------------------------------ */

async function initPixiApp() {
    pixiApp = new PIXI.Application();
    await pixiApp.init({
        view: DOM.pixiCanvas,
        resizeTo: window,
        backgroundColor: 0x0f172a
    });

    // Preload parallelo: la decodifica avviene off-thread, evita lo stutter
    // da decodifica texture al primo crossfade.
    const urls = [];
    for (let i = 1; i <= TOTAL_IMAGES; i++) urls.push(`${IMAGE_PATH}${i}${IMAGE_EXTENSION}`);

    try {
        const textures = await Promise.all(urls.map(u => PIXI.Assets.load(u)));
        for (let i = 0; i < textures.length; i++) {
            const sprite = new PIXI.Sprite(textures[i]);
            sprite.anchor.set(0.5);
            sprite.x = pixiApp.screen.width / 2;
            sprite.y = pixiApp.screen.height / 2;
            sprite.visible = false;
            sprite.renderable = false;
            pixiApp.stage.addChild(sprite);
            spritePool.push(sprite);
        }
    } catch (err) {
        console.error('[ERROR] Impossibile caricare gli asset:', err);
    }

    pixiApp.ticker.add(updateExperienceFrame);
}

async function switchView(view) {
    AppState.currentView = view;
    if (DOM.landingContainer) DOM.landingContainer.style.display = (view === "LANDING") ? 'block' : 'none';
    if (DOM.immersionContainer) DOM.immersionContainer.style.display = (view === "IMMERSION") ? 'block' : 'none';

    if (view === "IMMERSION") {
        if (spritePool.length === 0) await initPixiApp();
        startSession();
    } else {
        AppState.phase = PHASE.IDLE;
        closeOnboardingModal();
    }
}

function startSession() {
    AppState.targetFocus = 0.0;
    AppState.currentFocus = 0.0;
    AppState.targetVelocity = 0.0;
    AppState.eegVelocity = 0.0;
    AppState.sessionElapsed = 0.0;
    AppState.locked = false;
    AppState.lockTimer = 0.0;
    AppState.seeded = false;
    AppState.gatedWindows = 0;
    debugLogCounter = 0;
    calibWindowBuffer.length = 0;
    Normalizer.reset();
    lastRenderedMagnification = -1;

    if (CONFIG.USE_ADAPTIVE_PIPELINE) {
        AppState.phase = PHASE.ONBOARDING;
        AppState.phaseElapsed = 0.0;
        openOnboardingModal();
        console.log('[FSM] -> ONBOARDING (calibrazione sotto la modale)');
    } else {
        // Percorso legacy: nessuna FSM, calibrazione a snapshot da 5s.
        AppState.phase = PHASE.INTERACTIVE;
        AppState.phaseElapsed = 0.0;
        isCalibrated = false;
        calibrationBuffer = [];
    }
}

function updateExperienceFrame() {
    if (spritePool.length === 0) return;

    const dt = pixiApp.ticker.deltaMS / 1000;
    updatePhase(dt);

    // --- Accumulo velocità sul target zoom ---
    if (AppState.phase === PHASE.OUTRO || AppState.phase === PHASE.DONE) {
        // Conclusione scriptata: garanzia della FSM, non una speranza sull'EEG.
        AppState.targetFocus += (CONFIG.OUTRO_TARGET_FOCUS - AppState.targetFocus) * CONFIG.OUTRO_EASING;
    } else if (!CONFIG.USE_ADAPTIVE_PIPELINE) {
        // Percorso legacy invariato.
        if (AppState.inputMode === "BCI") {
            if (isCalibrated) AppState.targetFocus += AppState.targetVelocity * ZOOM_SPEED_FACTOR;
        } else {
            AppState.targetFocus += AppState.targetVelocity * ZOOM_SPEED_FACTOR;
        }
    } else {
        let velocity = resolveAuthorityVelocity();
        if (AppState.phase === PHASE.INTERACTIVE) {
            velocity = applyHoldSelect(velocity, dt);
        }
        AppState.targetFocus += velocity * ZOOM_SPEED_FACTOR;
    }

    // Attrito della rotella (solo simulazione).
    if (AppState.inputMode === "SIMULATION") {
        AppState.targetVelocity *= MOUSE_FRICTION;
        if (Math.abs(AppState.targetVelocity) < 0.001) AppState.targetVelocity = 0.0;
    }

    AppState.targetFocus = Math.max(0.0, Math.min(1.0, AppState.targetFocus));
    AppState.currentFocus += (AppState.targetFocus - AppState.currentFocus) * FOCUS_EASING;

    const rawZoomIndex = AppState.currentFocus * (TOTAL_IMAGES - 1);
    const activeIndex = Math.min(Math.floor(rawZoomIndex), TOTAL_IMAGES - 2);
    const progress = rawZoomIndex - activeIndex;

    for (let i = 0; i < spritePool.length; i++) {
        const sprite = spritePool[i];
        if (i === activeIndex || i === activeIndex + 1) {
            const alpha = (i === activeIndex) ? (1.0 - progress) : progress;
            // Uno sprite ad alpha ~0 non va lasciato renderizzabile: è overdraw puro.
            const visible = alpha > 0.004;
            sprite.visible = visible;
            sprite.renderable = visible;
            sprite.alpha = alpha;
            if (visible) {
                const scale = (i === activeIndex) ? (1.0 + progress * 0.5) : (0.66 + progress * 0.34);
                applyBaseScale(sprite, scale);
            }
        } else {
            sprite.visible = false;
            sprite.renderable = false;
        }
    }

    // --- HUD throttlato ---
    hudTimer += dt;
    if (hudTimer >= 1 / CONFIG.HUD_UPDATE_HZ) {
        hudTimer = 0;
        updateHUD(activeIndex, progress);
    }
}

function updateHUD(activeIndex, progress) {
    const lowerLabel = SCALE_LABELS[activeIndex];
    const upperLabel = SCALE_LABELS[activeIndex + 1];
    const realTimeMagnification = Math.round(lowerLabel + (upperLabel - lowerLabel) * progress);

    if (realTimeMagnification !== lastRenderedMagnification && DOM.immersionMagLabel) {
        DOM.immersionMagLabel.innerText = `MAGNIFICATION: ${realTimeMagnification}x`;
        lastRenderedMagnification = realTimeMagnification;
    }

    const effectiveVelocity = (AppState.inputMode === "SIMULATION")
        ? AppState.targetVelocity
        : AppState.eegVelocity;

    if (DOM.debugRawRatio) DOM.debugRawRatio.innerText = AppState.inputMode === "BCI" ? AppState.rawRatio.toFixed(3) : "SIMULATED";
    if (DOM.debugVelocity) DOM.debugVelocity.innerText = effectiveVelocity.toFixed(3);
    if (DOM.debugTarget) DOM.debugTarget.innerText = AppState.targetFocus.toFixed(3);
    if (DOM.debugCurrent) DOM.debugCurrent.innerText = AppState.currentFocus.toFixed(3);
    if (DOM.debugBarTarget) DOM.debugBarTarget.style.width = `${AppState.targetFocus * 100}%`;
    if (DOM.debugBarCurrent) DOM.debugBarCurrent.style.width = `${AppState.currentFocus * 100}%`;

    if (DOM.debugPhase) {
        DOM.debugPhase.innerText = AppState.locked ? `${AppState.phase} (HOLD)` : AppState.phase;
    }
    if (DOM.debugPercentile) {
        DOM.debugPercentile.innerText = AppState.inputMode === "BCI" ? AppState.percentile.toFixed(2) : "--";
    }
    if (DOM.debugGate) {
        if (!AppState.contactOk) {
            DOM.debugGate.innerText = "CONTACT";
            DOM.debugGate.className = "font-bold text-red-400";
        } else if (AppState.isGated) {
            DOM.debugGate.innerText = "ARTIFACT";
            DOM.debugGate.className = "font-bold text-yellow-400";
        } else {
            DOM.debugGate.innerText = "OK";
            DOM.debugGate.className = "font-bold text-green-400";
        }
    }
}

function updateSynchronizedLEDs(connected, name = "") {
    AppState.isMuseConnected = connected;
    const ledClass = connected ? "led-green" : "led-red";
    if (DOM.homeLed) DOM.homeLed.className = `led-indicator ${ledClass}`;
    if (DOM.immersionStatusLed) DOM.immersionStatusLed.className = `led-indicator ${ledClass}`;
    if (name && DOM.homeDeviceString) DOM.homeDeviceString.innerText = name;
}

function applyBaseScale(sprite, additionalZoom) {
    if (!sprite.texture || !sprite.texture.valid) return;
    const baseScale = Math.max(window.innerWidth / sprite.texture.width, window.innerHeight / sprite.texture.height);
    sprite.scale.set(baseScale * additionalZoom);
}

/* ------------------------------------------------------------------ *
 *  LEGACY - calibrazione a snapshot 5s (CONFIG.USE_ADAPTIVE_PIPELINE = false)
 * ------------------------------------------------------------------ */

function calculateScientificFocusLegacy(fftData) {
    const currentRatio = computePopeIndex(fftData);
    if (currentRatio === null) return;
    AppState.rawRatio = currentRatio;

    if (!isCalibrated) {
        calibrationBuffer.push(currentRatio);
        if (DOM.immersionMagLabel) {
            DOM.immersionMagLabel.innerText = `CALIBRATING BASELINE: ${calibrationBuffer.length}/${CALIBRATION_DURATION}s`;
        }

        if (calibrationBuffer.length >= CALIBRATION_DURATION) {
            bciMean = calibrationBuffer.reduce((sum, v) => sum + v, 0) / CALIBRATION_DURATION;
            const variance = calibrationBuffer.reduce((sum, v) => sum + Math.pow(v - bciMean, 2), 0) / CALIBRATION_DURATION;
            bciStdDev = Math.sqrt(variance) || 0.01;
            isCalibrated = true;
            console.log(`[BCI][legacy] Calibrazione completata. Media: ${bciMean.toFixed(3)}, DevStd: ${bciStdDev.toFixed(3)}`);
        }
        AppState.targetVelocity = 0.0;
        return;
    }

    const zScore = (currentRatio - bciMean) / bciStdDev;
    const sigmoid = 1 / (1 + Math.exp(-zScore));

    bciMean = (1 - LEARNING_RATE) * bciMean + LEARNING_RATE * currentRatio;
    const currentVariance = Math.pow(currentRatio - bciMean, 2);
    bciStdDev = (1 - LEARNING_RATE) * bciStdDev + LEARNING_RATE * Math.sqrt(currentVariance);
    bciStdDev = Math.max(bciStdDev, 0.01);

    const deviation = sigmoid - 0.5;

    if (Math.abs(deviation) <= DEAD_ZONE) {
        AppState.targetVelocity = 0.0;
    } else {
        const sign = Math.sign(deviation);
        const maxPossibleDeviation = 0.5 - DEAD_ZONE;
        AppState.targetVelocity = (deviation - sign * DEAD_ZONE) / maxPossibleDeviation;
    }
}
