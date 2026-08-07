#pragma once

// Rate control + hold/select + crossfade. Port 1:1 di app.js
// (resolveAuthorityVelocity, applyHoldSelect, il blocco di accumulo nel ticker).
//
// VINCOLO: la matematica base del rate control non si tocca. Del port cambia
// solo la SORGENTE della velocità (controllo a estremi), non l'integratore.

#include "config.hpp"
#include "control/tunables.hpp"

namespace mz::control {

/** Fasi dell'esperienza. */
enum class Phase { Idle, Onboarding, Hook, Handover, Interactive, Outro, Done };

const char* toString(Phase phase) noexcept;

/** Stato di rendering derivato dal focus corrente: due sprite in crossfade. */
struct CrossfadeState {
    int    activeIndex = 0;   // sprite inferiore
    double progress    = 0.0; // [0,1) verso lo sprite successivo
    double activeAlpha = 1.0;
    double nextAlpha   = 0.0;
    double activeScale = 1.0;
    double nextScale   = 0.66;
    int    magnification = config::kScaleLabels[0];
};

/**
 * Integratore dello zoom.
 *
 * Il controllo aggiorna a ~5.3 Hz mentre il render gira a 60: l'easing su
 * currentFocus è ciò che rende fluida la differenza, non un filtro sulla
 * velocità.
 */
class ZoomController {
public:
    /**
     * Un passo di rendering.
     * @param inputVelocity velocità dal controllo a estremi (già gated)
     * @param dt            passo temporale del frame
     * @param phase         fase corrente: decide chi ha autorità
     * @param phaseElapsed  tempo nella fase, serve al crossfade di autorità
     */
    void update(double inputVelocity, double dt, Phase phase, double phaseElapsed,
                const Tunables& t);

    /** Risolve chi comanda lo zoom nella fase corrente. */
    double authorityVelocity(double inputVelocity, Phase phase, double phaseElapsed) const;

    /**
     * Dead-zone + detent + isteresi + dwell-to-lock. Solo in fase interattiva.
     * @return la velocità che sopravvive al gating del detent.
     *
     * L'aggancio richiede PERMANENZA sotto soglia e lo sgancio apre una finestra
     * REFRATTARIA. Senza queste due, un controllo che passa per zero fra un
     * impulso e l'altro si riagganciava ad ogni frame: si otteneva un frame di
     * movimento per volta, che è come non muoversi affatto.
     */
    double applyHoldSelect(double velocity, double dt, const Tunables& t);

    CrossfadeState crossfade() const;

    double targetFocus() const noexcept { return targetFocus_; }
    double currentFocus() const noexcept { return currentFocus_; }
    bool   locked() const noexcept { return locked_; }
    double lockTimer() const noexcept { return lockTimer_; }
    /** Tempo residuo di permanenza prima dell'aggancio, 0 se non si sta agganciando. */
    double lockPending() const noexcept { return quietTimer_; }
    /** Tempo residuo di refrattarietà dopo uno sgancio. */
    double refractory() const noexcept { return refractory_; }

    void reset();

private:
    double targetFocus_  = 0.0;
    double currentFocus_ = 0.0;
    bool   locked_       = false;
    double lockedLevel_  = 0.0;
    double lockTimer_    = 0.0;
    double quietTimer_   = 0.0;   // da quanto la velocità sta sotto enterHold
    double refractory_   = 0.0;   // quanto manca alla riabilitazione del lock
};

} // namespace mz::control
