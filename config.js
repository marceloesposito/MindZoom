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
    // ENABLE_HOOK_PHASE = false: chiusa la modale si va dritti all'interazione.
    // Con true si riattivano HOOK (auto-zoom di aggancio) e HANDOVER (crossfade di autorità).
    ENABLE_HOOK_PHASE: false,
    PHASE_HOOK_S: 5.5,          // auto-zoom di aggancio (solo se ENABLE_HOOK_PHASE)
    PHASE_HANDOVER_S: 5.0,      // crossfade di autorità auto -> EEG (idem)
    PHASE_INTERACTIVE_S: 80.0,  // core interattivo
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
    P_LOW: 0.35,                // bordo inferiore dead-zone
    P_HIGH: 0.65,               // bordo superiore dead-zone (dead-zone = 30% del range)
    // ZOOM_GAIN scala percentile -> velocità. A gain 1.0 si attraversano tutti i 12
    // livelli in ~5.5 s: troppo per una fase interattiva da 80 s, e il percentile satura
    // spesso (p=1.0 ogni volta che il campione è il massimo del ring). A 0.25 la
    // traversata completa a velocità piena richiede ~22 s.
    ZOOM_GAIN: 0.25,            // TUNE
    VEL_SMOOTHING: 0.25,        // TUNE - EMA sulla velocità, smussa gli step a 5.3 Hz

    // --- Detent / isteresi / hold ---
    // Le soglie sono FRAZIONI della velocità di input massima, non valori assoluti:
    // così cambiare ZOOM_GAIN non rende un detent inescapabile.
    SNAP_STRENGTH: 0.06,        // forza dell'attrattore verso il livello
    SNAP_VEL_FRAC: 0.25,        // sotto cui si attiva lo snap al livello
    ENTER_HOLD_FRAC: 0.25,      // TUNE - soglia di aggancio di un detent
    BREAK_HOLD_FRAC: 0.55,      // TUNE - soglia di sgancio (deve essere > ENTER_HOLD_FRAC)
    LOCK_DWELL_S: 3.0,          // dwell-to-lock
    LOCK_DWELL_MULT: 1.5,       // TUNE - moltiplicatore della soglia di sgancio dopo il dwell

    // --- Gating artefatti (µV) ---
    // Soglia sul segnale dopo rimozione della DC, prima del filtraggio in banda.
    ARTIFACT_UV_RAW: 100,
    // Dopo questo tempo di gating continuo la velocità decade verso 0 invece di
    // restare congelata: un gating permanente non deve poter incollare lo zoom.
    ARTIFACT_HOLD_MAX_S: 1.0,
    CONTACT_STD_MIN: 0.5,       // TUNE - proxy contatto: sotto = canale piatto/staccato
    CONTACT_STD_MAX: 60.0,      // TUNE - proxy contatto: sopra = rumore/contatto instabile

    // --- HUD / debug ---
    HUD_UPDATE_HZ: 12,          // throttling degli update DOM
    DEBUG_SENSOR_LOG: true,     // log in console di ciò che il sensore sta leggendo
    DEBUG_LOG_HZ: 2             // frequenza dei log (il controllo gira a ~5.3 Hz)
};

window.CONFIG = CONFIG;
