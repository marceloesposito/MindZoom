/**
 * MIND ZOOM - SEM Microscope Interactive Experience
 * Implementazione Input Duale: BCI e Simulazione via Rotella Mouse (Rate Control)
 */

const SCALE_LABELS = [32, 32, 100, 220, 700, 1500, 3000, 6000, 10000, 17000, 25000, 41000];
const TOTAL_IMAGES = 12;
const IMAGE_EXTENSION = '.webp';
const IMAGE_PATH = '/images/';

const FOCUS_EASING = 0.06;

// Parametri di calibrazione BCI (5 secondi)
const CALIBRATION_DURATION = 5;
let calibrationBuffer = [];
let isCalibrated = false;
let bciMean = 1.0;
let bciStdDev = 0.15;
const LEARNING_RATE = 0.02; 

// Parametri geometrici per il controllo di velocità (Rate Control)
const DEAD_ZONE = 0.08;         
const ZOOM_SPEED_FACTOR = 0.003; 

// Costanti specifiche per la simulazione via Mouse Wheel
const MOUSE_ACCELERATION = 0.15; 
const MOUSE_FRICTION = 0.92;     

const AppState = {
    currentView: "LANDING",
    inputMode: "BCI",   
    targetFocus: 0.0,   
    currentFocus: 0.0,  
    isMuseConnected: false,
    rawRatio: 0.0,
    targetVelocity: 0.0 
};

let lastRenderedMagnification = -1;
let pixiApp = null;
let spritePool = [];

// L'oggetto memorizzerà i riferimenti ai nodi una volta che il DOM sarà pronto
const DOM = {};

const muse = new MuseBluetooth();

document.addEventListener("DOMContentLoaded", () => {
    // Inizializzazione sicura dei riferimenti del DOM ad albero caricato
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
    
    DOM.debugInputSource = document.getElementById('debug-input-source');
    DOM.debugVelocity = document.getElementById('debug-velocity');
    DOM.debugRawRatio = document.getElementById('debug-raw-ratio');
    DOM.debugTarget = document.getElementById('debug-target');
    DOM.debugCurrent = document.getElementById('debug-current');
    DOM.debugBarTarget = document.getElementById('debug-bar-target');
    DOM.debugBarCurrent = document.getElementById('debug-bar-current');

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
                if (!isCalibrated && DOM.immersionMagLabel) {
                    DOM.immersionMagLabel.innerText = "WAITING HARDWARE...";
                }
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
        if (brainData.fft && AppState.inputMode === "BCI") {
            calculateScientificFocus(brainData.fft);
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

function calculateScientificFocus(fftData) {
    if (!fftData || !fftData.mags) return AppState.targetFocus;
    
    let totalTheta = 0;
    let totalAlpha = 0;
    let totalBeta = 0;
    const frontalChannels = [1, 2];

    frontalChannels.forEach(channel => {
        const channelMags = fftData.mags[channel];
        if (!channelMags) return;
        
        for (let b = 4;  b <= 8;  b++) totalTheta += channelMags[b] || 0;
        for (let b = 8;  b <= 12; b++) totalAlpha += channelMags[b] || 0;
        for (let b = 13; b <= 30; b++) totalBeta += channelMags[b] || 0;
    });

    if ((totalAlpha + totalTheta) === 0) return AppState.targetFocus;

    let currentRatio = totalBeta / (totalAlpha + totalTheta);
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
            console.log(`[BCI] Calibrazione completata. Media: ${bciMean.toFixed(3)}, DevStd: ${bciStdDev.toFixed(3)}`);
        }
        AppState.targetVelocity = 0.0;
        return AppState.targetFocus;
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

    return AppState.targetFocus;
}

async function initPixiApp() {
    pixiApp = new PIXI.Application();
    await pixiApp.init({
        view: DOM.pixiCanvas,
        resizeTo: window,
        backgroundColor: 0x0f172a
    });

    for (let i = 1; i <= TOTAL_IMAGES; i++) {
        const url = `${IMAGE_PATH}${i}${IMAGE_EXTENSION}`;
        try {
            const texture = await PIXI.Assets.load(url);
            const sprite = new PIXI.Sprite(texture);
            sprite.anchor.set(0.5);
            sprite.x = pixiApp.screen.width / 2;
            sprite.y = pixiApp.screen.height / 2;
            sprite.visible = false;
            pixiApp.stage.addChild(sprite);
            spritePool.push(sprite);
        } catch (err) {
            console.error(`[ERROR] Impossibile caricare asset: ${url}`);
        }
    }
    pixiApp.ticker.add(updateExperienceFrame);
}

async function switchView(view) {
    AppState.currentView = view;
    if (DOM.landingContainer) DOM.landingContainer.style.display = (view === "LANDING") ? 'block' : 'none';
    if (DOM.immersionContainer) DOM.immersionContainer.style.display = (view === "IMMERSION") ? 'block' : 'none';

    if (view === "IMMERSION") {
        if (spritePool.length === 0) await initPixiApp();
        if (AppState.inputMode === "SIMULATION" && DOM.immersionMagLabel) {
            DOM.immersionMagLabel.innerText = "MAGNIFICATION: 32x";
        }
    }
}

function updateExperienceFrame() {
    if (spritePool.length === 0) return;
    
    // Sicurezza: blocca l'esecuzione se le proprietà vitali dell'oggetto non sono definite
    if (typeof AppState.rawRatio === 'undefined' || typeof AppState.targetFocus === 'undefined') {
        return;
    }
    
    // Aggiornamento sicuro della telemetria HUD (controlla che i nodi esistano)
    if (DOM.debugRawRatio) DOM.debugRawRatio.innerText = AppState.inputMode === "BCI" ? AppState.rawRatio.toFixed(3) : "SIMULATED";
    if (DOM.debugVelocity) DOM.debugVelocity.innerText = AppState.targetVelocity.toFixed(3);
    if (DOM.debugTarget) DOM.debugTarget.innerText = AppState.targetFocus.toFixed(3); 
    if (DOM.debugCurrent) DOM.debugCurrent.innerText = AppState.currentFocus.toFixed(3);
    if (DOM.debugBarTarget) DOM.debugBarTarget.style.width = `${AppState.targetFocus * 100}%`;
    if (DOM.debugBarCurrent) DOM.debugBarCurrent.style.width = `${AppState.currentFocus * 100}%`;

    // Calcolo accumulo velocità sul target zoom
    if (AppState.inputMode === "BCI") {
        if (isCalibrated) {
            AppState.targetFocus += AppState.targetVelocity * ZOOM_SPEED_FACTOR;
        }
    } else if (AppState.inputMode === "SIMULATION") {
        AppState.targetFocus += AppState.targetVelocity * ZOOM_SPEED_FACTOR;
        AppState.targetVelocity *= MOUSE_FRICTION;
        if (Math.abs(AppState.targetVelocity) < 0.001) {
            AppState.targetVelocity = 0.0; 
        }
    }

    AppState.targetFocus = Math.max(0.0, Math.min(1.0, AppState.targetFocus));
    AppState.currentFocus += (AppState.targetFocus - AppState.currentFocus) * FOCUS_EASING;
    
    const rawZoomIndex = AppState.currentFocus * (TOTAL_IMAGES - 1);
    const activeIndex = Math.min(Math.floor(rawZoomIndex), TOTAL_IMAGES - 2);
    const progress = rawZoomIndex - activeIndex;

    const lowerLabel = SCALE_LABELS[activeIndex];
    const upperLabel = SCALE_LABELS[activeIndex+1];
    const realTimeMagnification = Math.round(lowerLabel + (upperLabel - lowerLabel) * progress);
    
    if (AppState.inputMode === "SIMULATION" || isCalibrated) {
        if (realTimeMagnification !== lastRenderedMagnification && DOM.immersionMagLabel) {
            DOM.immersionMagLabel.innerText = `MAGNIFICATION: ${realTimeMagnification}x`;
            lastRenderedMagnification = realTimeMagnification;
        }
    }

    for (let i = 0; i < spritePool.length; i++) {
        const sprite = spritePool[i];
        if (i === activeIndex || i === activeIndex + 1) {
            sprite.visible = true;
            sprite.alpha = (i === activeIndex) ? (1.0 - progress) : progress;
            const scale = (i === activeIndex) ? (1.0 + progress * 0.5) : (0.66 + progress * 0.34);
            applyBaseScale(sprite, scale);
        } else {
            sprite.visible = false;
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