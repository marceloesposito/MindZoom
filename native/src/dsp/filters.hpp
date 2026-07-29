#pragma once

// Catena di filtraggio per campione, port 1:1 di MuseBluetooth.js.
// Ordine: rimozione DC -> notch -> passa-basso.

#include "config.hpp"

namespace mz::dsp {

/**
 * Rimozione della componente continua con predittore a inseguimento lento.
 * Si inizializza col primo campione: partendo da 0 impiegherebbe ~0.5 s a
 * raggiungere l'offset reale dell'ADC e in quel transitorio il segnale sembra
 * enorme, facendolo scartare dal gating d'ampiezza.
 */
class DcRemover {
public:
    double process(double uv) noexcept {
        if (!initialized_) {
            predictor_   = uv;
            initialized_ = true;
        }
        const double filtered = uv - predictor_;
        predictor_ += config::kDcLeak * filtered;
        return filtered;
    }

    void reset() noexcept {
        predictor_   = 0.0;
        initialized_ = false;
    }

private:
    double predictor_   = 0.0;
    bool   initialized_ = false;
};

/** Biquad in forma diretta I: y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2. */
class Biquad {
public:
    constexpr Biquad(double b0, double b1, double b2, double a1, double a2) noexcept
        : b0_(b0), b1_(b1), b2_(b2), a1_(a1), a2_(a2) {}

    double process(double x) noexcept {
        const double y = (b0_ * x) + (b1_ * x1_) + (b2_ * x2_) - (a1_ * y1_) - (a2_ * y2_);
        x2_ = x1_; x1_ = x;
        y2_ = y1_; y1_ = y;
        return y;
    }

    void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0.0; }

private:
    double b0_, b1_, b2_, a1_, a2_;
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

inline Biquad makeNotch() noexcept {
    return Biquad(config::kNotchB0, config::kNotchB1, config::kNotchB2,
                  config::kNotchA1, config::kNotchA2);
}

inline Biquad makeLowpass() noexcept {
    return Biquad(config::kLpB0, config::kLpB1, config::kLpB2,
                  config::kLpA1, config::kLpA2);
}

} // namespace mz::dsp
