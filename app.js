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

// Sotto-stati della scheda di calibrazione attiva, interni a PHASE.ONBOARDING.
const CALIB = {
    INTRO: "INTRO",             // schermata introduttiva, attende il click
    CONCENTRATE: "CONCENTRATE", // l'utente spinge il quadratino in alto (registra absMax)
    RELAX: "RELAX",             // l'utente lo lascia scendere (registra absMin)
    DONE: "DONE",               // calibrazione riuscita, breve conferma poi auto-avanzo
    FAILED: "FAILED"            // segnale assente o modulazione troppo debole -> Riprova
};

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

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
    percentile: 0.5,       // solo diagnostica/legacy
    isGated: false,        // artefatto d'ampiezza sulla finestra corrente
    contactOk: true,       // proxy qualità contatto
    gatedWindows: 0,       // finestre consecutive scartate
    lastEEGAt: 0,          // timestamp dell'ultimo dato EEG ricevuto
    eegStalled: false,     // flusso EEG fermo (vedi checkEEGWatchdog)
    watchdogRetries: 0,

    // --- Indice smussato (denoise EMA, condiviso calibrazione/interazione) ---
    smoothedIndex: 0.0,
    smoothedInit: false,

    // --- Calibrazione attiva ---
    calibStage: CALIB.INTRO,
    calibStageElapsed: 0.0,
    calibRunMin: Infinity,     // min corrente della fase (auto-scala il binario)
    calibRunMax: -Infinity,    // max corrente della fase
    calibPeak: -Infinity,      // picco assoluto della fase CONCENTRATE  -> absMax
    calibTrough: Infinity,     // minimo assoluto della fase RELAX        -> absMin
    calibDisplayPos: 0.0,      // posizione renderizzata del quadratino [0,1]
    calibDisplayTarget: 0.5,   // altezza normalizzata verso cui il quadratino si muove
    calibDoneTimer: 0.0,       // tempo sulla schermata di conferma
    calibValid: false,         // estremi validi -> controllo a estremi attivo

    // --- Estremi della legge di controllo (da calibrazione) ---
    absMax: 0.0,
    absMin: 0.0,
    neutralM: 0.0,
    localMax: 0.0,             // banda locale di isteresi (decade verso il segnale)
    localMin: 0.0,

    // --- Hold / select ---
    locked: false,
    lockedLevel: 0.0,
    lockTimer: 0.0
};

let lastRenderedMagnification = -1;
let pixiApp = null;
let spritePool = [];
let hudTimer = 0.0;
let watchdogTimer = 0.0;

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
    DOM.calibIntro = document.getElementById('calib-intro');
    DOM.calibStartBtn = document.getElementById('calib-start-btn');
    DOM.calibActive = document.getElementById('calib-active');
    DOM.calibTitle = document.getElementById('calib-title');
    DOM.calibPrompt = document.getElementById('calib-prompt');
    DOM.calibRail = document.getElementById('calib-rail');
    DOM.calibTarget = document.getElementById('calib-target');
    DOM.calibSquare = document.getElementById('calib-square');
    DOM.calibCountdown = document.getElementById('calib-countdown');
    DOM.calibContactHint = document.getElementById('calib-contact-hint');
    DOM.calibDone = document.getElementById('calib-done');
    DOM.calibDoneTitle = document.getElementById('calib-done-title');
    DOM.calibDoneMsg = document.getElementById('calib-done-msg');
    DOM.calibRetryBtn = document.getElementById('calib-retry-btn');

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

    if (DOM.calibStartBtn) {
        DOM.calibStartBtn.addEventListener('click', () => {
            if (AppState.phase !== PHASE.ONBOARDING) return;
            if (AppState.calibStage !== CALIB.INTRO) return;
            startCalibration();
        });
    }

    if (DOM.calibRetryBtn) {
        DOM.calibRetryBtn.addEventListener('click', () => {
            if (AppState.phase !== PHASE.ONBOARDING) return;
            if (AppState.calibStage !== CALIB.FAILED) return;
            startCalibration();
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
        // Timbrato sempre, anche in modalità rotella: è il segnale di vita del flusso.
        AppState.lastEEGAt = Date.now();
        if (AppState.eegStalled) {
            console.log('[EEG] flusso ripristinato');
            AppState.eegStalled = false;
        }
        AppState.watchdogRetries = 0;

        if (!brainData.fft || AppState.inputMode !== "BCI") return;
        if (CONFIG.USE_ADAPTIVE_PIPELINE) {
            processAdaptiveIndex(brainData);
        } else {
            calculateScientificFocusLegacy(brainData.fft);
        }
    });

    muse.onDisconnect(() => {
        console.warn('[MUSE] disconnesso');
        updateSynchronizedLEDs(false);
        AppState.eegVelocity = 0.0;
    });

    muse.onReconnect(() => {
        console.log('[MUSE] riconnesso');
        updateSynchronizedLEDs(true, muse.device?.name || '');
        AppState.watchdogRetries = 0;
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

/**
 * Il flusso BLE può fermarsi senza che venga emesso alcun evento di disconnessione:
 * il GATT resta nominalmente connesso ma le notifiche non arrivano più. Senza
 * watchdog l'esperienza si congela in silenzio, che è esattamente ciò che è successo
 * durante i test. Qui lo si segnala, si azzera la velocità (meglio fermo che
 * incollato sull'ultimo valore) e si tenta di far ripartire lo streaming.
 */
function checkEEGWatchdog() {
    if (AppState.inputMode !== "BCI" || !AppState.isMuseConnected) return;
    if (AppState.lastEEGAt === 0) return;   // non è mai partito: niente da diagnosticare

    const silentFor = (Date.now() - AppState.lastEEGAt) / 1000;
    if (silentFor < CONFIG.EEG_WATCHDOG_S) return;

    if (!AppState.eegStalled) {
        AppState.eegStalled = true;
        console.warn(`[EEG] nessun dato da ${silentFor.toFixed(1)}s: flusso fermo. ` +
                     `Zoom congelato in attesa.`);
    }

    // Meglio fermo che alla deriva sull'ultimo valore letto.
    AppState.eegVelocity = 0.0;

    if (CONFIG.EEG_WATCHDOG_RESUME && AppState.watchdogRetries < CONFIG.EEG_WATCHDOG_MAX_RETRY) {
        const attempt = AppState.watchdogRetries + 1;
        AppState.watchdogRetries = attempt;
        console.warn(`[EEG] tentativo di ripresa dello streaming (${attempt}/${CONFIG.EEG_WATCHDOG_MAX_RETRY})`);
        // 'd' è il comando di resume dello streaming nel protocollo Muse.
        muse.sendControlCommand('d').catch(() => {
            console.warn('[EEG] comando di ripresa fallito: probabile disconnessione BLE');
        });
    }
}

/** Rate di aggiornamento del controllo, derivato dall'hop della STFT. */
function controlRateHz() {
    return 256 / (CONFIG.STFT_HOP || 48);
}

let debugLogCounter = 0;
let lastLogAt = 0;
let measuredHz = 0;

/**
 * Log periodico di cosa sta effettivamente leggendo il sensore.
 * Viene chiamato PRIMA delle decisioni di gating, così si vede anche il motivo
 * per cui il controllo eventualmente non si muove.
 */
function logSensorState(quality, index) {
    if (!CONFIG.DEBUG_SENSOR_LOG) return;

    const every = Math.max(1, Math.round(controlRateHz() / CONFIG.DEBUG_LOG_HZ));
    if (++debugLogCounter % every !== 0) return;

    // Rate realmente osservato: se scende sotto il nominale il collo di bottiglia
    // è il flusso BLE, non il DSP.
    const now = Date.now();
    if (lastLogAt !== 0) {
        const dtLog = (now - lastLogAt) / 1000;
        if (dtLog > 0) measuredHz = every / dtLog;
    }
    lastLogAt = now;

    let gate = 'OK';
    if (!quality.contactOk) gate = 'CONTATTO SCARSO';
    else if (quality.artifact) gate = 'ARTEFATTO';

    const idx = (index === null) ? '  n/a' : index.toFixed(3);
    const c = AppState.smoothedInit ? AppState.smoothedIndex.toFixed(3) : ' --';

    // Stato del controllo a estremi (quando calibrato) o della calibrazione in corso.
    let ctrl;
    if (AppState.calibValid) {
        ctrl = `M=${AppState.neutralM.toFixed(3)} lo=${AppState.localMin.toFixed(3)} hi=${AppState.localMax.toFixed(3)}` +
               ` [${AppState.absMin.toFixed(3)}..${AppState.absMax.toFixed(3)}]`;
    } else {
        ctrl = `calib=${AppState.calibStage} peak=${isFinite(AppState.calibPeak) ? AppState.calibPeak.toFixed(3) : '--'}` +
               ` trough=${isFinite(AppState.calibTrough) ? AppState.calibTrough.toFixed(3) : '--'}`;
    }

    console.log(
        `[EEG] ${AppState.phase.padEnd(11)} idx=${idx} c=${c} v=${AppState.eegVelocity >= 0 ? '+' : ''}${AppState.eegVelocity.toFixed(3)}` +
        ` | θ=${Bands.theta.toFixed(2)} α=${Bands.alpha.toFixed(2)} β=${Bands.beta.toFixed(2)}` +
        ` | ${ctrl}` +
        ` | ampiezza=${quality.maxAbsRaw.toFixed(0)}µV gate=${gate}` +
        ` | adc=[${(quality.adcMin || 0).toFixed(0)}..${(quality.adcMax || 0).toFixed(0)}] µ=${(quality.adcMean || 0).toFixed(0)}` +
        ` | ${measuredHz.toFixed(1)}Hz sps=${(quality.sps || 0).toFixed(0)}/256` +
        ` pkt=${quality.packetLen || 0}B/${quality.samplesPerPacket || 0}smp`
    );
}

/* ------------------------------------------------------------------ *
 *  Pipeline adattiva
 * ------------------------------------------------------------------ */

function processAdaptiveIndex(brainData) {
    const quality = brainData.quality || { artifact: false, contactOk: true, maxAbsRaw: 0 };

    // L'indice si calcola sempre: serve anche solo per la diagnostica, così quando
    // il controllo è fermo si vede in console *perché* è fermo.
    const index = computePopeIndex(brainData.fft);
    if (index !== null) AppState.rawRatio = index;

    AppState.contactOk = quality.contactOk;
    AppState.isGated = quality.artifact || !quality.contactOk;
    logSensorState(quality, index);

    // Gating qualità contatto: meglio uno zoom fermo che uno impazzito. Durante la
    // calibrazione questo mette anche in pausa il conteggio (vedi updateOnboardingModal).
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

    // Denoise EMA sull'indice (NON una normalizzazione a finestra): riduce solo il
    // rumore finestra-a-finestra del Pope ratio. È la stessa `c` usata dal quadratino
    // in calibrazione e dalla legge di controllo a estremi in interazione.
    const c = updateSmoothedIndex(index);

    if (AppState.phase === PHASE.ONBOARDING) {
        updateCalibrationSample(c);   // registra estremi + posizione del quadratino
        return;                       // in ONBOARDING lo zoom resta fermo (autorità = 0)
    }

    // Interazione: velocità dallo scostamento della concentrazione rispetto agli
    // estremi (assoluti dalla calibrazione + banda locale di isteresi).
    AppState.eegVelocity = computeExtremaVelocity(c, 1 / controlRateHz());
}

/** EMA di denoise sull'indice Pope; inizializza al primo campione (niente transitorio). */
function updateSmoothedIndex(index) {
    if (!AppState.smoothedInit) {
        AppState.smoothedIndex = index;
        AppState.smoothedInit = true;
    } else {
        AppState.smoothedIndex += (index - AppState.smoothedIndex) * CONFIG.CALIB_INDEX_EMA;
    }
    return AppState.smoothedIndex;
}

/* ------------------------------------------------------------------ *
 *  Legge di controllo a estremi (post-calibrazione, niente moving-average)
 * ------------------------------------------------------------------ */

/**
 * Velocità di zoom dalla concentrazione `c` relativa agli estremi.
 * - Neutro M = media dei due estremi assoluti: sopra = concentrazione (zoom in),
 *   sotto = distrazione (zoom out).
 * - Una banda LOCALE di isteresi (localMin, localMax) che segue lentamente il segnale
 *   rende il controllo fasico: la velocità va a 0 appena `c` scende sotto il massimo
 *   locale (ferma lo zoom in) e diventa negativa sotto il minimo locale (zoom out).
 * - Gli estremi ASSOLUTI (dalla calibrazione) delimitano la banda e definiscono la
 *   saturazione: piena velocità al `CALIB_CONC_FRACTION` del tragitto M->estremo.
 */
function computeExtremaVelocity(c, dt) {
    if (!AppState.calibValid) return 0.0;

    const absMax = AppState.absMax, absMin = AppState.absMin, M = AppState.neutralM;
    const span = absMax - absMin;
    if (span <= 0) return 0.0;

    // Gli estremi locali decadono verso il segnale a LOCAL_DECAY frazioni di span/s.
    const stepv = CONFIG.LOCAL_DECAY * dt * span;
    AppState.localMax = Math.min(absMax, Math.max(c, AppState.localMax - stepv));
    AppState.localMin = Math.max(absMin, Math.min(c, AppState.localMin + stepv));

    const gain = CONFIG.EXTREMA_GAIN;
    const frac = CONFIG.CALIB_CONC_FRACTION;

    if (c >= AppState.localMax) {
        const denom = frac * (absMax - M);
        return (denom > 0) ? gain * clamp01((c - M) / denom) : 0.0;      // zoom in
    }
    if (c <= AppState.localMin) {
        const denom = frac * (M - absMin);
        return (denom > 0) ? -gain * clamp01((M - c) / denom) : 0.0;     // zoom out
    }
    return 0.0;                                                          // hold
}

/* ------------------------------------------------------------------ *
 *  Registrazione dei campioni durante la calibrazione attiva
 * ------------------------------------------------------------------ */

/**
 * Aggiorna gli estremi (run min/max per il display, picco/minimo per gli assoluti)
 * e l'altezza-target del quadratino. Chiamata sia dal flusso EEG (BCI) sia dal
 * percorso rotella (SIMULATION). Lo scarto iniziale di CALIB_LEADIN_S evita di
 * registrare il transitorio di reazione al prompt.
 */
function updateCalibrationSample(c) {
    const stage = AppState.calibStage;
    if (stage !== CALIB.CONCENTRATE && stage !== CALIB.RELAX) return;

    // Il display si auto-scala sul range visto nella fase, così il binario resta
    // leggibile anche prima di conoscere gli estremi assoluti.
    if (c < AppState.calibRunMin) AppState.calibRunMin = c;
    if (c > AppState.calibRunMax) AppState.calibRunMax = c;
    const lo = AppState.calibRunMin, hi = AppState.calibRunMax;
    AppState.calibDisplayTarget = (hi > lo) ? clamp01((c - lo) / (hi - lo)) : 0.5;

    // Gli estremi ASSOLUTI si registrano solo dopo il lead-in.
    if (AppState.calibStageElapsed < CONFIG.CALIB_LEADIN_S) return;
    if (stage === CALIB.CONCENTRATE) {
        if (c > AppState.calibPeak) AppState.calibPeak = c;
    } else {
        if (c < AppState.calibTrough) AppState.calibTrough = c;
    }
}

/* ------------------------------------------------------------------ *
 *  Macchina a stati a fasi
 * ------------------------------------------------------------------ */

/** Fase che segue l'onboarding: dritti all'interazione salvo hook riabilitato. */
function phaseAfterOnboarding() {
    return CONFIG.ENABLE_HOOK_PHASE ? PHASE.HOOK : PHASE.INTERACTIVE;
}

function enterPhase(phase) {
    // La calibrazione attiva finalizza da sé (finalizeActiveCalibration); qui non
    // c'è più la semina passiva agganciata all'uscita da ONBOARDING.
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
            // La scheda di calibrazione guida l'avanzamento (concentrate/relax/done).
            updateOnboardingModal(dt);
            break;
        case PHASE.HOOK:
            if (AppState.phaseElapsed >= CONFIG.PHASE_HOOK_S) enterPhase(PHASE.HANDOVER);
            break;

        case PHASE.HANDOVER:
            if (AppState.phaseElapsed >= CONFIG.PHASE_HANDOVER_S) enterPhase(PHASE.INTERACTIVE);
            break;
        case PHASE.INTERACTIVE:
            // Senza limite di tempo l'interazione non scade: l'uscita è dell'utente.
            if (CONFIG.ENABLE_TIME_LIMIT && AppState.phaseElapsed >= CONFIG.PHASE_INTERACTIVE_S) {
                enterPhase(PHASE.OUTRO);
            }
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
    // in BCI il controllo a estremi satura a EXTREMA_GAIN, con la rotella a 1.0.
    const maxInput = (AppState.inputMode === "SIMULATION") ? 1.0 : CONFIG.EXTREMA_GAIN;
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
 *  Scheda di calibrazione attiva (modale)
 * ------------------------------------------------------------------ */

const CALIB_RAIL_H = 300;   // deve combaciare con .calib-rail height in index.html
const CALIB_SQUARE_H = 40;  // .calib-square

/** Mostra uno dei tre stage della modale (intro / active / done). */
function showCalibStage(which) {
    DOM.calibIntro?.classList.toggle('hidden', which !== 'intro');
    DOM.calibActive?.classList.toggle('hidden', which !== 'active');
    DOM.calibDone?.classList.toggle('hidden', which !== 'done');
}

function openOnboardingModal() {
    if (!DOM.onboardingModal) return;
    DOM.onboardingModal.classList.remove('hidden');
    AppState.calibStage = CALIB.INTRO;
    showCalibStage('intro');
}

function closeOnboardingModal() {
    DOM.onboardingModal?.classList.add('hidden');
}

/** INTRO -> CONCENTRATE: azzera gli accumulatori e avvia la prima fase. */
function startCalibration() {
    AppState.calibPeak = -Infinity;
    AppState.calibTrough = Infinity;
    AppState.smoothedInit = false;
    AppState.calibValid = false;
    AppState.calibDisplayPos = 0.0;
    AppState.calibDisplayTarget = 0.5;
    showCalibStage('active');
    enterCalibStage(CALIB.CONCENTRATE);
    console.log('[CALIB] avvio: fase CONCENTRATE');
}

/** Entra in una fase attiva (CONCENTRATE/RELAX): reset run min/max e prompt/obiettivo. */
function enterCalibStage(stage) {
    AppState.calibStage = stage;
    AppState.calibStageElapsed = 0.0;
    AppState.calibRunMin = Infinity;
    AppState.calibRunMax = -Infinity;

    const concentrate = (stage === CALIB.CONCENTRATE);
    if (DOM.calibTitle) DOM.calibTitle.innerText = concentrate ? "Concentrazione" : "Rilassamento";
    if (DOM.calibPrompt) {
        DOM.calibPrompt.innerText = concentrate
            ? "Concentrati per spingere il quadratino verso l'obiettivo in alto."
            : "Ora rilassati e lascia scendere il quadratino verso il basso.";
    }
    // Zona-obiettivo in alto per la concentrazione, in basso per il rilassamento.
    if (DOM.calibTarget) DOM.calibTarget.style.top = concentrate ? "8px" : `${CALIB_RAIL_H - 44 - 8}px`;
}

function updateOnboardingModal(dt) {
    switch (AppState.calibStage) {
        case CALIB.INTRO:
        case CALIB.FAILED:
            return;   // attende un click (Inizia / Riprova)

        case CALIB.DONE:
            // Breve conferma, poi auto-avanzo all'interazione.
            AppState.calibDoneTimer += dt;
            if (AppState.calibDoneTimer >= CONFIG.CALIB_DONE_HOLD_S) {
                closeOnboardingModal();
                enterPhase(phaseAfterOnboarding());
            }
            return;

        case CALIB.CONCENTRATE:
        case CALIB.RELAX:
            updateActiveCalibStage(dt);
            return;
    }
}

function updateActiveCalibStage(dt) {
    const stage = AppState.calibStage;
    const contactOk = (AppState.inputMode === "SIMULATION") ? true : AppState.contactOk;

    // In SIMULATION la rotella pilota un indice sintetico, per testare a secco.
    if (AppState.inputMode === "SIMULATION") {
        const c = updateSmoothedIndex((AppState.smoothedInit ? AppState.smoothedIndex : 0) +
                                      AppState.targetVelocity * 0.05);
        updateCalibrationSample(c);
    }

    // Contatto scarso: si mette in pausa il conteggio e si segnala.
    if (DOM.calibContactHint) DOM.calibContactHint.classList.toggle('hidden', contactOk);
    DOM.calibSquare?.classList.toggle('paused', !contactOk);
    if (contactOk) AppState.calibStageElapsed += dt;

    // Posizione del quadratino: EMA a 60 Hz verso l'altezza-target normalizzata.
    AppState.calibDisplayPos += (AppState.calibDisplayTarget - AppState.calibDisplayPos) * CONFIG.CALIB_DISPLAY_EMA;
    const pos = clamp01(AppState.calibDisplayPos);
    if (DOM.calibSquare) {
        DOM.calibSquare.style.bottom = `${pos * (CALIB_RAIL_H - CALIB_SQUARE_H)}px`;
        const reached = (stage === CALIB.CONCENTRATE) ? (pos > 0.8) : (pos < 0.2);
        DOM.calibSquare.classList.toggle('reached', reached && contactOk);
    }

    // Countdown della fase.
    const dur = (stage === CALIB.CONCENTRATE) ? CONFIG.CALIB_CONCENTRATE_S : CONFIG.CALIB_RELAX_S;
    if (DOM.calibCountdown) DOM.calibCountdown.innerText = Math.ceil(Math.max(0, dur - AppState.calibStageElapsed));

    if (AppState.calibStageElapsed >= dur) {
        if (stage === CALIB.CONCENTRATE) {
            console.log(`[CALIB] CONCENTRATE fine: picco=${isFinite(AppState.calibPeak) ? AppState.calibPeak.toFixed(3) : '--'}`);
            enterCalibStage(CALIB.RELAX);
        } else {
            finalizeActiveCalibration();
        }
    }
}

/** Calcola gli estremi assoluti e valida la calibrazione. */
function finalizeActiveCalibration() {
    const peak = AppState.calibPeak, trough = AppState.calibTrough;

    if (!isFinite(peak) || !isFinite(trough)) {
        return failCalibration("Segnale assente durante la calibrazione.");
    }
    const M = (peak + trough) / 2;
    const span = peak - trough;
    const minSpan = CONFIG.CALIB_MIN_SPAN_REL * Math.max(1e-6, Math.abs(M));
    if (span < minSpan) {
        return failCalibration("Modulazione troppo debole: prova a marcare di più la differenza fra concentrazione e rilassamento.");
    }

    AppState.absMax = peak;
    AppState.absMin = trough;
    AppState.neutralM = M;
    AppState.localMax = M;   // la banda locale parte dal neutro
    AppState.localMin = M;
    AppState.calibValid = true;
    AppState.calibStage = CALIB.DONE;
    AppState.calibDoneTimer = 0.0;

    console.log(`[CALIB] OK: absMin=${trough.toFixed(3)} M=${M.toFixed(3)} absMax=${peak.toFixed(3)} span=${span.toFixed(3)}`);
    showCalibStage('done');
    if (DOM.calibDoneTitle) DOM.calibDoneTitle.innerText = "Calibrazione completata";
    if (DOM.calibDoneMsg) DOM.calibDoneMsg.innerText = "Concentrandoti aumenterai lo zoom, rilassandoti tornerai indietro. Buona esplorazione.";
    DOM.calibRetryBtn?.classList.add('hidden');
}

function failCalibration(reason) {
    AppState.calibValid = false;
    AppState.calibStage = CALIB.FAILED;
    console.warn(`[CALIB] fallita: ${reason}`);
    showCalibStage('done');
    if (DOM.calibDoneTitle) DOM.calibDoneTitle.innerText = "Calibrazione non riuscita";
    if (DOM.calibDoneMsg) DOM.calibDoneMsg.innerText = reason;
    DOM.calibRetryBtn?.classList.remove('hidden');
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

    refreshBaseScales();
    window.addEventListener('resize', refreshBaseScales);

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
    AppState.gatedWindows = 0;

    // Reset del controllo a estremi e della calibrazione.
    AppState.smoothedInit = false;
    AppState.smoothedIndex = 0.0;
    AppState.calibValid = false;
    AppState.calibPeak = -Infinity;
    AppState.calibTrough = Infinity;
    AppState.absMax = AppState.absMin = AppState.neutralM = 0.0;
    AppState.localMax = AppState.localMin = 0.0;

    debugLogCounter = 0;
    Normalizer.reset();
    lastRenderedMagnification = -1;

    if (CONFIG.USE_ADAPTIVE_PIPELINE) {
        AppState.phase = PHASE.ONBOARDING;
        AppState.phaseElapsed = 0.0;
        openOnboardingModal();
        console.log('[FSM] -> ONBOARDING (scheda di calibrazione attiva)');
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

    // Il watchdog va valutato a bassa frequenza: a 60 Hz esaurirebbe i tentativi
    // di ripresa in tre frame.
    watchdogTimer += dt;
    if (watchdogTimer >= 1.0) {
        watchdogTimer = 0;
        checkEEGWatchdog();
    }

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
        // Riusa la riga "Percentile" per lo stato del controllo a estremi: c vs neutro M.
        if (AppState.inputMode !== "BCI") {
            DOM.debugPercentile.innerText = "--";
        } else if (AppState.calibValid) {
            DOM.debugPercentile.innerText = `${AppState.smoothedIndex.toFixed(2)} / ${AppState.neutralM.toFixed(2)}`;
        } else {
            DOM.debugPercentile.innerText = "calibrazione…";
        }
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

/**
 * Lo scale di base dipende solo da viewport e texture: ricalcolarlo ad ogni frame
 * costringeva a leggere window.innerWidth/innerHeight due volte per frame (layout
 * sincrono, ~120 letture/s). Si calcola una volta e si ricalcola solo al resize.
 */
function refreshBaseScales() {
    const w = window.innerWidth;
    const h = window.innerHeight;
    for (let i = 0; i < spritePool.length; i++) {
        const sprite = spritePool[i];
        if (!sprite.texture || !sprite.texture.width) continue;
        sprite._baseScale = Math.max(w / sprite.texture.width, h / sprite.texture.height);
        sprite.x = w / 2;
        sprite.y = h / 2;
    }
}

function applyBaseScale(sprite, additionalZoom) {
    if (!sprite._baseScale) return;
    sprite.scale.set(sprite._baseScale * additionalZoom);
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
