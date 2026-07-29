#pragma once

// Calibrazione attiva (scheda col quadratino sul binario) + legge di controllo
// a estremi. Port 1:1 di app.js: updateSmoothedIndex, updateCalibrationSample,
// finalizeActiveCalibration, computeExtremaVelocity.
//
// Modello: la calibrazione fissa DUE ESTREMI ASSOLUTI personali. Il neutro M è
// la loro media: sopra = concentrazione (zoom in), sotto = distrazione (zoom
// out). Una banda LOCALE di isteresi rende il controllo fasico. Niente
// moving-average, niente ring buffer percentile: l'interazione parte subito.

#include "config.hpp"

#include <limits>
#include <string>

namespace mz::control {

/** Sotto-stati della scheda di calibrazione, interni alla fase di onboarding. */
enum class CalibStage {
    Intro,        // schermata introduttiva, attende il via
    Concentrate,  // l'utente spinge il quadratino in alto (registra absMax)
    Relax,        // lo lascia scendere (registra absMin)
    Done,         // riuscita: breve conferma, poi si passa all'interazione
    Failed        // segnale assente o modulazione troppo debole -> Riprova
};

const char* toString(CalibStage stage) noexcept;

/** EMA di denoise sull'indice di Pope. NON è una normalizzazione a finestra. */
class IndexSmoother {
public:
    /** Inizializza col primo campione, così non c'è transitorio di partenza. */
    double push(double index) noexcept;
    double value() const noexcept { return value_; }
    bool   initialized() const noexcept { return init_; }
    void   reset() noexcept { value_ = 0.0; init_ = false; }

private:
    double value_ = 0.0;
    bool   init_  = false;
};

/**
 * Stato della calibrazione e della banda di controllo che ne deriva.
 *
 * Uso tipico:
 *   start();                            // -> Concentrate
 *   ogni frame: tick(dt, contactOk)     // avanza le fasi (in pausa se contatto ko)
 *   ogni campione di controllo: sample(c)
 *   quando stage()==Done: velocity(c, dt) guida lo zoom
 */
class Calibration {
public:
    void start();                       // reset completo -> fase Concentrate
    void enterStage(CalibStage stage);  // reset degli accumulatori di fase

    /** Registra un campione dell'indice smussato nella fase corrente. */
    void sample(double c);

    /**
     * Avanza il tempo di fase. Il conteggio si ferma se il contatto è scarso,
     * così una fascia mal posizionata non consuma la calibrazione.
     * @return true se la fase è cambiata in questo tick.
     */
    bool tick(double dt, bool contactOk);

    /** Chiude la calibrazione: valida la span e fissa gli estremi assoluti. */
    void finalize();

    /**
     * Velocità di zoom dalla concentrazione `c` relativa agli estremi.
     * Positiva = zoom in, negativa = zoom out, 0 = fermo (banda locale).
     * Torna 0 finché la calibrazione non è valida.
     */
    double velocity(double c, double dt);

    CalibStage stage() const noexcept { return stage_; }
    bool   valid() const noexcept { return valid_; }
    double absMax() const noexcept { return absMax_; }
    double absMin() const noexcept { return absMin_; }
    double neutral() const noexcept { return neutralM_; }
    double localMax() const noexcept { return localMax_; }
    double localMin() const noexcept { return localMin_; }
    double stageElapsed() const noexcept { return stageElapsed_; }
    const std::string& message() const noexcept { return message_; }

    /** Altezza normalizzata [0,1] del quadratino: display auto-scalato sulla fase. */
    double displayTarget() const noexcept { return displayTarget_; }

    /** Durata della fase corrente, 0 se non è una fase a tempo. */
    double stageDuration() const noexcept;

private:
    void fail(std::string reason);

    CalibStage stage_ = CalibStage::Intro;
    double stageElapsed_ = 0.0;
    double doneTimer_    = 0.0;

    // Estremi della fase corrente, per l'auto-scala del display.
    double runMin_ =  std::numeric_limits<double>::infinity();
    double runMax_ = -std::numeric_limits<double>::infinity();
    double displayTarget_ = 0.5;

    // Estremi assoluti registrati (dopo il lead-in).
    double peak_   = -std::numeric_limits<double>::infinity();
    double trough_ =  std::numeric_limits<double>::infinity();

    // Banda di controllo derivata.
    bool   valid_   = false;
    double absMax_  = 0.0;
    double absMin_  = 0.0;
    double neutralM_ = 0.0;
    double localMax_ = 0.0;
    double localMin_ = 0.0;

    std::string message_;
};

} // namespace mz::control
