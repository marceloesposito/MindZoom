/**
 * MIND ZOOM - Configurazione unica della pipeline di controllo.
 * Tutte le costanti tarabili vivono qui. I valori marcati // TUNE sono
 * default ragionevoli ma non validati sul campo.
 */

const CONFIG = {
    // --- Selettore pipeline (A/B con il percorso legacy a snapshot 5s) ---
    USE_ADAPTIVE_PIPELINE: true,

    // --- Calibrazione ATTIVA (scheda con quadratino su binario) ---
    // Due fasi guidate: l'utente spinge il quadratino verso l'alto concentrandosi
    // (registra il picco -> absMax) e poi lo lascia scendere rilassandosi
    // (registra il minimo -> absMin). Da questi due estremi si ricava la legge di
    // controllo dello zoom (vedi computeExtremaVelocity). Niente moving-average.
    CALIB_INTRO_MIN_S: 1.0,     // tempo minimo sulla schermata introduttiva
    CALIB_CONCENTRATE_S: 15.0,  // durata fase di concentrazione
    CALIB_RELAX_S: 15.0,        // durata fase di rilassamento
    CALIB_LEADIN_S: 1.5,        // scarto iniziale di ogni fase (transitorio di reazione)
    CALIB_INDEX_EMA: 0.30,      // TUNE - EMA di denoise sull'indice (NON normalizzazione)
    CALIB_DISPLAY_EMA: 0.20,    // TUNE - EMA di posizione del quadratino a 60 Hz
    CALIB_CONC_FRACTION: 0.75,  // la velocità piena si raggiunge al 75% del tragitto M->estremo
    CALIB_DONE_HOLD_S: 1.6,     // schermata "completata" prima dell'auto-avanzo all'interazione
    // Span minima accettabile fra i due estremi, relativa al neutro M: sotto questa
    // la calibrazione è considerata fallita (segnale piatto / utente non modulante).
    CALIB_MIN_SPAN_REL: 0.08,   // TUNE

    // --- Controllo dello zoom a estremi (post-calibrazione) ---
    // Velocità di zoom in funzione della concentrazione relativa agli estremi
    // assoluti (calibrazione) e a una banda locale di isteresi.
    EXTREMA_GAIN: 0.25,         // TUNE - velocità normalizzata massima (ex ZOOM_GAIN)
    // Quanto in fretta gli estremi LOCALI seguono il segnale, in frazioni di span/s.
    // Piccolo -> un plateau ferma lo zoom, bisogna spingere ancora (fasico).
    // Grande -> controllo più continuo.
    LOCAL_DECAY: 0.35,          // TUNE

    // --- Onboarding legacy (percorso passivo, USE_ADAPTIVE_PIPELINE con semina 5s) ---
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
    // ENABLE_TIME_LIMIT = false: la fase interattiva non scade mai e l'outro non
    // parte da solo. L'esperienza dura finché l'utente non esce.
    ENABLE_TIME_LIMIT: false,
    PHASE_INTERACTIVE_S: 80.0,  // usato solo se ENABLE_TIME_LIMIT
    PHASE_OUTRO_S: 18.0,        // conclusione scriptata

    // --- Velocità scriptate ---
    HOOK_AUTO_VELOCITY: 0.45,   // TUNE - velocità normalizzata dell'auto-zoom di aggancio
    OUTRO_TARGET_FOCUS: 1.0,    // discesa finale; 0.0 = ritiro in superficie
    OUTRO_EASING: 0.012,        // TUNE - easing lungo dell'outro

    // --- Decodifica dei campioni EEG ---
    // 'unsigned-centered': l'ADC emette unsigned centrati su 8192 (come il decode di
    //   riferimento del Muse, che a 12 bit sottrae 0x800).
    // 'signed': complemento a due (comportamento originale). Se i valori reali stanno
    //   attorno a 8192 produce un'onda quadra da ±725 µV invece che EEG.
    EEG_DECODE_MODE: 'unsigned-centered',

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
    // EMA sulla velocità: il controllo aggiorna a 5.3 Hz mentre il render gira a 60,
    // quindi senza smoothing si vedono gli scalini. Più basso = più fluido, più lento.
    VEL_SMOOTHING: 0.15,        // TUNE

    // --- Detent / isteresi / hold ---
    // Le soglie sono FRAZIONI della velocità di input massima, non valori assoluti:
    // così cambiare ZOOM_GAIN non rende un detent inescapabile.
    SNAP_STRENGTH: 0.06,        // forza dell'attrattore verso il livello
    SNAP_VEL_FRAC: 0.25,        // sotto cui si attiva lo snap al livello
    ENTER_HOLD_FRAC: 0.25,      // TUNE - soglia di aggancio di un detent
    BREAK_HOLD_FRAC: 0.55,      // TUNE - soglia di sgancio (deve essere > ENTER_HOLD_FRAC)
    LOCK_DWELL_S: 3.0,          // dwell-to-lock
    LOCK_DWELL_MULT: 1.5,       // TUNE - moltiplicatore della soglia di sgancio dopo il dwell

    // --- Gating artefatti ---
    // Il gating è RELATIVO, non assoluto: una soglia fissa in µV non funziona con
    // elettrodi dry di livello consumer, dove il picco su 1 s sta normalmente su
    // centinaia di µV e varia con la persona e con la qualità del contatto.
    // Una finestra è artefatto se il suo picco supera ARTIFACT_REL_MULT volte la
    // mediana dei picchi recenti E sta sopra un pavimento assoluto di sicurezza.
    ARTIFACT_REL_MULT: 2.5,     // TUNE
    ARTIFACT_UV_FLOOR: 150,     // TUNE - sotto questo non si marca mai artefatto
    ARTIFACT_PEAK_HISTORY: 48,  // ~9 s di picchi a 5.3 Hz
    // Dopo questo tempo di gating continuo la velocità decade verso 0 invece di
    // restare congelata: un gating permanente non deve poter incollare lo zoom.
    ARTIFACT_HOLD_MAX_S: 1.0,
    CONTACT_STD_MIN: 0.5,       // TUNE - proxy contatto: sotto = canale piatto/staccato
    CONTACT_STD_MAX: 60.0,      // TUNE - proxy contatto: sopra = rumore/contatto instabile

    // --- Watchdog del flusso EEG ---
    // Il flusso BLE può fermarsi senza emettere alcun evento: senza watchdog
    // l'esperienza si congela in silenzio e non si capisce perché.
    EEG_WATCHDOG_S: 3.0,        // secondi senza dati prima di segnalare
    EEG_WATCHDOG_RESUME: true,  // prova a rimandare il comando di streaming
    EEG_WATCHDOG_MAX_RETRY: 3,

    // --- HUD / debug ---
    HUD_UPDATE_HZ: 12,          // throttling degli update DOM
    DEBUG_SENSOR_LOG: true,     // log in console di ciò che il sensore sta leggendo
    DEBUG_LOG_HZ: 2             // frequenza dei log (il controllo gira a ~5.3 Hz)
};

window.CONFIG = CONFIG;
