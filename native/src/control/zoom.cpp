#include "control/zoom.hpp"

#include <algorithm>
#include <cmath>

namespace mz::control {

const char* toString(Phase phase) noexcept {
    switch (phase) {
        case Phase::Idle:        return "IDLE";
        case Phase::Onboarding:  return "ONBOARDING";
        case Phase::Hook:        return "HOOK";
        case Phase::Handover:    return "HANDOVER";
        case Phase::Interactive: return "INTERACTIVE";
        case Phase::Outro:       return "OUTRO";
        case Phase::Done:        return "DONE";
    }
    return "?";
}

void ZoomController::reset() {
    targetFocus_  = 0.0;
    currentFocus_ = 0.0;
    locked_       = false;
    lockedLevel_  = 0.0;
    lockTimer_    = 0.0;
}

double ZoomController::authorityVelocity(double inputVelocity, Phase phase,
                                         double phaseElapsed) const {
    switch (phase) {
        case Phase::Onboarding:
            return 0.0;                          // fermo in superficie
        case Phase::Hook:
            return config::kHookAutoVelocity;    // auto-zoom scriptato, l'EEG osserva
        case Phase::Handover: {
            // Crossfade di autorità: evita lo scatto quando l'input prende il comando.
            const double t = std::min(1.0, phaseElapsed / config::kPhaseHandoverS);
            return config::kHookAutoVelocity * (1.0 - t) + inputVelocity * t;
        }
        case Phase::Interactive:
            return inputVelocity;
        default:
            return 0.0;
    }
}

double ZoomController::applyHoldSelect(double velocity, double dt) {
    const double absV    = std::fabs(velocity);
    const double step    = 1.0 / (config::kTotalImages - 1);
    const double nearest = std::round(targetFocus_ / step) * step;

    // Le soglie sono FRAZIONI della velocità massima che l'input può produrre,
    // non valori assoluti: così cambiare il gain non rende un detent inescapabile.
    constexpr double maxInput = config::kExtremaGain;
    constexpr double enterHold     = config::kEnterHoldFrac * maxInput;
    constexpr double snapThreshold = config::kSnapVelFrac * maxInput;

    // Dwell-to-lock: dopo kLockDwellS su un livello serve uno sforzo maggiore per
    // uscirne, così l'utente può rilassarsi del tutto senza scivolare. Il clamp
    // garantisce che un detent resti sempre sganciabile.
    const double rawBreak = (lockTimer_ >= config::kLockDwellS)
        ? config::kBreakHoldFrac * maxInput * config::kLockDwellMult
        : config::kBreakHoldFrac * maxInput;
    const double breakThreshold = std::min(rawBreak, maxInput * 0.95);

    if (locked_) {
        if (absV >= breakThreshold) {
            locked_    = false;
            lockTimer_ = 0.0;
            return velocity;
        }
        // Agganciato: attrattore verso il livello, nessuna deriva.
        lockTimer_ += dt;
        targetFocus_ += (lockedLevel_ - targetFocus_) * config::kSnapStrength;
        return 0.0;
    }

    if (absV < enterHold) {
        locked_      = true;
        lockedLevel_ = nearest;
        lockTimer_   = 0.0;
        return 0.0;
    }

    if (absV < snapThreshold) {
        targetFocus_ += (nearest - targetFocus_) * config::kSnapStrength;
    }

    return velocity;
}

void ZoomController::update(double inputVelocity, double dt, Phase phase,
                            double phaseElapsed) {
    if (phase == Phase::Outro || phase == Phase::Done) {
        // Conclusione scriptata: garanzia della FSM, non una speranza sull'EEG.
        targetFocus_ += (config::kOutroTargetFocus - targetFocus_) * config::kOutroEasing;
    } else {
        double velocity = authorityVelocity(inputVelocity, phase, phaseElapsed);
        if (phase == Phase::Interactive) {
            velocity = applyHoldSelect(velocity, dt);
        }
        targetFocus_ += velocity * config::kZoomSpeedFactor;
    }

    targetFocus_  = std::clamp(targetFocus_, 0.0, 1.0);
    currentFocus_ += (targetFocus_ - currentFocus_) * config::kFocusEasing;
}

CrossfadeState ZoomController::crossfade() const {
    CrossfadeState s;

    const double raw = currentFocus_ * (config::kTotalImages - 1);
    s.activeIndex = std::min(static_cast<int>(std::floor(raw)), config::kTotalImages - 2);
    s.progress    = raw - s.activeIndex;

    s.activeAlpha = 1.0 - s.progress;
    s.nextAlpha   = s.progress;
    s.activeScale = 1.0 + s.progress * 0.5;
    s.nextScale   = 0.66 + s.progress * 0.34;

    const auto lo = static_cast<std::size_t>(s.activeIndex);
    const double lower = config::kScaleLabels[lo];
    const double upper = config::kScaleLabels[lo + 1];
    s.magnification = static_cast<int>(std::lround(lower + (upper - lower) * s.progress));

    return s;
}

} // namespace mz::control
