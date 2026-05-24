/**
 * ============================================================================
 * MIND ZOOM - SEM Microscope Interactive Experience
 * ============================================================================
 * A single-page web application for interactive visualization of SEM microscope
 * images with seamless zoom through dual-sprite crossfade and real-time focus
 * control via WebSocket connection.
 * 
 * Tech Stack: Vanilla JS (ES6+), PixiJS (Canvas/WebGL), TailwindCSS
 * ============================================================================
 */

// ============================================================================
// CONFIGURATION & CONSTANTS
// ============================================================================

/**
 * Array of magnification multipliers corresponding to each image.
 * Index 0 = 2.webp at 10x, Index 8 = 10.webp at 50000x
 * These represent the actual SEM magnification values.
 */
const SCALE_LABELS = [10, 50, 200, 500, 1000, 5000, 10000, 25000, 50000];

/**
 * Total number of images available (2.webp through 10.webp)
 */
const TOTAL_IMAGES = 9;

/**
 * Image file extension
 */
const IMAGE_EXTENSION = '.webp';

/**
 * Path to images folder
 */
const IMAGE_PATH = '/images/';

/**
 * WebSocket server URL for receiving focus data
 */
const WS_SERVER_URL = 'wss://il-tuo-bridge.onrender.com';

/**
 * Focus interpolation easing factor (0.0 to 1.0)
 * Lower values = smoother, slower transitions
 * 0.05 means the current focus will move 5% toward the target each frame
 */
const FOCUS_EASING = 0.05;

// ============================================================================
// APPLICATION STATE
// ============================================================================

const AppState = {
    // View states: "LANDING" or "IMMERSION"
    currentView: "LANDING",

    // Focus level (0.0 to 1.0)
    // 0.0 = fully zoomed out (image 1)
    // 1.0 = fully zoomed in (image 10)
    targetFocus: 0.0,
    currentFocus: 0.0,

    // Connection states
    isMuseConnected: false,
    isMindMonitorConnected: false,

    // Simulation mode flag (activated when entering immersion without connection)
    isSimulationMode: false,

    // WebSocket connection reference
    ws: null,

    reconnectAttempts: 0,
};

// ============================================================================
// INTERFACCIA SENSORE (0 - 255)
// ============================================================================

/**
 * Aggiorna il livello di zoom in base a un input esterno.
 * Questa è l'unica interfaccia che l'app usa per muovere lo zoom.
 * In futuro basterà passare a questa funzione la lettura di un sensore reale.
 * 
 * @param {number} rawValue - Un valore normalizzato compreso tra 0 e 255.
 */
function updateFocusFromSensor(rawValue) {
    // Assicura che il valore resti nei limiti hardware previsti (0-255)
    const clampedValue = Math.max(0, Math.min(255, rawValue));
    
    // Normalizza il valore nel range (0.0 - 1.0) richiesto dal motore di rendering
    AppState.targetFocus = clampedValue / 255.0;
}

// Esposto globalmente per facilitare test o l'integrazione di script esterni
window.setSensorValue = updateFocusFromSensor;

// ============================================================================
// DOM ELEMENT REFERENCES
// ============================================================================

const DOM = {
    // Landing view
    landingContainer: document.getElementById('landing-container'),
    
    // Immersion view
    immersionContainer: document.getElementById('immersion-container'),
    pixiCanvas: document.getElementById('pixi-canvas'),

    // Buttons
    beginImmersionBtn: document.getElementById('begin-immersion-btn'),
    establishConnectionBtn: document.getElementById('establish-connection-btn'),
    knowMoreBtn: document.getElementById('know-more-btn'),
    backButton: document.getElementById('back-button'),

    // Status indicators
    museIndicator: document.getElementById('muse-indicator'),
    mindmonitorIndicator: document.getElementById('mindmonitor-indicator'),

    // UI Elements in Immersion
    magnificationCanvas: document.getElementById('magnification-canvas'),
    magnificationLabel: document.getElementById('magnification-label'),
    waveformCanvas: document.getElementById('waveform-canvas'),

    // Modals
    alertModal: document.getElementById('alert-modal'),
    setupModal: document.getElementById('setup-modal'),
    modalGoBackBtn: document.getElementById('modal-go-back-btn'),
    modalContinueSimulationBtn: document.getElementById('modal-continue-simulation-btn'),
    modalSetupCloseBtn: document.getElementById('modal-setup-close-btn'),
};

// ============================================================================
// PIXI.JS SETUP & GLOBALS
// ============================================================================

let pixiApp = null;
let spriteA = null;
let spriteB = null;

/**
 * Preloaded texture assets for all 10 images
 * Key: imageIndex (0-9), Value: PIXI.Texture
 */
const textureCache = {};

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * Main initialization function - called when DOM is ready
 */
async function initialize() {
    console.log('🚀 Initializing Mind Zoom Application...');

    // Apply background image fix directly to container on load
    DOM.landingContainer.style.backgroundColor = 'transparent';
    DOM.landingContainer.style.backgroundImage = "url('/images/1.webp')";
    DOM.landingContainer.style.backgroundSize = "cover";
    DOM.landingContainer.style.backgroundPosition = "center";

    // Attach event listeners
    attachEventListeners();

    // Update connection status indicators
    updateConnectionIndicators();

    // Initialize WebSocket connection
    initializeWebSocket();

    // Preload all images
    await preloadImages();

    // Initialize PixiJS app (will be activated when entering IMMERSION)
    await initializePixiJS();

    console.log('✅ Initialization complete');
}

// ============================================================================
// EVENT LISTENERS
// ============================================================================

function attachEventListeners() {
    // Landing view CTA buttons
    DOM.beginImmersionBtn.addEventListener('click', handleBeginImmersion);
    DOM.establishConnectionBtn.addEventListener('click', handleEstablishConnection);
    DOM.knowMoreBtn.addEventListener('click', handleKnowMore);

    // Immersion view back button
    DOM.backButton.addEventListener('click', handleBackButton);

    // Modal buttons
    DOM.modalGoBackBtn.addEventListener('click', closeAlertModal);
    DOM.modalContinueSimulationBtn.addEventListener('click', handleContinueSimulation);
    DOM.modalSetupCloseBtn.addEventListener('click', closeSetupModal);

    // Window resize to adjust PixiJS canvas
    window.addEventListener('resize', onWindowResize);

    // Disable establish connection button if both are connected
    updateEstablishConnectionButtonState();
}

/**
 * Handle "Begin the immersion" button click
 * If connected: switch to IMMERSION view
 * If NOT connected: show alert modal
 */
function handleBeginImmersion() {
    console.log('📍 Begin Immersion clicked');
    console.log(`   Muse connected: ${AppState.isMuseConnected}, Mind Monitor connected: ${AppState.isMindMonitorConnected}`);

    // Check if at least one device is connected
    const isConnected = AppState.isMuseConnected || AppState.isMindMonitorConnected;

    if (isConnected) {
        // Both devices are connected, enter immersion
        switchToImmersion();
    } else {
        // No connection, show alert modal
        showAlertModal();
    }
}

/**
 * Handle "Establish connection" button click
 * Opens the setup modal with device connection guide
 */
function handleEstablishConnection() {
    console.log('🔌 Establish Connection clicked');
    showSetupModal();
}

/**
 * Handle "Know more" button click
 * Smoothly scroll landing page down by 80vh
 */
function handleKnowMore() {
    console.log('ℹ️  Know More clicked - scrolling down');
    DOM.landingContainer.scrollBy({
        top: window.innerHeight * 0.8,
        behavior: 'smooth',
    });
}

/**
 * Handle back button in immersion view
 * Return to landing page
 */
function handleBackButton() {
    console.log('↩️  Back button clicked');
    switchToLanding();
}

/**
 * Handle "Prosegui con simulazione" (Continue with simulation)
 * Activate simulation mode and enter immersion
 */
function handleContinueSimulation() {
    console.log('🎮 Activating simulation mode');
    AppState.isSimulationMode = true;
    closeAlertModal();
    switchToImmersion();

    // Activate mouse wheel listener for focus simulation
    activateSimulationMode();
}

// ============================================================================
// MODULO SIMULAZIONE (Scollegabile)
// ============================================================================

// Stato incapsulato per non sporcare il resto dell'app
const SimulationState = {
    value: 0,          // Valore simulato del sensore (0-255)
    velocity: 0,       // Inerzia dello scroll
    accumulator: 0,    // Accumulatore scatti rotellina
    lastScrollTime: 0  // Timestamp ultimo scroll
};

/**
 * Attiva la simulazione tramite mouse.
 * Per disabilitarla quando avrai il sensore reale, basterà NON chiamare 
 * questa funzione (o commentarne la chiamata in handleContinueSimulation).
 */
function activateSimulationMode() {
    console.log('🖱️ Modulo simulazione sensore (mouse wheel) attivato');

    // 1. Ascolto della rotellina del mouse
    DOM.immersionContainer.addEventListener('wheel', handleSimulationWheel, { passive: false });

    // 2. Aggiunge il loop di simulazione indipendente al motore di rendering
    if (pixiApp && pixiApp.ticker) {
        pixiApp.ticker.add(simulationLoop);
    }
}

function handleSimulationWheel(event) {
    event.preventDefault();
    
    SimulationState.accumulator += (event.deltaY > 0 ? 1 : -1);

    // Applica forza ogni 3 scatti della rotellina (dampening)
    if (Math.abs(SimulationState.accumulator) >= 3) {
        SimulationState.lastScrollTime = performance.now();
        
        const direction = SimulationState.accumulator > 0 ? 1 : -1;
        const force = direction * 4.0; // Incremento inerziale (scala 0-255)

        SimulationState.velocity = Math.max(-15, Math.min(15, SimulationState.velocity + force));
        SimulationState.accumulator = 0;
    }
}

function simulationLoop() {
    // Decadimento naturale della velocità (inerzia)
    SimulationState.velocity *= 0.95;

    // Decadimento target per inattività (>1 secondo senza input)
    const timeSinceLastScroll = performance.now() - SimulationState.lastScrollTime;
    if (timeSinceLastScroll > 1000) {
        SimulationState.value *= 0.98; // Collasso rapido verso lo 0
    } else {
        SimulationState.value += SimulationState.velocity;
    }

    // Limita il valore strettamente al range del sensore simulato (0 - 255)
    SimulationState.value = Math.max(0, Math.min(255, SimulationState.value));

    // INVIA IL VALORE ALL'INTERFACCIA HARDWARE/LOGICA
    updateFocusFromSensor(SimulationState.value);
}

// ============================================================================
// VIEW SWITCHING
// ============================================================================

/**
 * Switch to LANDING view
 */
function switchToLanding() {
    console.log('🏠 Switching to LANDING view');
    AppState.currentView = "LANDING";
    AppState.isSimulationMode = false;

    // Hide immersion container
    DOM.immersionContainer.style.display = 'none';

    // Show landing container
    DOM.landingContainer.style.display = 'block';
    
    // Restore background image for landing view
    document.body.style.backgroundImage = "url('/images/1.webp')";
    DOM.landingContainer.style.backgroundColor = 'transparent';
    DOM.landingContainer.style.backgroundImage = "url('/images/1.webp')";
    DOM.landingContainer.style.backgroundSize = "cover";
    DOM.landingContainer.style.backgroundPosition = "center";

    // ========================================================================
    // Note: In PixiJS v8, the ticker runs automatically.
    // Pausing is handled by toggling ticker.speed if needed, but for this app,
    // we keep the ticker running for smooth transitions. The canvas is simply
    // hidden from view, so it won't affect visual performance.
    // ========================================================================
}

/**
 * Switch to IMMERSION view
 */
function switchToImmersion() {
    console.log('🔬 Switching to IMMERSION view');
    AppState.currentView = "IMMERSION";

    // Hide landing container
    DOM.landingContainer.style.display = 'none';

    // Show immersion container and clear background image
    DOM.immersionContainer.style.display = 'block';
    document.body.style.backgroundImage = 'none';
    document.body.style.backgroundColor = '#000';
    DOM.immersionContainer.style.backgroundColor = '#000';

    // ========================================================================
    // Note: In PixiJS v8, the ticker runs automatically upon initialization.
    // No need to call .start() - the animation loop is already running.
    // ========================================================================
}

// ============================================================================
// MODAL MANAGEMENT
// ============================================================================

function showAlertModal() {
    console.log('⚠️  Showing alert modal');
    DOM.alertModal.classList.remove('hidden');
}

function closeAlertModal() {
    console.log('✅ Closing alert modal');
    DOM.alertModal.classList.add('hidden');
}

function showSetupModal() {
    console.log('📋 Showing setup modal');
    DOM.setupModal.classList.remove('hidden');
}

function closeSetupModal() {
    console.log('✅ Closing setup modal');
    DOM.setupModal.classList.add('hidden');
}

// ============================================================================
// CONNECTION & STATUS MANAGEMENT
// ============================================================================

/**
 * Initialize WebSocket connection to the bridge server
 * Listens for focus data: { "focus": value }
 */
function initializeWebSocket() {
    console.log(`🔗 Initializing WebSocket connection to ${WS_SERVER_URL}`);

    try {
        AppState.ws = new WebSocket(WS_SERVER_URL);

        // Connection opened
        AppState.ws.onopen = () => {
            console.log('✅ WebSocket connected');
            AppState.reconnectAttempts = 0;
            // Device connections will be confirmed via messages
        };

        // Message received
        AppState.ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);

                // Handle focus data
                if (typeof data.focus !== 'undefined') {
                    AppState.targetFocus = Math.max(0.0, Math.min(1.0, data.focus));
                    console.log(`📊 Focus updated via WebSocket: ${AppState.targetFocus.toFixed(3)}`);
                }

                // Handle device connection status
                if (typeof data.muse !== 'undefined') {
                    AppState.isMuseConnected = data.muse;
                    console.log(`🧠 Muse connection: ${AppState.isMuseConnected}`);
                    updateConnectionIndicators();
                    updateEstablishConnectionButtonState();
                }

                if (typeof data.mindmonitor !== 'undefined') {
                    AppState.isMindMonitorConnected = data.mindmonitor;
                    console.log(`📱 Mind Monitor connection: ${AppState.isMindMonitorConnected}`);
                    updateConnectionIndicators();
                    updateEstablishConnectionButtonState();
                }
            } catch (err) {
                console.warn('Error parsing WebSocket message:', err);
            }
        };

        // Connection closed
        AppState.ws.onclose = () => {
            console.log('❌ WebSocket disconnected');
            AppState.isMuseConnected = false;
            AppState.isMindMonitorConnected = false;
            updateConnectionIndicators();
            updateEstablishConnectionButtonState();

            if (AppState.reconnectAttempts < 3) {
                AppState.reconnectAttempts++;
                console.log(`🔄 Attempting to reconnect... (${AppState.reconnectAttempts}/3)`);
                setTimeout(() => {
                    initializeWebSocket();
                }, 3000);
            } else {
                console.warn('⚠️ Max WebSocket reconnection attempts reached. Staying in simulation mode.');
                AppState.isSimulationMode = true;
            }
        };

        // Connection error
        AppState.ws.onerror = (error) => {
            console.error('❌ WebSocket error:', error);
        };
    } catch (err) {
        console.error('Failed to initialize WebSocket:', err);
    }
}

/**
 * Update the visual status indicators (small circles) based on connection state
 */
function updateConnectionIndicators() {
    console.log(`📊 Updating connection indicators`);

    const container = document.querySelector('.status-indicators');
    if (container && !container.dataset.styled) {
        container.dataset.styled = "true";
        container.style.flexDirection = 'column';
        container.style.background = 'rgba(15, 23, 42, 0.85)';
        container.style.padding = '1rem';
        container.style.borderRadius = '0.5rem';
        container.style.border = '1px solid rgba(102, 126, 234, 0.3)';
        container.style.overflow = 'hidden';
        container.style.transition = 'max-height 0.3s ease';
        container.style.maxHeight = '150px';

        const label = document.createElement('div');
        label.innerHTML = 'Connection Status <span id="connection-chevron">▼</span>';
        label.style.color = '#667eea';
        label.style.fontWeight = 'bold';
        label.style.fontSize = '0.85rem';
        label.style.marginBottom = '0.5rem';
        label.style.cursor = 'pointer';
        label.style.userSelect = 'none';
        label.style.display = 'flex';
        label.style.justifyContent = 'space-between';
        label.style.alignItems = 'center';
        label.style.gap = '0.5rem';
        
        label.addEventListener('click', () => {
            const isCollapsed = container.style.maxHeight === '35px';
            container.style.maxHeight = isCollapsed ? '150px' : '35px';
            const chevron = document.getElementById('connection-chevron');
            if (chevron) chevron.textContent = isCollapsed ? '▼' : '▲';
        });
        
        container.insertBefore(label, container.firstChild);
    }

    const applyBloom = (indicator, isConnected) => {
        const color = isConnected ? '#10b981' : '#ef4444';
        indicator.style.backgroundColor = color;
        indicator.style.color = color;
        indicator.style.boxShadow = `0 0 4px ${color}`;
        indicator.style.filter = `drop-shadow(0 0 2px ${color})`;
    };

    // Update Muse indicator
    if (AppState.isMuseConnected) {
        DOM.museIndicator.classList.remove('disconnected');
        DOM.museIndicator.classList.add('connected');
        applyBloom(DOM.museIndicator, true);
    } else {
        DOM.museIndicator.classList.remove('connected');
        DOM.museIndicator.classList.add('disconnected');
        applyBloom(DOM.museIndicator, false);
    }

    // Update Mind Monitor indicator
    if (AppState.isMindMonitorConnected) {
        DOM.mindmonitorIndicator.classList.remove('disconnected');
        DOM.mindmonitorIndicator.classList.add('connected');
        applyBloom(DOM.mindmonitorIndicator, true);
    } else {
        DOM.mindmonitorIndicator.classList.remove('connected');
        DOM.mindmonitorIndicator.classList.add('disconnected');
        applyBloom(DOM.mindmonitorIndicator, false);
    }
}

/**
 * Update the state of "Establish connection" button
 * Disabled if both devices are connected
 */
function updateEstablishConnectionButtonState() {
    const bothConnected = AppState.isMuseConnected && AppState.isMindMonitorConnected;
    DOM.establishConnectionBtn.disabled = bothConnected;
}

// ============================================================================
// IMAGE PRELOADING
// ============================================================================

/**
 * Preload all 10 images asynchronously
 * Store textures in textureCache for later use in PixiJS
 * Falls back to generated placeholder canvases if images fail to load or on localhost
 */
async function preloadImages() {
    console.log('📸 Preloading images...');

    const loadPromises = [];

    for (let i = 0; i < TOTAL_IMAGES; i++) {
        const imageNumber = i + 2;
        const imagePath = `${IMAGE_PATH}${imageNumber}${IMAGE_EXTENSION}`;

        const loadPromise = PIXI.Assets.load(imagePath)
            .then((texture) => {
                console.log(`   ✅ Loaded: ${imagePath}`);
                textureCache[`image-${i}`] = texture;
            })
            .catch((err) => {
                console.warn(`   ⚠️  Failed to load: ${imagePath}. Generating placeholder...`);
                const placeholderDataURL = generatePlaceholderTexture(i);
                textureCache[`image-${i}`] = PIXI.Texture.from(placeholderDataURL);
            });

        loadPromises.push(loadPromise);
    }

    try {
        await Promise.all(loadPromises);
        console.log('✅ All images preloaded (or placeholders generated)');
    } catch (err) {
        console.error('❌ Error preloading images:', err);
    }
}

/**
 * Generate a colored placeholder canvas with text
 * Used for local testing when images fail to load or on localhost
 * 
 * @param {number} imageIndex - Image number (1-10)
 * @returns {string} Data URL of the placeholder canvas
 */
function generatePlaceholderTexture(index) {
    // Create canvas element
    const canvas = document.createElement('canvas');
    canvas.width = 1024;
    canvas.height = 768;

    const ctx = canvas.getContext('2d');

    // ========================================================================
    // Generate a unique color for each image based on its index
    // Uses HSL to create a nice spectrum of distinct colors
    // ========================================================================
    const hue = (index / TOTAL_IMAGES) * 360; // 0-360
    const saturation = 60 + (index % 3) * 10; // 60-90%
    const lightness = 40 + (index % 2) * 10; // 40-50%

    const backgroundColor = `hsl(${hue}, ${saturation}%, ${lightness}%)`;

    // Fill background
    ctx.fillStyle = backgroundColor;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    // Add subtle gradient overlay
    const gradient = ctx.createLinearGradient(0, 0, canvas.width, canvas.height);
    gradient.addColorStop(0, 'rgba(255, 255, 255, 0.1)');
    gradient.addColorStop(1, 'rgba(0, 0, 0, 0.2)');
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    // Draw centered text
    ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
    ctx.font = 'bold 64px Arial, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';

    const imageNumber = index + 2;
    const label = `Image ${imageNumber}`;
    ctx.shadowColor = 'rgba(0, 0, 0, 0.7)';
    ctx.shadowBlur = 10;
    ctx.shadowOffsetX = 2;
    ctx.shadowOffsetY = 2;

    ctx.fillText(label, canvas.width / 2, canvas.height / 2);

    // Draw magnification label in bottom right
    ctx.font = 'bold 32px Arial, sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'bottom';
    const magLabel = `${SCALE_LABELS[index]}x`;
    ctx.fillText(magLabel, canvas.width - 40, canvas.height - 40);

    // Convert to data URL
    const dataURL = canvas.toDataURL('image/png');
    console.log(`   ✨ Generated placeholder for Image ${imageNumber}`);

    return dataURL;
}

// ============================================================================
// PIXI.JS INITIALIZATION & RENDERING
// ============================================================================

/**
 * Initialize PixiJS application using v8 async factory pattern
 * Creates canvas, sprites, and rendering loop
 */
async function initializePixiJS() {
    console.log('🎨 Initializing PixiJS v8 Application...');

    try {
        // ====================================================================
        // PixiJS v8 Async Factory Pattern
        // ====================================================================
        // Create a new PixiJS application instance
        pixiApp = new PIXI.Application();

        // Initialize the app with options
        // resizeTo: window = automatically handles canvas resizing on window resize
        // backgroundAlpha: 0 = transparent background
        // antialias: true = smooth rendering
        await pixiApp.init({
            canvas: DOM.pixiCanvas,
            resizeTo: window,
            backgroundAlpha: 0,
            antialias: true,
        });

        console.log(`   ✅ PixiJS initialized. Canvas size: ${pixiApp.canvas.width}x${pixiApp.canvas.height}`);

        // ====================================================================
        // Create Sprites
        // ====================================================================
        // Create sprite A (current image during crossfade)
        spriteA = new PIXI.Sprite(PIXI.Texture.WHITE);
        spriteA.anchor.set(0.5);
        spriteA.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
        pixiApp.stage.addChild(spriteA);

        // Create sprite B (next image during crossfade)
        spriteB = new PIXI.Sprite(PIXI.Texture.WHITE);
        spriteB.anchor.set(0.5);
        spriteB.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
        pixiApp.stage.addChild(spriteB);

        // ====================================================================
        // Set up Animation Loop
        // ====================================================================
        // In PixiJS v8, the ticker starts automatically upon initialization.
        // We simply add our update function to the ticker.
        // The ticker parameter contains delta time information.
        pixiApp.ticker.add(updateFrame);

        console.log('✅ PixiJS v8 Application fully initialized');
    } catch (err) {
        console.error('❌ Failed to initialize PixiJS:', err);
        throw err;
    }
}

/**
 * Main animation frame update
 * Called every frame by PixiJS ticker
 */
function updateFrame() {
    // ========================================================================
    // FOCUS INTERPOLATION (Smoothing)
    // ========================================================================
    // Linear interpolation: gradually move currentFocus toward targetFocus
    // using the easing factor (FOCUS_EASING = 0.05 = 5% per frame)
    AppState.currentFocus += (AppState.targetFocus - AppState.currentFocus) * FOCUS_EASING;

    // ========================================================================
    // MAGNIFICATION INTERPOLATION
    // ========================================================================
    // Map currentFocus (0.0-1.0) to a fractional index across the 10-image array
    // This gives us a value from 0 to 9

    const fractionalIndex = AppState.currentFocus * (TOTAL_IMAGES - 1);

    // Split into integer and fractional parts
    const imageIndex = Math.floor(fractionalIndex);
    const localProgress = fractionalIndex - imageIndex;

    // Ensure we don't exceed array bounds
    const nextImageIndex = Math.min(imageIndex + 1, TOTAL_IMAGES - 1);

    // Smooth lerped visual zoom strictly from 1x to 100x max
    const currentMagnification = 1 + (AppState.currentFocus * 99);

    // ========================================================================
    // DUAL-SPRITE CROSSFADE ZOOM
    // ========================================================================
    // Sprite A: Current image (fading out as we zoom in)
    //   - Scale increases exponentially: Math.pow(2.0, localProgress)
    //   - Alpha decreases: 1 - localProgress
    // Sprite B: Next image (fading in as we zoom in)
    //   - Scale is half of Sprite A initially: scaleA / 2.0
    //   - Alpha increases: localProgress

    const scaleA = Math.pow(2.0, localProgress);
    const scaleB = scaleA / 2.0;

    spriteA.scale.set(scaleA);
    spriteA.alpha = 1 - localProgress;

    spriteB.scale.set(scaleB);
    spriteB.alpha = localProgress;

    // ========================================================================
    // LOAD TEXTURES FOR CURRENT IMAGE PAIR
    // ========================================================================
    // Load texture for Sprite A (imageIndex)
    const textureKeyA = `image-${imageIndex}`;
    spriteA.texture = textureCache[textureKeyA];

    // Load texture for Sprite B (nextImageIndex)
    const textureKeyB = `image-${nextImageIndex}`;
    spriteB.texture = textureCache[textureKeyB];

    // ========================================================================
    // UPDATE UI OVERLAYS
    // ========================================================================
    updateMagnificationBar(currentMagnification);
    updateWaveformBar();
}

// ============================================================================
// UI OVERLAY RENDERING
// ============================================================================

/**
 * Update the magnification bar display
 * Draws tick marks on canvas and updates the magnification label
 * Maps 0.0-1.0 focus to 1x-100x display (capped at 100x max)
 * 
 * @param {number} magnification - Current magnification value (calculated from SCALE_LABELS)
 */
function updateMagnificationBar(magnification) {
    const ctx = DOM.magnificationCanvas.getContext('2d');
    const canvas = DOM.magnificationCanvas;

    // Clear canvas
    ctx.fillStyle = 'rgba(0, 0, 0, 0)';
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    // Draw horizontal line (stroke only)
    ctx.strokeStyle = 'rgba(102, 126, 234, 0.6)';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(20, canvas.height / 2);
    ctx.lineTo(canvas.width - 20, canvas.height / 2);
    ctx.stroke();

    // ========================================================================
    // DYNAMIC TICK MARK SPACING
    // ========================================================================
    // The spacing of tick marks is proportional to the current local zoom level
    // At low zoom (localProgress near 0): marks are spread out
    // At high zoom (localProgress near 1): marks are closer together
    // This creates a visual effect where zooming changes the "frequency" of marks

    const fractionalIndex = AppState.currentFocus * (TOTAL_IMAGES - 1);
    const localProgress = fractionalIndex - Math.floor(fractionalIndex);

    // Base tick spacing in pixels
    // Higher zoom level (higher localProgress) = tighter spacing
    const baseSpacing = 20 + localProgress * 40; // Ranges from 20 to 60 pixels

    // Draw tick marks
    ctx.strokeStyle = 'rgba(102, 126, 234, 0.5)';
    ctx.lineWidth = 1;

    let x = canvas.width / 2; // Start from center
    const tickHeight = 8;

    // Draw ticks extending to the right from center
    while (x < canvas.width - 20) {
        ctx.beginPath();
        ctx.moveTo(x, canvas.height / 2 - tickHeight / 2);
        ctx.lineTo(x, canvas.height / 2 + tickHeight / 2);
        ctx.stroke();
        x += baseSpacing;
    }

    // Draw ticks extending to the left from center
    x = canvas.width / 2;
    while (x > 20) {
        ctx.beginPath();
        ctx.moveTo(x, canvas.height / 2 - tickHeight / 2);
        ctx.lineTo(x, canvas.height / 2 + tickHeight / 2);
        ctx.stroke();
        x -= baseSpacing;
    }

    // ========================================================================
    // CAPPED MAGNIFICATION DISPLAY (1x - 100x)
    // ========================================================================
    // The internal image transitions use the passed magnification parameter
    const displayMagnification = magnification;

    // Format label
    let label;
    if (displayMagnification >= 10) {
        label = Math.round(displayMagnification) + 'x';
    } else {
        label = displayMagnification.toFixed(1) + 'x';
    }
    DOM.magnificationLabel.textContent = label;
}

/**
 * Update the waveform bar display
 * Draws a real-time sine wave whose amplitude expands/contracts based on focus
 * Adds overlaid sine waves based on focus intensity and a subtle bloom.
 */
function updateWaveformBar() {
    const ctx = DOM.waveformCanvas.getContext('2d');
    const canvas = DOM.waveformCanvas;

    // Clear canvas
    ctx.fillStyle = 'rgba(0, 0, 0, 0)';
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    // ========================================================================
    // WAVEFORM PARAMETERS
    // ========================================================================
    // The amplitude of the sine wave is proportional to currentFocus
    // Calculate max amplitude dynamically to fit the canvas height
    // Accounting for the shadow blur to prevent clipping
    const maxAmplitude = (canvas.height / 2) - 10; 
    const amplitude = AppState.currentFocus * maxAmplitude;

    // Wave frequency and phase for animation
    const frequency = 0.05; // Lower = wider waves
    const basePhase = performance.now() * 0.003; // Animate over time
    const centerY = canvas.height / 2;

    // Apply bloom to all rendering in this canvas
    ctx.shadowBlur = 4;
    ctx.shadowColor = 'rgba(102, 126, 234, 0.8)';

    // Function to draw a single sine wave
    const drawWave = (phaseOffset, alpha, lineWidth) => {
        ctx.strokeStyle = `rgba(102, 126, 234, ${alpha})`;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        
        for (let x = 0; x < canvas.width; x++) {
            // Spatial Amplitude Modulation (Parabolic multiplier)
            // Normalized x position (0 to 1)
            const nx = x / canvas.width;
            // Parabola peaking at 1.0 in center, 0.0 at edges
            const parabolicMultiplier = 1.0 - Math.pow(2.0 * nx - 1.0, 2.0);
            
            // Multiply the amplitude by the spatial parabolic curve
            const y = centerY + (amplitude * parabolicMultiplier) * Math.sin(frequency * x + basePhase + phaseOffset);
            
            if (x === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.stroke();
    };

    // ========================================================================
    // OVERLAY WAVES
    // ========================================================================
    // Add extra waves based on concentration (currentFocus)
    // Map currentFocus to a number of extra waves (0 to 3 max)
    const extraWaves = Math.floor(AppState.currentFocus * 3);
    
    // Ensure we always have deterministic randomness for the current frame
    // We base the pseudo-random phase offsets on the number of waves so they jitter slightly
    for (let i = 0; i < extraWaves; i++) {
        // pseudo random phase offset and slightly reduced amplitude/alpha
        const phaseOffset = (i + 1) * 2.5 + Math.sin(basePhase * 0.5) * i;
        const overlayAlpha = 0.2 + (0.1 * AppState.currentFocus); 
        drawWave(phaseOffset, overlayAlpha, 1);
    }

    // ========================================================================
    // DRAW MAIN SINE WAVE
    // ========================================================================
    // Draw the primary wave last so it's on top
    drawWave(0, 0.8, 2);

    // Reset shadow for any other canvas operations
    ctx.shadowBlur = 0;
}

// ============================================================================
// WINDOW RESIZING
// ============================================================================

/**
 * Handle window resize
 * In PixiJS v8 with resizeTo: window, the canvas is automatically resized.
 * This function handles repositioning sprites and overlays.
 */
function onWindowResize() {
    // Safety check: ensure pixiApp is fully initialized
    if (!pixiApp || !pixiApp.screen) {
        console.warn('⚠️  PixiJS app not yet initialized, skipping resize handler');
        return;
    }

    console.log(`🔄 Window resized: ${window.innerWidth}x${window.innerHeight}`);

    // ========================================================================
    // Note: PixiJS v8 automatically handles canvas resizing with resizeTo: window
    // The renderer automatically adjusts to the new window dimensions.
    // ========================================================================

    // Reposition sprites to center (they should stay centered on window resize)
    if (spriteA && pixiApp.screen) {
        spriteA.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
    }
    if (spriteB && pixiApp.screen) {
        spriteB.position.set(pixiApp.screen.width / 2, pixiApp.screen.height / 2);
    }

    // Resize magnification canvas
    if (DOM.magnificationCanvas) {
        DOM.magnificationCanvas.width = Math.min(600, window.innerWidth * 0.8);
    }

    // Resize waveform canvas
    if (DOM.waveformCanvas) {
        DOM.waveformCanvas.width = Math.min(600, window.innerWidth * 0.8);
    }
}

// ============================================================================
// STARTUP
// ============================================================================

/**
 * Run initialization when DOM is fully loaded
 */
document.addEventListener('DOMContentLoaded', initialize);

// ============================================================================
// DEBUG HELPERS (Remove in production)
// ============================================================================

/**
 * Global debug function to inspect current state
 * Call from browser console: window.debugState()
 */
window.debugState = function() {
    console.log('=== APP STATE ===');
    console.log(`Current View: ${AppState.currentView}`);
    console.log(`Target Focus: ${AppState.targetFocus.toFixed(3)}`);
    console.log(`Current Focus: ${AppState.currentFocus.toFixed(3)}`);
    console.log(`Concentration Velocity: ${AppState.concentrationVelocity.toFixed(4)}`);
    const displayMag = 1 + (AppState.currentFocus * 99);
    console.log(`Display Magnification (Capped): ${displayMag.toFixed(1)}x`);
    console.log(`Muse Connected: ${AppState.isMuseConnected}`);
    console.log(`Mind Monitor Connected: ${AppState.isMindMonitorConnected}`);
    console.log(`Simulation Mode: ${AppState.isSimulationMode}`);
    console.log(`WebSocket State: ${AppState.ws ? AppState.ws.readyState : 'Not initialized'}`);
};

/**
 * Global debug function to simulate focus changes
 * Call from browser console: window.debugSetFocus(0.5)
 */
window.debugSetFocus = function(focusValue) {
    AppState.targetFocus = Math.max(0.0, Math.min(1.0, focusValue));
    console.log(`🎯 Debug: Target focus set to ${AppState.targetFocus.toFixed(3)}`);
};

/**
 * Global debug function to toggle connection states
 * Call from browser console: window.debugToggleMuse() or window.debugToggleMindMonitor()
 */
window.debugToggleMuse = function() {
    AppState.isMuseConnected = !AppState.isMuseConnected;
    updateConnectionIndicators();
    updateEstablishConnectionButtonState();
    console.log(`🎯 Debug: Muse connection toggled to ${AppState.isMuseConnected}`);
};

window.debugToggleMindMonitor = function() {
    AppState.isMindMonitorConnected = !AppState.isMindMonitorConnected;
    updateConnectionIndicators();
    updateEstablishConnectionButtonState();
    console.log(`🎯 Debug: Mind Monitor connection toggled to ${AppState.isMindMonitorConnected}`);
};

/**
 * Global debug function to simulate concentration velocity
 * Call from browser console: window.debugConcentration(0.05)
 */
window.debugConcentration = function(velocity) {
    AppState.concentrationVelocity = Math.max(-0.1, Math.min(0.1, velocity));
    console.log(`🎯 Debug: Concentration velocity set to ${AppState.concentrationVelocity.toFixed(4)}`);
};

/**
 * Global debug function to observe decay in real-time
 * Call from browser console: window.debugWatchDecay() to watch 10 frames
 */
window.debugWatchDecay = function() {
    console.log('%c=== CONCENTRATION VELOCITY DECAY OBSERVATION ===', 'color: #667eea; font-weight: bold;');
    let frame = 0;
    const interval = setInterval(() => {
        console.log(`Frame ${frame}: Concentration Velocity = ${AppState.concentrationVelocity.toFixed(6)}`);
        frame++;
        if (frame >= 10) {
            clearInterval(interval);
        }
    }, 30);
};

console.log('%c🧠 Mind Zoom Application Loaded', 'color: #667eea; font-size: 16px; font-weight: bold;');
console.log('%cDebug commands available:', 'color: #cbd5e1;');
console.log('%c  debugState() - View all app state', 'color: #cbd5e1;');
console.log('%c  debugSetFocus(0-1) - Set focus level', 'color: #cbd5e1;');
console.log('%c  debugToggleMuse() - Toggle Muse connection', 'color: #cbd5e1;');
console.log('%c  debugToggleMindMonitor() - Toggle Mind Monitor connection', 'color: #cbd5e1;');
console.log('%c  debugConcentration(velocity) - Set concentration velocity (-0.1 to 0.1)', 'color: #cbd5e1;');
console.log('%c  debugWatchDecay() - Observe concentration decay over 10 frames', 'color: #cbd5e1;');
