#pragma once

// Primitive di smoothing condivise da DSP, controllo e render.
//
// Regola valida per tutto il file: i filtri si tarano in SECONDI, non in alfa.
// Un alfa è legato al rate a cui viene applicato, e qui i rate sono tre diversi
// (5.3 Hz il controllo, 60+ Hz il render, variabile il replay): la stessa
// costante di tempo dà lo stesso comportamento su tutti e tre.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mz::util {

constexpr double clamp01(double x) noexcept {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

/**
 * Rampa liscia 0->1 su [0,1], con derivata nulla ai due estremi.
 * È ciò che sostituisce le soglie nette nella legge di controllo: stesso
 * significato, senza il gradino che si vedeva come scatto.
 */
constexpr double smoothstep01(double x) noexcept {
    const double t = clamp01(x);
    return t * t * (3.0 - 2.0 * t);
}

/**
 * Coefficiente EMA equivalente a una costante di tempo `tau` su un passo `dt`.
 * dt <= 0 -> 0 (nessun avanzamento), tau <= 0 -> 1 (nessun filtro).
 */
inline double emaAlpha(double dt, double tau) noexcept {
    if (dt <= 0.0)  return 0.0;
    if (tau <= 0.0) return 1.0;
    return 1.0 - std::exp(-dt / tau);
}

/** EMA a un polo tarato in secondi. Si inizializza col primo campione. */
class Ema {
public:
    double push(double x, double dt, double tau) noexcept {
        if (!init_) {
            value_ = x;
            init_  = true;
            return value_;
        }
        value_ += (x - value_) * emaAlpha(dt, tau);
        return value_;
    }

    /** Decadimento verso zero, per quando il segnale sorgente manca. */
    double decay(double dt, double tau) noexcept {
        if (!init_) return 0.0;
        value_ -= value_ * emaAlpha(dt, tau);
        return value_;
    }

    double value() const noexcept { return init_ ? value_ : 0.0; }
    bool   initialized() const noexcept { return init_; }
    void   reset() noexcept { value_ = 0.0; init_ = false; }
    void   set(double v) noexcept { value_ = v; init_ = true; }

private:
    double value_ = 0.0;
    bool   init_  = false;
};

/**
 * Due poli in cascata: l'uscita non ha spigoli, a differenza del polo singolo
 * che riproduce ogni scalino dell'ingresso attenuato ma ancora spigoloso.
 * Ogni stadio prende tau/2, così il tempo di salita complessivo resta
 * confrontabile con quello di un polo singolo di costante tau.
 */
class TwoPoleEma {
public:
    double push(double x, double dt, double tau) noexcept {
        const double half = tau * 0.5;
        return b_.push(a_.push(x, dt, half), dt, half);
    }

    double value() const noexcept { return b_.value(); }
    bool   initialized() const noexcept { return b_.initialized(); }
    void   reset() noexcept { a_.reset(); b_.reset(); }

private:
    Ema a_, b_;
};

/**
 * Mediana scorrevole a N campioni (N dispari).
 *
 * Va PRIMA dell'EMA, non dopo: un EMA attenua un campione anomalo ma lo spalma
 * su tutta la sua coda, mentre la mediana lo elimina del tutto. Costa una
 * latenza di (N-1)/2 campioni.
 */
template <std::size_t N>
class MedianFilter {
    static_assert(N % 2 == 1, "la mediana vuole un numero dispari di campioni");

public:
    double push(double x) noexcept {
        buf_[write_] = x;
        write_ = (write_ + 1) % N;
        if (count_ < N) ++count_;

        std::array<double, N> scratch{};
        std::copy_n(buf_.begin(), count_, scratch.begin());
        const auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(count_ / 2);
        std::nth_element(scratch.begin(), mid,
                         scratch.begin() + static_cast<std::ptrdiff_t>(count_));
        return *mid;
    }

    void reset() noexcept {
        buf_.fill(0.0);
        write_ = 0;
        count_ = 0;
    }

    std::size_t count() const noexcept { return count_; }

private:
    std::array<double, N> buf_{};
    std::size_t write_ = 0;
    std::size_t count_ = 0;
};

} // namespace mz::util
