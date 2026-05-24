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
 * Index 0 = 1.webp at 1x, Index 9 = 10.webp at 50000x
 * These represent the actual SEM magnification values.
 */
const SCALE_LABELS = [1, 10, 50, 200, 500, 1000, 5000, 10000, 25000, 50000];

/**
 * Total number of images available (1.webp through 10.webp)
 */
const TOTAL_IMAGES = 10;

/**
 * Image file extension
 */
const IMAGE_EXTENSION = '.webp';

/**
 * Path to images folder (absolute from root after public directory migration)
 */
const IMAGE_PATH = '/images/';

/**
 * WebSocket server URL for receiving focus data
 */
const WS_SERVER_URL = 'wss://il-tuo-bridge.onrender.com';

/**
 * Maximum WebSocket reconnection attempts (then give up)
 */
const MAX_WS_RECONNECT_ATTEMPTS = 3;

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

    // Concentration-based zoom mechanics
    // concentrationVelocity: increases with wheel input, naturally decays over time
    concentrationVelocity: 0.0,

    // WebSocket connection tracking
    ws: null,
    wsReconnectAttempts: 0,
};

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

/**
 * Activate simulation mode: listen to mouse wheel for focus control
 * Instead of directly incrementing focus, wheel input increases concentrationVelocity
 * which is then processed in the updateFrame loop with natural decay
 */
function activateSimulationMode() {
    console.log('🖱️  Mouse wheel listener activated for focus simulation');

    // Listen to wheel events on immersion container
    DOM.immersionContainer.addEventListener('wheel', (event) => {
        event.preventDefault();

        // ====================================================================
        // Concentration Velocity Model
        // ====================================================================
        // Instead of directly modifying focus, we increase concentrationVelocity
        // This creates a more organic, fluid interaction where:
        // - Multiple wheel events compound into a velocity
        // - The velocity naturally decays when no input is received
        // - The zoom feels reactive rather than mechanical
        // ====================================================================

        // Positive deltaY = scrolling down = increasing concentration (zooming in)
        // Negative deltaY = scrolling up = decreasing concentration (zooming out)
        const wheelDelta = event.deltaY > 0 ? 0.02 : -0.02;

        // Add to concentration velocity (accumulate input)
        AppState.concentrationVelocity = Math.max(-0.1, Math.min(0.1, AppState.concentrationVelocity + wheelDelta));

        console.log(`🎯 Concentration Velocity: ${AppState.concentrationVelocity.toFixed(3)}`);
    }, { passive: false });
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

    // Show immersion container
    DOM.immersionContainer.style.display = 'block';

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

            // ================================================================
            // Capped Reconnection Logic (Max 3 attempts)
            // ================================================================
            if (AppState.wsReconnectAttempts < MAX_WS_RECONNECT_ATTEMPTS) {
                AppState.wsReconnectAttempts++;
                console.log(`🔄 Reconnection attempt ${AppState.wsReconnectAttempts}/${MAX_WS_RECONNECT_ATTEMPTS}...`);
                setTimeout(() => {
                    initializeWebSocket();
                }, 3000);
            } else {
                console.log(`⛔ WebSocket reconnection attempts exhausted (${MAX_WS_RECONNECT_ATTEMPTS} attempts). Falling back to simulation mode.`);
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

    // Update Muse indicator
    if (AppState.isMuseConnected) {
        DOM.museIndicator.classList.remove('disconnected');
        DOM.museIndicator.classList.add('connected');
    } else {
        DOM.museIndicator.classList.remove('connected');
        DOM.museIndicator.classList.add('disconnected');
    }

    // Update Mind Monitor indicator
    if (AppState.isMindMonitorConnected) {
        DOM.mindmonitorIndicator.classList.remove('disconnected');
        DOM.mindmonitorIndicator.classList.add('connected');
    } else {
        DOM.mindmonitorIndicator.classList.remove('connected');
        DOM.mindmonitorIndicator.classList.add('disconnected');
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

    for (let i = 1; i <= TOTAL_IMAGES; i++) {
        const imagePath = `${IMAGE_PATH}${i}${IMAGE_EXTENSION}`;

        // Create a promise for each image load
        const loadPromise = new Promise((resolve, reject) => {
            const img = new Image();
            img.onload = () => {
                console.log(`   ✅ Loaded: ${imagePath}`);
                resolve();
            };
            img.onerror = () => {
                console.warn(`   ⚠️  Failed to load: ${imagePath}. Generating placeholder...`);
                // Generate colored placeholder canvas
                const placeholderDataURL = generatePlaceholderTexture(i);
                textureCache[`image-${i - 1}`] = PIXI.Texture.from(placeholderDataURL);
                resolve();
            };
            img.src = imagePath;
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
function generatePlaceholderTexture(imageIndex) {
    // Create canvas element
    const canvas = document.createElement('canvas');
    canvas.width = 1024;
    canvas.height = 768;

    const ctx = canvas.getContext('2d');

    // ========================================================================
    // Generate a unique color for each image based on its index
    // Uses HSL to create a nice spectrum of distinct colors
    // ========================================================================
    const hue = (imageIndex / TOTAL_IMAGES) * 360; // 0-360
    const saturation = 60 + (imageIndex % 3) * 10; // 60-90%
    const lightness = 40 + (imageIndex % 2) * 10; // 40-50%

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

    const label = `Image ${imageIndex}`;
    ctx.shadowColor = 'rgba(0, 0, 0, 0.7)';
    ctx.shadowBlur = 10;
    ctx.shadowOffsetX = 2;
    ctx.shadowOffsetY = 2;

    ctx.fillText(label, canvas.width / 2, canvas.height / 2);

    // Draw magnification label in bottom right
    ctx.font = 'bold 32px Arial, sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'bottom';
    const magLabel = `${SCALE_LABELS[imageIndex - 1]}x`;
    ctx.fillText(magLabel, canvas.width - 40, canvas.height - 40);

    // Convert to data URL
    const dataURL = canvas.toDataURL('image/png');
    console.log(`   ✨ Generated placeholder for Image ${imageIndex}`);

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
    // CONCENTRATION DECAY MECHANICS
    // ========================================================================
    // When no wheel input is received, concentrationVelocity naturally decays
    // This creates an organic, fluid zoom feel where the zoom level smoothly
    // drops back down when concentration fades (no user input)
    // Decay factor: 0.95 = 5% reduction per frame (~30fps = smooth decay)
    AppState.concentrationVelocity *= 0.95;

    // ========================================================================
    // UPDATE TARGET FOCUS FROM CONCENTRATION VELOCITY
    // ========================================================================
    // In simulation mode, the target focus is driven by concentration velocity
    // In connected mode, it's driven by WebSocket focus data
    // Both can coexist - WebSocket overrides when connected
    if (AppState.isSimulationMode) {
        AppState.targetFocus = Math.max(0.0, Math.min(1.0, AppState.targetFocus + AppState.concentrationVelocity));
    }

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

    // Calculate the interpolated magnification value between two consecutive scale labels
    // For example: if imageIndex = 2 and localProgress = 0.5
    //   magnification = SCALE_LABELS[2] * (1 - 0.5) + SCALE_LABELS[3] * 0.5
    //   = 50 * 0.5 + 200 * 0.5 = 125x
    const currentMagnification = SCALE_LABELS[imageIndex] * (1 - localProgress) + SCALE_LABELS[nextImageIndex] * localProgress;

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
    if (!textureCache[textureKeyA]) {
        textureCache[textureKeyA] = PIXI.Texture.from(`${IMAGE_PATH}${imageIndex + 1}${IMAGE_EXTENSION}`);
    }
    spriteA.texture = textureCache[textureKeyA];

    // Load texture for Sprite B (nextImageIndex)
    const textureKeyB = `image-${nextImageIndex}`;
    if (!textureCache[textureKeyB]) {
        textureCache[textureKeyB] = PIXI.Texture.from(`${IMAGE_PATH}${nextImageIndex + 1}${IMAGE_EXTENSION}`);
    }
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
    // Map the currentFocus (0.0-1.0) to a 1x-100x display range
    // This provides a smooth, intuitive magnification scale for the user
    // The internal image transitions still use SCALE_LABELS for precise control
    const displayMagnification = 1 + (AppState.currentFocus * 99); // 1x at 0.0, 100x at 1.0

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
    // At focus = 0.0: minimal amplitude
    // At focus = 1.0: maximum amplitude
    const maxAmplitude = canvas.height / 4; // Max wave height
    const amplitude = AppState.currentFocus * maxAmplitude;

    // Wave frequency and phase for animation
    const frequency = 0.05; // Lower = wider waves
    const phase = performance.now() * 0.003; // Animate over time

    // ========================================================================
    // DRAW SINE WAVE
    // ========================================================================
    ctx.strokeStyle = 'rgba(102, 126, 234, 0.8)';
    ctx.lineWidth = 2;
    ctx.beginPath();

    const centerY = canvas.height / 2;

    for (let x = 0; x < canvas.width; x++) {
        // Sine wave: y = A * sin(ωx + φ)
        // A = amplitude (proportional to focus)
        // ω = frequency
        // φ = phase (for animation)
        const y = centerY + amplitude * Math.sin(frequency * x + phase);

        if (x === 0) {
            ctx.moveTo(x, y);
        } else {
            ctx.lineTo(x, y);
        }
    }

    ctx.stroke();

    // Optional: Draw a subtle background glow
    ctx.strokeStyle = 'rgba(102, 126, 234, 0.2)';
    ctx.lineWidth = 4;
    ctx.stroke();
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
