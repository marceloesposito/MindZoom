#pragma once

// Specchio 1:1 di config.js + le costanti di testa di app.js.
// Single source of truth del port: qui NON si re-inventa nulla, si trascrive.
// I valori marcati TUNE sono le manopole di feel, da ri-tarare sul campo.
//
// I valori marcati LIVE hanno una copia modificabile a runtime in
// control/tunables.hpp: qui restano i DEFAULT, che sono anche ciò a cui il
// tasto R riporta tutto.

#include <array>
#include <cstddef>

namespace mz::config {

// --- Acquisizione ---
inline constexpr int    kSampleRate   = 256;   // Hz, hardware Muse (non alzare)
inline constexpr int    kChannels     = 4;     // TP9, AF7, AF8, TP10
inline constexpr int    kFrontalA     = 1;     // AF7
inline constexpr int    kFrontalB     = 2;     // AF8

// --- STFT a finestra scorrevole ---
inline constexpr int    kStftWindow   = 256;   // 1 s @256 Hz: risolve theta a 4 Hz
inline constexpr int    kStftHop      = 48;    // -> ~5.33 Hz di update del controllo
inline constexpr int    kStftBins     = kStftWindow / 2;

// Rate del controllo e passo temporale corrispondente.
inline constexpr double kControlHz    = double(kSampleRate) / double(kStftHop);
inline constexpr double kControlDt    = 1.0 / kControlHz;

// --- Decodifica campioni EEG ---
// L'ADC del Muse emette unsigned a 14 bit centrati sul fondo scala / 2.
inline constexpr int    kAdcCenter    = 8192;
inline constexpr double kAdcToMicrovolts = 1450.0 / 16383.0;

// --- Bin dell'indice di Pope (1 bin = FS/N = 1 Hz) ---
inline constexpr int    kThetaLo = 4,  kThetaHi = 8;
inline constexpr int    kAlphaLo = 8,  kAlphaHi = 12;
inline constexpr int    kBetaLo  = 13, kBetaHi  = 30;

// --- Filtri (biquad, forma diretta I) ---
// y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
inline constexpr double kDcLeak = 0.02;        // predittore DC
inline constexpr double kNotchB0 = 0.9391, kNotchB1 = -0.4024, kNotchB2 = 0.9391;
inline constexpr double kNotchA1 = -0.4024, kNotchA2 = 0.8782;
inline constexpr double kLpB0 = 0.2066, kLpB1 = 0.4132, kLpB2 = 0.2066;
inline constexpr double kLpA1 = -0.3695, kLpA2 = 0.1958;

// --- Calibrazione attiva ---
inline constexpr double kCalibIntroMinS    = 1.0;
inline constexpr double kCalibConcentrateS = 15.0;
inline constexpr double kCalibRelaxS       = 15.0;
inline constexpr double kCalibLeadInS      = 1.5;   // scarto del transitorio di reazione
inline constexpr double kCalibDisplayEma   = 0.20;  // TUNE - posizione quadratino a 60 Hz
inline constexpr double kCalibConcFraction = 0.50;  // LIVE - saturazione al 50% di M->estremo
inline constexpr double kCalibDoneHoldS    = 1.6;
inline constexpr double kCalibMinSpanRel   = 0.08;  // TUNE - soglia di fallimento

// --- Condizionamento dell'indice di Pope ---
// L'indice è un rapporto fra potenze di banda: ha code pesanti, un singolo
// campione anomalo passerebbe dritto in un EMA. Prima la mediana lo toglie,
// poi due poli in cascata smussano senza lasciare spigoli.
inline constexpr int    kIndexMedianTaps = 5;       // dispari; latenza (n-1)/2 campioni
inline constexpr double kIndexTauS       = 0.90;    // TUNE - costante di tempo, non un alfa

// --- Controllo a estremi ---
inline constexpr double kExtremaGain     = 0.30;    // LIVE - velocità normalizzata massima
// Velocità con cui la banda locale insegue il segnale, in frazioni di span/s.
// Va letta INSIEME allo smoothing dell'indice: ora che `c` è molto più liscio,
// un decadimento alto rende la banda inerte (resta incollata a `c`, il gate
// vale 1 sempre) e con essa la tolleranza. Misurato su registrazione: a 0.35 la
// tolleranza non cambia nulla, a 0.10 sposta il tempo di controllo attivo dal
// 62% al 93%. Da qui il valore, più basso del precedente 0.35.
inline constexpr double kLocalDecay      = 0.10;    // LIVE
// Ampiezza della rampa di attivazione, in frazioni della semi-span M->estremo.
// È il compromesso fra "fasico e faticoso" (piccolo) e "continuo e facile"
// (grande): con 0 si torna esattamente al gradino di prima.
inline constexpr double kLocalTolerance  = 0.18;    // LIVE
// Zona morta attorno al neutro, in frazioni di u: toglie la deriva a riposo
// senza reintrodurre una soglia netta.
inline constexpr double kNeutralDeadzone = 0.10;    // TUNE

// --- Smoothing della velocità ---
// Il controllo aggiorna a 5.3 Hz mentre il render gira a 60: senza smoothing si
// vedono gli scalini. Costanti di tempo, non alfa: indipendenti dal rate.
inline constexpr double kVelTauS       = 0.35;      // LIVE - a valle della legge di controllo
inline constexpr double kVelRenderTauS = 0.12;      // TUNE - interpolazione 5.3 Hz -> 60 Hz
inline constexpr double kVelStaleTauS  = 0.50;      // TUNE - decadimento a segnale assente

// --- Rate control (INVARIATO rispetto al JS) ---
inline constexpr double kZoomSpeedFactor = 0.003;
inline constexpr double kFocusEasing     = 0.06;

// --- Detent / isteresi / hold ---
inline constexpr double kSnapStrength   = 0.06;
inline constexpr double kSnapVelFrac    = 0.25;
inline constexpr double kEnterHoldFrac  = 0.15;   // TUNE
inline constexpr double kBreakHoldFrac  = 0.32;   // TUNE (> kEnterHoldFrac)
inline constexpr double kLockDwellS     = 3.0;
inline constexpr double kLockDwellMult  = 1.25;   // TUNE
// L'aggancio richiede PERMANENZA sotto soglia: senza, un controllo che passa per
// zero fra un impulso e l'altro si riaggancia ad ogni frame e annulla il
// movimento appena ottenuto.
inline constexpr double kEnterHoldDwellS  = 0.90; // TUNE
// Dopo uno sgancio il lock resta disabilitato per questo tempo, così si ottiene
// una finestra di movimento utilizzabile invece di un singolo frame.
inline constexpr double kBreakRefractoryS = 1.50; // TUNE

// --- Gating artefatti / contatto ---
inline constexpr double kArtifactRelMult    = 2.5;   // TUNE
inline constexpr double kArtifactUvFloor    = 150.0; // TUNE
inline constexpr int    kArtifactPeakHistory = 48;   // ~9 s di picchi a 5.3 Hz
inline constexpr double kArtifactHoldMaxS   = 1.0;
inline constexpr double kContactStdMin      = 0.5;   // TUNE - canale piatto = staccato

// --- Plausibilità fisica del segnale ---
// Rispondono alla domanda che precede ogni gating: quello che arriva è un
// segnale biologico? Senza questi controlli una sessione intera si calibra su
// rumore e nessuno se ne accorge, perché l'indice di Pope su rumore bianco vale
// meccanicamente 17/8 = 2.1 e sembra un valore normale.
//
// L'autocorrelazione è la prova decisiva: a 256 Hz due campioni consecutivi
// distano 4 ms, e l'EEG è fortemente passa-banda, quindi si somigliano quasi del
// tutto. Valori reali stanno sopra 0.9; byte non interpretabili danno ~0.
inline constexpr double kMinAutocorr1   = 0.50;  // TUNE - generoso di proposito
inline constexpr double kMaxRailFraction = 0.02; // oltre il 2% ai fondo scala = saturo
inline constexpr double kMinSpreadCounts = 2.0;  // sotto = canale piatto

// --- Watchdog del flusso ---
inline constexpr double kEegWatchdogS     = 3.0;
inline constexpr int    kEegWatchdogRetry = 3;

// --- Fasi ---
inline constexpr bool   kEnableHookPhase = false;
inline constexpr bool   kEnableTimeLimit = false;
inline constexpr double kPhaseHookS      = 5.5;
inline constexpr double kPhaseHandoverS  = 5.0;
inline constexpr double kPhaseInteractiveS = 80.0;
inline constexpr double kPhaseOutroS     = 18.0;
inline constexpr double kHookAutoVelocity = 0.45;  // TUNE
inline constexpr double kOutroTargetFocus = 1.0;
inline constexpr double kOutroEasing      = 0.012; // TUNE

// --- Rendering ---
inline constexpr int kTotalImages = 12;            // /images/1..12.webp
inline constexpr std::array<int, kTotalImages> kScaleLabels = {
    32, 32, 100, 220, 700, 1500, 3000, 6000, 10000, 17000, 25000, 41000
};

} // namespace mz::config
