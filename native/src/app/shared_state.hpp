#pragma once

// Stato condiviso fra il thread DSP (produttore) e il thread di render
// (consumatore). Deve restare banalmente copiabile: viene pubblicato con un
// double buffer atomico, quindi niente std::string e niente puntatori.

#include <cstdint>

namespace mz::app {

/** Comandi dalla UI al thread DSP. Uno alla volta, via atomica. */
enum class Command : int {
    None = 0,
    StartCalibration,
    RetryCalibration,
    // Riparte da zero: cambiata la sorgente del segnale, gli estremi calibrati
    // su un'altra sessione non valgono più.
    RestartSession
};

/** Motivo di fallimento della calibrazione, per scegliere il testo da mostrare. */
enum class FailReason : int {
    None = 0,
    NoSignal,
    WeakModulation,
    // Il segnale ricevuto non è EEG: nessuna calibrazione su questi dati
    // significherebbe qualcosa, per quanto i numeri possano sembrare plausibili.
    ImplausibleSignal
};

struct ControlState {
    // --- fase ---
    int    phase        = 0;    // control::Phase
    double phaseElapsed = 0.0;

    // --- calibrazione ---
    int    calibStage         = 0;    // control::CalibStage
    double calibDisplayTarget = 0.5;  // altezza normalizzata del quadratino
    double calibRemaining     = 0.0;  // secondi al termine della fase
    bool   calibValid         = false;
    int    failReason         = 0;

    // --- banda di controllo ---
    double absMin   = 0.0;
    double absMax   = 0.0;
    double neutral  = 0.0;
    double localMin = 0.0;
    double localMax = 0.0;

    // --- segnale ---
    double rawIndex      = 0.0;
    double smoothedIndex = 0.0;
    double velocity      = 0.0;   // dopo lo smoothing: è quella che guida lo zoom
    double velocityRaw   = 0.0;   // uscita cruda della legge di controllo
    // Le due componenti della velocità. Separano "sono lontano dal neutro ma la
    // banda non lascia passare" (gate basso) da "la banda lascia passare ma sono
    // vicino al neutro" (ampiezza bassa): richiedono correzioni opposte.
    double gate      = 0.0;
    double magnitude = 0.0;
    double theta = 0.0, alpha = 0.0, beta = 0.0;
    double maxAbsRaw = 0.0;
    bool   contactOk = true;
    bool   artifact  = false;

    // --- plausibilità fisica: quello che arriva è un segnale biologico? ---
    int    signalFault  = 0;    // dsp::SignalFault
    double autocorr1    = 1.0;
    double railFraction = 0.0;
    double spreadCounts = 0.0;
    // Un frame è arrivato di recente. Distingue "fascia storta" (contatto scarso
    // ma dati presenti) da "fascia assente" (nessun dato): sono due messaggi
    // diversi da dare all'utente.
    bool   signalFresh = false;

    // --- flusso ---
    int           bleState      = 0;   // ble::State
    int           packetLen     = 0;
    int           packetSamples = 0;
    std::uint64_t rawPackets    = 0;   // notifiche EEG ricevute, prima dei filtri
    std::uint64_t validPackets  = 0;   // quelle riconosciute come pacchetti EEG
    std::uint64_t frames        = 0;
    std::uint64_t droppedSamples = 0;
    bool          stalled       = false;

    // --- registrazione / riproduzione ---
    bool          replaying     = false;  // il segnale viene da un file, non dalla fascia
    bool          recording     = false;
    std::uint64_t recordedSamples = 0;
};

} // namespace mz::app
