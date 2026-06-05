/**
 * ============================================================================
 * MIND ZOOM - SEM Microscope Interactive Experience
 * ============================================================================
 * A single-page web application for interactive visualization of SEM microscope
 * images with seamless zoom through dual-sprite crossfade and real-time focus
 * control via native Bluetooth connection (Muse).
 * 
 * Tech Stack: Vanilla JS (ES6+), PixiJS (Canvas/WebGL), TailwindCSS
 * ============================================================================
 */

const SCALE_LABELS = [10, 50, 200, 500, 1000, 5000, 10000, 25000, 50000];
const TOTAL_IMAGES = 9;
const IMAGE_EXTENSION = '.webp';
const IMAGE_PATH = '/images/';
const FOCUS_EASING = 0.05;
const SHOW_DEBUG_MONITOR = true;

const AppState = {
    currentView: "LANDING",
    targetFocus: 0.0,
    currentFocus: 0.0,
    isMuseConnected: false,
    isSimulationMode: false,
};

const DOM = {
    landingContainer: document.getElementById('landing-container'),
    immersionContainer: document.getElementById('immersion-container'),
    pixiCanvas: document.getElementById('pixi-canvas'),

    beginImmersionBtn: document.getElementById('begin-immersion-btn'),
    establishConnectionBtn: document.getElementById('establish-connection-btn'),
    knowMoreBtn: document.getElementById('know-more-btn'),
    backButton: document.getElementById('back-button'),
    museIndicator: document.getElementById('muse-indicator'),

    magnificationCanvas: document.getElementById('magnification-canvas'),
    magnificationLabel: document.getElementById('magnification-label'),
    waveformCanvas: document.getElementById('waveform-canvas'),

    alertModal: document.getElementById('alert-modal'),
    setupModal: document.getElementById('setup-modal'),
    modalGoBackBtn: document.getElementById('modal-go-back-btn'),
    modalContinueSimulationBtn: document.getElementById('modal-continue-simulation-btn'),
    
    modalConnectBtn: document.getElementById('modal-connect-btn'),
    modalCancelBtn: document.getElementById('modal-cancel-btn'),
    modalSetupCloseBtn: document.getElementById('modal-setup-close-btn'),
    signalAf7Led: document.getElementById('signal-af7-led'),
    signalAf8Led: document.getElementById('signal-af8-led'),
    connectionStatusBadge: document.getElementById('connection-status-badge'),
    eegChartCanvas: document.getElementById('eeg-chart-canvas'),
    
    eegTestingUi: document.getElementById('eeg-testing-ui'),
    telemetryAlphaBar: document.getElementById('telemetry-alpha-bar'),
    telemetryBetaBar: document.getElementById('telemetry-beta-bar'),
    telemetryAlphaVal: document.getElementById('telemetry-alpha-val'),
    telemetryBetaVal: document.getElementById('telemetry-beta-val')
};

let pixiApp = null;
let spriteA = null;
let spriteB = null;
const textureCache = {};
const museHardware = new MuseBluetooth();

let lastSignalCheck = 0;

async function initialize() {
    console.log('🚀 Initializing Mind Zoom Application...');
    DOM.landingContainer.style.backgroundColor = 'transparent';
    DOM.landingContainer.style.backgroundImage = "url('/images/1.webp')";
    DOM.landingContainer.style.backgroundSize = "cover";
    DOM.landingContainer.style.backgroundPosition = "center";

    attachEventListeners();
    setupMuseCallbacks();
    
    await preloadImages();
    await initializePixiJS();
    console.log('✅ Initialization complete');
}

function attachEventListeners() {
    DOM.beginImmersionBtn.addEventListener('click', handleBeginImmersion);
    DOM.knowMoreBtn.addEventListener('click', () => DOM.landingContainer.scrollBy({ top: window.innerHeight * 0.8, behavior: 'smooth' }));
    DOM.backButton.addEventListener('click', switchToLanding);
    
    if (DOM.modalGoBackBtn) DOM.modalGoBackBtn.addEventListener('click', () => DOM.alertModal.classList.add('hidden'));
    if (DOM.modalContinueSimulationBtn) DOM.modalContinueSimulationBtn.addEventListener('click', handleContinueSimulation);

    DOM.establishConnectionBtn.addEventListener('click', () => {
        DOM.setupModal.classList.remove('hidden');
        if (DOM.eegChartCanvas) {
            const rect = DOM.eegChartCanvas.parentElement.getBoundingClientRect();
            DOM.eegChartCanvas.width = rect.width;
            DOM.eegChartCanvas.height = rect.height;
        }
    });

    if (DOM.modalSetupCloseBtn) {
        DOM.modalSetupCloseBtn.addEventListener('click', () => {
            DOM.setupModal.classList.add('hidden');
        });
    }

    if (DOM.modalConnectBtn) {
        DOM.modalConnectBtn.addEventListener('click', initNativeMuse);
    }

    if (DOM.modalCancelBtn) {
        DOM.modalCancelBtn.addEventListener('click', handleCancelConnection);
    }

    window.addEventListener('resize', onWindowResize);
}

function handleBeginImmersion() {
    if (AppState.isMuseConnected || AppState.isSimulationMode) {
        switchToImmersion();
    } else {
        DOM.alertModal.classList.remove('hidden');
    }
}

function handleContinueSimulation() {
    DOM.alertModal.classList.add('hidden');
    AppState.isSimulationMode = true;
    switchToImmersion();
    activateSimulationMode();
}

function setupMuseCallbacks() {
    museHardware.onEEGData((brainData) => {
        if (brainData.focus !== undefined) {
            AppState.targetFocus = Math.max(0, Math.min(255, brainData.focus)) / 255.0;
        }

        if (DOM.telemetryAlphaBar && brainData.alpha !== undefined) {
            DOM.telemetryAlphaBar.style.width = `${brainData.alpha}%`;
            if (DOM.telemetryAlphaVal) DOM.telemetryAlphaVal.textContent = `${Math.round(brainData.alpha)}%`;
        }
        if (DOM.telemetryBetaBar && brainData.beta !== undefined) {
            DOM.telemetryBetaBar.style.width = `${brainData.beta}%`;
            if (DOM.telemetryBetaVal) DOM.telemetryBetaVal.textContent = `${Math.round(brainData.beta)}%`;
        }

        const now = performance.now();
        if (now - lastSignalCheck > 100) {
            lastSignalCheck = now;
            checkSignalQuality();
            if (SHOW_DEBUG_MONITOR && museHardware.fftReal) {
                drawFFT(museHardware.fftReal);
            }
        }
    });

    museHardware.onDisconnect(() => {
        console.log('Muse natively disconnected.');
        AppState.isMuseConnected = false;
        resetModalState();
        activateSimulationMode();
    });
}

function initNativeMuse() {
    if (DOM.connectionStatusBadge) {
        DOM.connectionStatusBadge.textContent = "Pairing with device...";
        DOM.connectionStatusBadge.className = "px-3 py-1 rounded-full text-xs font-bold bg-yellow-900/50 text-yellow-400 border border-yellow-500/30 animate-pulse";
    }

    museHardware.connect().then(() => {
        AppState.isMuseConnected = true;
        
        if (DOM.modalConnectBtn) {
            DOM.modalConnectBtn.disabled = true;
            DOM.modalConnectBtn.classList.replace('bg-blue-600', 'bg-gray-600');
            DOM.modalConnectBtn.classList.remove('hover:bg-blue-500');
            DOM.modalConnectBtn.innerText = "Connesso";
        }
        if (DOM.modalCancelBtn) {
            DOM.modalCancelBtn.disabled = false;
            DOM.modalCancelBtn.classList.remove('opacity-50', 'cursor-not-allowed', 'bg-gray-700', 'text-gray-300');
            DOM.modalCancelBtn.classList.add('bg-red-600', 'hover:bg-red-500', 'text-white');
        }
        if (DOM.connectionStatusBadge) {
            DOM.connectionStatusBadge.textContent = "Connesso";
            DOM.connectionStatusBadge.className = "px-3 py-1 rounded-full text-xs font-bold bg-green-900/50 text-green-400 border border-green-500/30";
        }
        if (DOM.museIndicator) {
            DOM.museIndicator.classList.replace('disconnected', 'connected');
        }
        if (DOM.eegTestingUi) {
            DOM.eegTestingUi.classList.remove('hidden');
        }

        DOM.setupModal.classList.add('hidden');
        switchToImmersion();

    }).catch(err => {
        console.error("Native connection failed:", err);
        resetModalState();
        if (DOM.connectionStatusBadge) {
            DOM.connectionStatusBadge.textContent = "Connessione Fallita";
            DOM.connectionStatusBadge.className = "px-3 py-1 rounded-full text-xs font-bold bg-red-900/50 text-red-400 border border-red-500/30";
        }
    });
}

function handleCancelConnection() {
    museHardware.disconnect();
    resetModalState();
    AppState.targetFocus = 0.0;
    switchToLanding();
}

function resetModalState() {
    if (DOM.modalConnectBtn) {
        DOM.modalConnectBtn.disabled = false;
        DOM.modalConnectBtn.classList.remove('bg-gray-600');
        DOM.modalConnectBtn.classList.add('bg-blue-600', 'hover:bg-blue-500');
        DOM.modalConnectBtn.innerText = "Connetti Fascia";
    }
    if (DOM.modalCancelBtn) {
        DOM.modalCancelBtn.disabled = true;
        DOM.modalCancelBtn.classList.add('opacity-50', 'cursor-not-allowed', 'bg-gray-700', 'text-gray-300');
        DOM.modalCancelBtn.classList.remove('bg-red-600', 'hover:bg-red-500', 'text-white');
    }
    if (DOM.connectionStatusBadge) {
        DOM.connectionStatusBadge.textContent = "Awaiting Connection...";
        DOM.connectionStatusBadge.className = "px-3 py-1 rounded-full text-xs font-bold bg-blue-900/50 text-blue-400 border border-blue-500/30 animate-pulse";
    }
    if (DOM.signalAf7Led) DOM.signalAf7Led.className = "w-4 h-4 rounded-full bg-gray-600 transition-colors duration-300";
    if (DOM.signalAf8Led) DOM.signalAf8Led.className = "w-4 h-4 rounded-full bg-gray-600 transition-colors duration-300";
    
    if (DOM.museIndicator) {
        DOM.museIndicator.classList.replace('connected', 'disconnected');
    }
    if (DOM.eegTestingUi) {
        DOM.eegTestingUi.classList.add('hidden');
    }
}

function checkSignalQuality() {
    let stdDev = 0;
    
    if (museHardware.realBuffer) {
        let sum = 0;
        let sumSq = 0;
        const len = museHardware.realBuffer.length;
        
        for (let i = 0; i < len; i++) {
            const val = museHardware.realBuffer[i];
            sum += val;
            sumSq += val * val;
        }
        
        const mean = sum / len;
        const variance = (sumSq / len) - (mean * mean);
        stdDev = Math.sqrt(variance > 0 ? variance : 0);
    }
    
    let ledClass = 'bg-gray-600';
    if (stdDev === 0) {
        ledClass = 'bg-gray-600';
    } else if (stdDev > 800) {
        ledClass = 'bg-red-500 shadow-[0_0_8px_#ef4444]'; 
    } else if (stdDev >= 10 && stdDev <= 200) {
        ledClass = 'bg-green-500 shadow-[0_0_8px_#10b981]'; 
    } else {
        ledClass = 'bg-yellow-500 shadow-[0_0_8px_#eab308]'; 
    }

    if (DOM.signalAf7Led) DOM.signalAf7Led.className = `w-4 h-4 rounded-full transition-colors duration-300 ${ledClass}`;
    if (DOM.signalAf8Led) DOM.signalAf8Led.className = `w-4 h-4 rounded-full transition-colors duration-300 ${ledClass}`;
}

function drawFFT(fftReal) {
    if (!DOM.eegChartCanvas) return;
    const ctx = DOM.eegChartCanvas.getContext('2d');
    const width = DOM.eegChartCanvas.width;
    const height = DOM.eegChartCanvas.height;
    
    ctx.clearRect(0, 0, width, height);
    
    ctx.strokeStyle = '#1f2937'; 
    ctx.lineWidth = 1;
    ctx.beginPath();
    for(let i = 1; i < 4; i++) {
        ctx.moveTo(0, height * (i / 4)); 
        ctx.lineTo(width, height * (i / 4));
    }
    ctx.stroke();

    const maxBins = 40;
    const barWidth = width / maxBins;
    
    for (let i = 0; i < maxBins; i++) {
        const magnitude = Math.abs(fftReal[i]) || 0; 
        const barHeight = Math.min(height, (magnitude / 256) * height * 5); 
        
        if (i >= 8 && i <= 12) {
            ctx.fillStyle = '#3b82f6'; 
        } else if (i >= 13 && i <= 30) {
            ctx.fillStyle = '#ef4444'; 
        } else {
            ctx.fillStyle = '#4b5563'; 
        }
        
        ctx.fillRect(i * barWidth + 1, height - barHeight, barWidth - 2, barHeight);
    }
}

const SimulationState = { value: 0, velocity: 0, accumulator: 0, lastScrollTime: 0 };

function activateSimulationMode() {
    AppState.isSimulationMode = true;
    DOM.immersionContainer.addEventListener('wheel', handleSimulationWheel, { passive: false });
    if (pixiApp && pixiApp.ticker) {
        pixiApp.ticker.remove(simulationLoop);
        pixiApp.ticker.add(simulationLoop);
    }
}

function handleSimulationWheel(event) {
    if (!AppState.isSimulationMode || AppState.isMuseConnected) return;
    event.preventDefault();
    SimulationState.accumulator += (event.deltaY > 0 ? 1 : -1);
    if (Math.abs(SimulationState.accumulator) >= 3) {
        SimulationState.lastScrollTime = performance.now();
        const direction = SimulationState.accumulator > 0 ? 1 : -1;
        SimulationState.velocity = Math.max(-15, Math.min(15, SimulationState.velocity + (direction * 4.0)));
        SimulationState.accumulator = 0;
    }
}

function simulationLoop() {
    if (!AppState.isSimulationMode || AppState.isMuseConnected) return;
    SimulationState.velocity *= 0.95; 
    
    if (performance.now() - SimulationState.lastScrollTime > 1000) {
        SimulationState.value *= 0.98; 
    } else {
        SimulationState.value += SimulationState.velocity;
    }
    
    SimulationState.value = Math.max(0, Math.min(255, SimulationState.value));
    AppState.targetFocus = SimulationState.value / 255.0;
}

function switchToLanding() {
    AppState.currentView = "LANDING";
    DOM.immersionContainer.style.display = 'none';
    DOM.landingContainer.style.display = 'block';
    DOM.landingContainer.style.backgroundColor = 'transparent';
    document.body.style.backgroundImage = "url('/images/1.webp')";
    DOM.landingContainer.style.backgroundImage = "url('/images/1.webp')";
    DOM.landingContainer.style.backgroundSize = "cover";
    DOM.landingContainer.style.backgroundPosition = "center";
}

function switchToImmersion() {
    AppState.currentView = "IMMERSION";
    DOM.landingContainer.style.display = 'none';
    DOM.immersionContainer.style.display = 'block';
    document.body.style.backgroundImage = 'none';
    document.body.style.backgroundColor = '#000';
    DOM.immersionContainer.style.backgroundColor = '#000';
}

async function preloadImages() {
    const loadPromises = [];
    for (let i = 0; i < TOTAL_IMAGES; i++) {
        const imageNumber = i + 2;
        const imagePath = `${IMAGE_PATH}${imageNumber}${IMAGE_EXTENSION}`;
        loadPromises.push(
            PIXI.Assets.load(imagePath)
                .then(texture => { textureCache[`image-${i}`] = texture; })
                .catch(() => { textureCache[`image-${i}`] = PIXI.Texture.from(generatePlaceholderTexture(i)); })
        );
    }
    await Promise.all(loadPromises);
}

function generatePlaceholderTexture(index) {
    const canvas = document.createElement('canvas');
    canvas.width = 1024; canvas.height = 768;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = `hsl(${(index / TOTAL_IMAGES) * 360}, 60%, 40%)`;
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
    ctx.font = 'bold 64px Arial';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText(`Image ${index + 2}`, canvas.width / 2, canvas.height / 2);
    return canvas.toDataURL('image/png');
}

async function initializePixiJS() {
    pixiApp = new PIXI.Application();
    await pixiApp.init({ canvas: DOM.pixiCanvas, resizeTo: window, backgroundAlpha: 0, antialias: true });
    
    spriteA = new PIXI.Sprite(PIXI.Texture.WHITE); 
    spriteA.anchor.set(0.5); 
    spriteA.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
    
    spriteB = new PIXI.Sprite(PIXI.Texture.WHITE); 
    spriteB.anchor.set(0.5); 
    spriteB.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
    
    pixiApp.stage.addChild(spriteA); 
    pixiApp.stage.addChild(spriteB);
    
    pixiApp.ticker.add(updateFrame);
}

function updateFrame() {
    AppState.currentFocus += (AppState.targetFocus - AppState.currentFocus) * FOCUS_EASING;
    
    const fractionalIndex = AppState.currentFocus * (TOTAL_IMAGES - 1);
    const imageIndex = Math.floor(fractionalIndex);
    const localProgress = fractionalIndex - imageIndex;
    const nextImageIndex = Math.min(imageIndex + 1, TOTAL_IMAGES - 1);
    const currentMagnification = 1 + (AppState.currentFocus * 99);

    const scaleA = Math.pow(2.0, localProgress);
    
    spriteA.scale.set(scaleA); 
    spriteA.alpha = 1 - localProgress;
    
    spriteB.scale.set(scaleA / 2.0); 
    spriteB.alpha = localProgress;

    spriteA.texture = textureCache[`image-${imageIndex}`];
    spriteB.texture = textureCache[`image-${nextImageIndex}`];

    if (DOM.magnificationCanvas && DOM.magnificationLabel) updateMagnificationBar(currentMagnification);
    if (DOM.waveformCanvas) updateWaveformBar();
}

function updateMagnificationBar(magnification) {
    const ctx = DOM.magnificationCanvas.getContext('2d'); 
    const canvas = DOM.magnificationCanvas;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    ctx.strokeStyle = 'rgba(102, 126, 234, 0.6)'; 
    ctx.lineWidth = 2; 
    ctx.beginPath();
    ctx.moveTo(20, canvas.height / 2); 
    ctx.lineTo(canvas.width - 20, canvas.height / 2); 
    ctx.stroke();
    
    const fractionalIndex = AppState.currentFocus * (TOTAL_IMAGES - 1);
    const baseSpacing = 20 + (fractionalIndex - Math.floor(fractionalIndex)) * 40;
    ctx.strokeStyle = 'rgba(102, 126, 234, 0.5)'; 
    ctx.lineWidth = 1;
    
    let x = canvas.width / 2;
    while (x < canvas.width - 20) { 
        ctx.beginPath(); 
        ctx.moveTo(x, canvas.height / 2 - 4); 
        ctx.lineTo(x, canvas.height / 2 + 4); 
        ctx.stroke(); 
        x += baseSpacing; 
    }
    
    x = canvas.width / 2;
    while (x > 20) { 
        ctx.beginPath(); 
        ctx.moveTo(x, canvas.height / 2 - 4); 
        ctx.lineTo(x, canvas.height / 2 + 4); 
        ctx.stroke(); 
        x -= baseSpacing; 
    }
    
    DOM.magnificationLabel.textContent = (magnification >= 10 ? Math.round(magnification) : magnification.toFixed(1)) + 'x';
}

function updateWaveformBar() {
    const ctx = DOM.waveformCanvas.getContext('2d'); 
    const canvas = DOM.waveformCanvas;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    const maxAmplitude = (canvas.height / 2) - 10; 
    const amplitude = AppState.currentFocus * maxAmplitude;
    const basePhase = performance.now() * 0.003;
    
    ctx.shadowBlur = 4; 
    ctx.shadowColor = 'rgba(102, 126, 234, 0.8)';

    const drawWave = (phaseOffset, alpha, lineWidth) => {
        ctx.strokeStyle = `rgba(102, 126, 234, ${alpha})`; 
        ctx.lineWidth = lineWidth; 
        ctx.beginPath();
        for (let x = 0; x < canvas.width; x++) {
            const parabolicMultiplier = 1.0 - Math.pow(2.0 * (x / canvas.width) - 1.0, 2.0);
            const y = (canvas.height / 2) + (amplitude * parabolicMultiplier) * Math.sin(0.05 * x + basePhase + phaseOffset);
            if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.stroke();
    };

    for (let i = 0; i < Math.floor(AppState.currentFocus * 3); i++) {
        drawWave((i + 1) * 2.5 + Math.sin(basePhase * 0.5) * i, 0.2 + (0.1 * AppState.currentFocus), 1);
    }
    drawWave(0, 0.8, 2); 
    ctx.shadowBlur = 0;
}

function onWindowResize() {
    if (!pixiApp || !pixiApp.screen) return;
    if (spriteA) spriteA.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
    if (spriteB) spriteB.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
    
    if (DOM.magnificationCanvas) DOM.magnificationCanvas.width = Math.min(600, window.innerWidth * 0.8);
    if (DOM.waveformCanvas) DOM.waveformCanvas.width = Math.min(600, window.innerWidth * 0.8);
}

document.addEventListener('DOMContentLoaded', () => {
    initialize().catch(err => console.error("Initialization error:", err));
});
