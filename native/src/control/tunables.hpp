#pragma once

// Manopole regolabili a programma aperto.
//
// Perché esistono: la legge di controllo si giudica solo con la fascia in testa,
// e ricompilare fra un tentativo e l'altro rende il confronto inutilizzabile —
// quando riprovi non ricordi più com'era. Questi valori si muovono coi tasti,
// si leggono nel pannello H e partono dai default di config.hpp, a cui il tasto
// R li riporta.
//
// Deve restare banalmente copiabile: viene pubblicato con util::DoubleBuffer.

#include "config.hpp"

#include <algorithm>

namespace mz::control {

struct Tunables {
    // --- regolabili dai tasti ---
    double sensitivity    = 1.0;                        // moltiplicatore su kExtremaGain
    double localTolerance = config::kLocalTolerance;    // ampiezza della rampa di attivazione
    double velTauS        = config::kVelTauS;           // smoothing a valle del controllo
    bool   holdEnabled    = true;                       // hold/select attivo

    // --- fissi, ma passati insieme agli altri per non spargere config:: nella
    //     legge di controllo e per poterli sostituire nei test ---
    double localDecay   = config::kLocalDecay;
    double deadzone     = config::kNeutralDeadzone;
    double concFraction = config::kCalibConcFraction;

    /** Velocità normalizzata massima, dopo la sensibilità. */
    double gain() const noexcept { return config::kExtremaGain * sensitivity; }

    void adjustSensitivity(int steps) noexcept {
        sensitivity = std::clamp(sensitivity + 0.1 * steps, 0.5, 2.5);
    }
    void adjustTolerance(int steps) noexcept {
        localTolerance = std::clamp(localTolerance + 0.02 * steps, 0.02, 0.40);
    }
    void adjustSmoothing(int steps) noexcept {
        velTauS = std::clamp(velTauS + 0.05 * steps, 0.10, 1.00);
    }
    void reset() noexcept { *this = Tunables{}; }
};

} // namespace mz::control
