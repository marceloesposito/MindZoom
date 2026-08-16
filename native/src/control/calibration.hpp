#pragma once

// Calibrazione attiva (scheda col quadratino sul binario) + legge di controllo
// a estremi. Specchio di app.js: updateSmoothedIndex, updateCalibrationSample,
// finalizeActiveCalibration, computeExtremaVelocity.
//
// Modello: la calibrazione fissa DUE ESTREMI ASSOLUTI personali. Il neutro M è
// la loro media: sopra = concentrazione (zoom in), sotto = distrazione (zoom
// out). Una banda LOCALE di isteresi rende il controllo fasico. Niente
// moving-average, niente ring buffer percentile: l'interazione parte subito.
//
// La banda locale ha una TOLLERANZA: il suo bordo non è una soglia netta ma una
// rampa. Con un bordo netto la velocità collassava a zero ogni volta che
// l'indice scendeva — cioè circa metà del tempo — e l'uscita era un segnale
// commutato a 5.3 Hz invece di un controllo.

#include "config.hpp"
#include "control/tunables.hpp"
#include "util/smoothing.hpp"

#include <cstddef>
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

/**
 * Denoise dell'indice di Pope. NON è una normalizzazione a finestra.
 *
 * Due stadi: mediana (toglie i campioni anomali, che in un rapporto fra potenze
 * di banda sono frequenti) e poi due poli in cascata (smussa senza lasciare
 * spigoli). La calibrazione e l'esercizio usano lo STESSO `c` condizionato, così
 * gli estremi misurati e il segnale su cui si guida restano coerenti.
 */
class IndexSmoother {
public:
    /** Inizializza col primo campione, così non c'è transitorio di partenza. */
    double push(double index, double dt) noexcept;
    double value() const noexcept { return ema_.value(); }
    bool   initialized() const noexcept { return ema_.initialized(); }
    void   reset() noexcept { median_.reset(); ema_.reset(); }

private:
    util::MedianFilter<static_cast<std::size_t>(config::kIndexMedianTaps)> median_;
    util::TwoPoleEma ema_;
};

/**
 * Statistiche correnti di una fase, aggiornate campione per campione.
 *
 * Tiene anche la correlazione a ritardo 1, perche' senza di quella il conteggio
 * dei campioni mente: l'indice e' filtrato, quindi campioni consecutivi non
 * sono informazione indipendente e contarli tutti sopravvaluta di molto quanto
 * si e' misurato.
 */
struct PhaseStats {
    int    n    = 0;
    double sum  = 0.0;
    double sumSq = 0.0;
    double sumProd = 0.0;   // somma dei prodotti fra campioni consecutivi
    double last = 0.0;
    bool   hasLast = false;

    void reset() noexcept { *this = PhaseStats{}; }
    void push(double x) noexcept;

    double mean() const noexcept;
    double variance() const noexcept;
    /** Correlazione a ritardo 1, limitata a [0, 0.99]. */
    double autocorr1() const noexcept;
    /** Campioni INDIPENDENTI equivalenti: n * (1-rho)/(1+rho). */
    double effectiveN() const noexcept;
};

/**
 * Separazione fra due fasi in unita' di errore standard (t di Welch).
 * Zero se una delle due non ha abbastanza campioni.
 */
double separationT(const PhaseStats& a, const PhaseStats& b) noexcept;

/**
 * Stato della calibrazione e della banda di controllo che ne deriva.
 *
 * NON e' a tempo: ogni fase finisce quando ha raccolto abbastanza campioni
 * indipendenti, e la seconda anche quando le due fasi sono separate in modo
 * statisticamente netto. Il tempo di orologio non dice niente su quanta
 * informazione e' stata raccolta - con la fascia che perde contatto quindici
 * secondi possono contenere due campioni utili o ottanta.
 *
 * Uso tipico:
 *   start();                            // -> Concentrate
 *   ogni frame: tick(dt, usable)        // avanza le fasi, misura il tempo utile
 *   ogni campione di controllo: sample(c, usable)
 *   quando stage()==Done: velocity(c, dt) guida lo zoom
 */
class Calibration {
public:
    void start();                       // reset completo -> fase Concentrate
    void enterStage(CalibStage stage);  // reset degli accumulatori di fase

    /**
     * Registra un campione dell'indice smussato nella fase corrente.
     * @param usable false se il segnale non è utilizzabile: il campione viene
     *        scartato invece di inquinare la statistica. È la sostituzione
     *        della vecchia "pausa del conteggio": non si mette in pausa un
     *        orologio, semplicemente non si conta ciò che non vale.
     */
    void sample(double c, bool usable = true);

    /**
     * Avanza il tempo. Serve solo per il tempo di conferma della fase Done e per
     * la rete di sicurezza sul tempo massimo di fase; l'avanzamento delle fasi
     * dipende dai campioni, non da qui.
     * @return true se la fase è cambiata in questo tick.
     */
    bool tick(double dt, bool usable);

    /** Chiude la calibrazione: valida la span e fissa gli estremi assoluti. */
    void finalize();

    /**
     * Velocità di zoom dalla concentrazione `c` relativa agli estremi.
     * Positiva = zoom in, negativa = zoom out, 0 = fermo.
     * Torna 0 finché la calibrazione non è valida.
     *
     * Forma: AMPIEZZA x GATE, entrambi continui.
     *  - ampiezza: distanza dal neutro in unità personali, con zona morta al
     *    centro e rampa liscia fino alla saturazione a `concFraction`;
     *  - gate: quanto si è vicini al proprio estremo recente, su una rampa
     *    larga `localTolerance` invece che su un gradino.
     */
    double velocity(double c, double dt, const Tunables& t);

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

    /**
     * Il quadratino va mostrato? Solo in concentrazione.
     *
     * Nel rilassamento un indicatore che si muove è controproducente: dà un
     * compito, e guardare come sta andando il proprio rilassamento è essa
     * stessa un'attività attenzionale. Si misura peggio proprio ciò che si
     * vuole misurare. In quella fase resta solo la barra di avanzamento.
     */
    bool showTarget() const noexcept { return stage_ == CalibStage::Concentrate; }

    /**
     * Quanto manca alla fine della fase, in [0,1].
     *
     * È il minimo fra i criteri attivi, cioè quello che sta effettivamente
     * frenando: una barra che mostra il criterio più avanzato arriverebbe in
     * fondo e poi resterebbe lì, che è il modo peggiore di far aspettare.
     */
    double progress() const noexcept;

    /** Campioni indipendenti raccolti nella fase corrente. */
    double effectiveSamples() const noexcept;

    /** Separazione fra le due fasi, in errori standard. 0 durante la prima. */
    double separation() const noexcept;

    // Le due componenti dell'ultima velocità calcolata. Servono al misuratore di
    // attivazione: separano "sono lontano dal neutro ma la banda non lascia
    // passare" da "la banda lascia passare ma sono vicino al neutro", che
    // richiedono due correzioni opposte.
    double gate() const noexcept { return lastGate_; }
    double magnitude() const noexcept { return lastMag_; }

private:
    void fail(std::string reason);

    CalibStage stage_ = CalibStage::Intro;
    double stageElapsed_ = 0.0;   // tempo di segnale UTILE nella fase (rete di sicurezza)
    double doneTimer_    = 0.0;

    PhaseStats concStats_;
    PhaseStats relaxStats_;
    int        phaseSamples_ = 0;   // conteggio grezzo, per il lead-in

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

    // Diagnostica dell'ultimo calcolo di velocity().
    double lastGate_ = 0.0;
    double lastMag_  = 0.0;

    std::string message_;
};

} // namespace mz::control
