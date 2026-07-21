/**
 * MIND ZOOM - Configurazione unica della pipeline di controllo.
 * Tutte le costanti tarabili vivono qui. I valori marcati // TUNE sono
 * default ragionevoli ma non validati sul campo.
 */

const CONFIG = {
    // --- Selettore pipeline (A/B con il percorso legacy a snapshot 5s) ---
    USE_ADAPTIVE_PIPELINE: true,

    // --- Fase ONBOARDING / calibrazione sotto la modale ---
    MODAL_UNLOCK_S: 6.0,        // quando il pulsante diventa cliccabile
    CALIB_START_S: 1.0,         // scarto del transitorio di reazione iniziale
    CALIB_END_OFFSET_S: 0.5,    // chiusura buffer prima dell'unlock (zona gesto di chiusura)
    CALIB_MIN_SAMPLES: 12,      // sotto questa soglia la semina è considerata fallita

    // --- Durate delle fasi (s) ---
    PHASE_HOOK_S: 5.5,          // auto-zoom di aggancio
    PHASE_HANDOVER_S: 5.0,      // crossfade di autorità auto -> EEG
    PHASE_INTERACTIVE_S: 70.0,  // core interattivo
    PHASE_OUTRO_S: 18.0,        // conclusione scriptata

    // --- Velocità scriptate ---
    HOOK_AUTO_VELOCITY: 0.45,   // TUNE - velocità normalizzata dell'auto-zoom di aggancio
    OUTRO_TARGET_FOCUS: 1.0,    // discesa finale; 0.0 = ritiro in superficie
    OUTRO_EASING: 0.012,        // TUNE - easing lungo dell'outro

    // --- STFT a finestra scorrevole ---
    STFT_WINDOW: 256,           // campioni (1 s @256 Hz) - risolve theta a 4 Hz
    STFT_HOP: 48,               // -> ~5.3 Hz di update del controllo

    // --- Normalizzazione ---
    NORM_MODE: "percentile",    // "percentile" | "ema"
    PCTL_BUFFER_S: 13,          // finestra del ring buffer percentile
    EMA_TAU_S: 10,              // solo se NORM_MODE = "ema"
    IQR_MIN_REL: 0.05,          // TUNE - dispersion guard, IQR relativo alla mediana seminata

    // --- Mapping percentile -> velocità ---
    P_LOW: 0.40,                // bordo inferiore dead-zone
    P_HIGH: 0.60,               // bordo superiore dead-zone
    ZOOM_GAIN: 1.0,             // TUNE - scala percentile -> velocità
    VEL_SMOOTHING: 0.25,        // TUNE - EMA sulla velocità, smussa gli step a 5.3 Hz

    // --- Detent / isteresi / hold ---
    SNAP_VEL_THRESHOLD: 0.08,   // sotto cui si attiva lo snap al livello
    SNAP_STRENGTH: 0.06,        // forza dell'attrattore verso il livello
    ENTER_HOLD: 0.08,           // TUNE - soglia di aggancio di un detent
    BREAK_HOLD: 0.22,           // TUNE - soglia di sgancio (deve essere > ENTER_HOLD)
    LOCK_DWELL_S: 3.0,          // dwell-to-lock
    LOCK_DWELL_MULT: 1.6,       // TUNE - moltiplicatore di BREAK_HOLD dopo il dwell

    // --- Gating artefatti (µV) ---
    ARTIFACT_UV_RAW: 100,       // soglia sul segnale grezzo non filtrato
    CONTACT_STD_MIN: 0.5,       // TUNE - proxy contatto: sotto = canale piatto/staccato
    CONTACT_STD_MAX: 60.0,      // TUNE - proxy contatto: sopra = rumore/contatto instabile

    // --- HUD ---
    HUD_UPDATE_HZ: 12           // throttling degli update DOM
};

window.CONFIG = CONFIG;
