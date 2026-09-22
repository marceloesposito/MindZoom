#pragma once

// Banda di controllo ADATTIVA: alternativa alla calibrazione a due fasi.
//
// La calibrazione classica misura due estremi una volta sola e li fissa per
// sempre. Misurato sul campo (quattro sessioni con fascia reale, 26-28/08) quel
// modo ha tre difetti che si presentano insieme:
//
//   1. il neutro finisce in un punto arbitrario della distribuzione - al 19°,
//      al 21°, al 47° e al 100° percentile nelle quattro sessioni;
//   2. il difetto ha un verso sistematico: la fase piu' LUNGA produce
//      l'estremo piu' estremo, e Relax dura sempre piu' di Concentrazione
//      (chiude a bordo di ciclo, quasi sempre in timeout a 45s, contro i
//      ~15-18s di Concentrazione). Il neutro viene quindi tirato in basso e
//      l'esperienza scivola verso lo zoom in: 78% del tempo sopra il neutro,
//      zoom out disponibile solo nel 17%;
//   3. l'indice DERIVA durante la sessione (+36% e +64% dal primo all'ultimo
//      terzo, misurato) mentre la banda resta ferma dove e' stata fissata.
//
// Qui invece la banda insegue il segnale: il neutro e' la MEDIANA corrente,
// quindi sta al centro per costruzione (niente piu' punto 1 e 2), e la
// finestra mobile segue la deriva invece di subirla (punto 3). In cambio si
// perde la misura garantita degli estremi personali prima di cominciare - ma
// quella misura, alla prova dei fatti, non era garantita affatto.
//
// NON contiene la legge di controllo: produce solo gli estremi, che vengono
// dati a Calibration::adoptBand(). La legge resta una sola, in un solo posto.

#include "config.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace mz::control {

/**
 * Percentili correnti dell'indice condizionato, su una finestra mobile.
 *
 * Ring buffer + nth_element a richiesta: con qualche centinaio di campioni e
 * un ricalcolo ogni kRecomputeEvery campioni il costo e' trascurabile, e in
 * cambio i percentili sono ESATTI invece che stimati in modo incrementale -
 * che su una distribuzione con code pesanti come questa (l'indice di Pope e'
 * un rapporto fra potenze) sarebbe stato il posto sbagliato dove risparmiare.
 */
class AdaptiveBand {
public:
    /** Registra un campione dell'indice condizionato. */
    void push(double c) {
        if (buffer_.size() < capacity()) {
            buffer_.push_back(c);
        } else {
            buffer_[write_] = c;
            write_ = (write_ + 1) % capacity();
        }
        ++seen_;
        if (--untilRecompute_ <= 0) recompute();
    }

    /**
     * true quando c'e' abbastanza segnale per fidarsi dei percentili. Prima
     * di allora il controllo resta fermo: guidare su una banda stimata su
     * pochi campioni sarebbe peggio che non guidare affatto.
     */
    bool ready() const noexcept { return ready_; }

    /** Frazione [0,1] del riscaldamento gia' fatta: serve solo a mostrarla. */
    double warmupProgress() const noexcept {
        const double serve = config::kAdaptiveWarmupS * config::kControlHz;
        return std::min(1.0, static_cast<double>(seen_) / serve);
    }

    double lo() const noexcept { return lo_; }
    double hi() const noexcept { return hi_; }
    double mid() const noexcept { return mid_; }

    void reset() {
        buffer_.clear();
        write_ = 0;
        seen_  = 0;
        untilRecompute_ = 1;
        ready_ = false;
        lo_ = hi_ = mid_ = 0.0;
    }

    /** Campioni nella finestra: diagnostica. */
    std::size_t size() const noexcept { return buffer_.size(); }

private:
    static constexpr int kRecomputeEvery = 8;   // ~1.5 volte al secondo a kControlHz

    static std::size_t capacity() {
        return static_cast<std::size_t>(config::kAdaptiveWindowS * config::kControlHz);
    }

    void recompute() {
        untilRecompute_ = kRecomputeEvery;

        const auto serve = static_cast<std::size_t>(config::kAdaptiveWarmupS * config::kControlHz);
        if (seen_ < serve) return;   // ancora in riscaldamento: nessuna banda

        scratch_ = buffer_;
        const auto n = scratch_.size();
        const auto at = [&](double q) {
            auto k = static_cast<std::size_t>(q * static_cast<double>(n - 1));
            std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(k),
                             scratch_.end());
            return scratch_[k];
        };

        // L'ordine conta: nth_element riordina, quindi si va dal percentile
        // piu' basso al piu' alto e ogni chiamata lavora su un vettore gia'
        // parzialmente ordinato a proprio favore.
        const double lo  = at(config::kAdaptiveLoPercentile);
        const double mid = at(0.5);
        const double hi  = at(config::kAdaptiveHiPercentile);

        // Una banda degenere (segnale piatto) non e' utilizzabile: meglio
        // restare "non pronti" che dare al controllo una span nulla, su cui
        // dividerebbe per zero o saturerebbe a ogni campione.
        if (!(hi > lo)) { ready_ = false; return; }

        lo_ = lo; mid_ = mid; hi_ = hi;
        ready_ = true;
    }

    std::vector<double> buffer_;
    std::vector<double> scratch_;
    std::size_t         write_ = 0;
    std::size_t         seen_  = 0;
    int                 untilRecompute_ = 1;
    bool                ready_ = false;
    double              lo_ = 0.0, hi_ = 0.0, mid_ = 0.0;
};

} // namespace mz::control
