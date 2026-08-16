#include "dsp/gating.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mz::dsp {
namespace {

/**
 * Statistiche di plausibilità su una finestra di ADC grezzi.
 *
 * L'autocorrelazione a un campione è la misura che conta: a 256 Hz due campioni
 * consecutivi distano 4 ms e l'EEG, essendo passa-banda, li rende quasi
 * identici. Se vale zero, quello che si sta leggendo non è una forma d'onda -
 * e nessun'altra statistica se ne accorge, perché media e ampiezza di byte
 * casuali possono benissimo sembrare plausibili.
 */
struct RawStats {
    double railFraction  = 0.0;
    double autocorr1     = 1.0;
    double spread        = 0.0;
    double mainsFraction = 0.0;
};

/**
 * Potenza alla frequenza del bin k, con l'algoritmo di Goertzel.
 *
 * Due comodità che rendono la cosa esatta e quasi gratuita: con finestra 256 a
 * 256 Hz un bin vale esattamente 1 Hz, quindi i 50 Hz cadono su un bin intero
 * senza dispersione; e il buffer è un ring che non serve riordinare, perché una
 * rotazione ciclica cambia la fase di un bin esatto ma non il suo modulo.
 */
double goertzelPower(const float* x, int n, int k) {
    const double w     = 2.0 * 3.14159265358979323846 * k / n;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = static_cast<double>(x[static_cast<std::size_t>(i)]) +
                          coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/**
 * Quota di potenza attorno alla frequenza di rete, su segnale con la sola DC
 * rimossa (sul filtrato non si vedrebbe: il notch l'ha già tolta).
 *
 * Si sommano i bin 49-51 invece del solo 50: la finestra non è apodizzata e uno
 * scarto minimo della frequenza di rete disperderebbe energia sui vicini.
 */
double mainsFraction(const float* dcFree, int n, double fs) {
    if (n < 8) return 0.0;

    double total = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = dcFree[static_cast<std::size_t>(i)];
        total += v * v;
    }
    if (total <= 1e-12) return 0.0;

    const int centre = static_cast<int>((config::kMainsHz * n / fs) + 0.5);
    double    mains  = 0.0;
    for (int k = centre - 1; k <= centre + 1; ++k) {
        if (k > 0 && k < n / 2) mains += 2.0 * goertzelPower(dcFree, n, k) / n;
    }
    return std::min(1.0, mains / total);
}

RawStats rawStats(const float* adc, int n) {
    RawStats r;
    if (n < 2) return r;

    double sum = 0.0;
    int    rail = 0;
    for (int i = 0; i < n; ++i) {
        const double v = adc[static_cast<std::size_t>(i)];
        sum += v;
        // Fondo scala a 14 bit, con un margine: un campione a 3 conteggi dal
        // limite è saturo quanto uno esattamente al limite.
        if (v <= 20.0 || v >= 16363.0) ++rail;
    }
    const double mean = sum / n;
    r.railFraction = static_cast<double>(rail) / n;

    double den = 0.0, num = 0.0;
    for (int i = 0; i < n; ++i) {
        const double d = adc[static_cast<std::size_t>(i)] - mean;
        den += d * d;
        if (i > 0) num += (adc[static_cast<std::size_t>(i - 1)] - mean) * d;
    }
    r.spread = std::sqrt(den / n);
    // Segnale costante: l'autocorrelazione non è definita. Lo cattura `spread`.
    r.autocorr1 = (den > 1e-9) ? (num / den) : 1.0;
    return r;
}

} // namespace

const char* toString(SignalFault f) noexcept {
    switch (f) {
        case SignalFault::None:         return "OK";
        case SignalFault::Mains:        return "RETE 50 Hz";
        case SignalFault::Flat:         return "PIATTO";
        case SignalFault::Railing:      return "SATURO";
        case SignalFault::Uncorrelated: return "NON UN SEGNALE";
    }
    return "?";
}

void Gating::reset() {
    peaks_.fill(0.0);
    count_    = 0;
    writeIdx_ = 0;
}

double Gating::medianPeak() const {
    if (count_ == 0) return 0.0;
    std::array<double, config::kArtifactPeakHistory> scratch{};
    std::copy_n(peaks_.begin(), count_, scratch.begin());
    const auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(count_ / 2);
    std::nth_element(scratch.begin(), mid, scratch.begin() + static_cast<std::ptrdiff_t>(count_));
    return *mid;
}

Quality Gating::assess(const SlidingStft& stft) {
    constexpr int kN = SlidingStft::kN;

    Quality q;
    double maxAbsRaw = 0.0;
    bool   contactOk = true;

    for (const int ch : {config::kFrontalA, config::kFrontalB}) {
        const float* raw  = stft.dcFree(ch);
        const float* filt = stft.filtered(ch);

        double sum = 0.0, sumSq = 0.0;
        for (int i = 0; i < kN; ++i) {
            const auto k = static_cast<std::size_t>(i);
            const double a = std::fabs(static_cast<double>(raw[k]));
            if (a > maxAbsRaw) maxAbsRaw = a;

            const double f = filt[k];
            sum   += f;
            sumSq += f * f;
        }

        const double mean     = sum / kN;
        const double variance = (sumSq / kN) - (mean * mean);
        const double stddev   = std::sqrt(variance > 0.0 ? variance : 0.0);
        if (stddev < config::kContactStdMin) contactOk = false;
    }

    const double median = medianPeak();

    // Servono almeno 8 finestre di storia perché la mediana significhi qualcosa.
    q.artifact = (count_ >= 8) &&
                 (maxAbsRaw > config::kArtifactUvFloor) &&
                 (maxAbsRaw > config::kArtifactRelMult * median);

    constexpr std::size_t kHistory = static_cast<std::size_t>(config::kArtifactPeakHistory);
    peaks_[writeIdx_] = maxAbsRaw;
    writeIdx_ = (writeIdx_ + 1) % kHistory;
    if (count_ < kHistory) ++count_;

    q.maxAbsRaw  = maxAbsRaw;
    q.medianPeak = median;
    q.contactOk  = contactOk;

    // Statistiche ADC su AF7: dicono se il decode è corretto. Valori stretti
    // attorno a ~8192 -> unsigned centrato (ok). Sparsi su tutto 0..16383 ->
    // disallineamento dei bit o del layout del pacchetto.
    const float* adc = stft.adc(config::kFrontalA);
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double v = adc[static_cast<std::size_t>(i)];
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        sum += v;
    }
    q.adcMin  = mn;
    q.adcMax  = mx;
    q.adcMean = sum / kN;

    // --- plausibilità: il segnale è biologico? ---
    // Si prende il canale MIGLIORE dei due frontali: se anche solo uno legge
    // qualcosa di sensato, il problema è di posizionamento e non di formato.
    // Bocciare sul peggiore farebbe scattare l'allarme ad ogni elettrodo storto.
    //
    // "Migliore" si decide sul GUASTO, non sulla sola autocorrelazione: un
    // canale di solo ronzio di rete ha autocorrelazione 0.33 e vincerebbe il
    // confronto contro un canale davvero collegato ma rumoroso.
    RawStats    best;
    SignalFault bestFault = SignalFault::Uncorrelated;
    bool        first     = true;

    for (const int ch : {config::kFrontalA, config::kFrontalB}) {
        const auto s = rawStats(stft.adc(ch), kN);

        SignalFault f;
        if (s.spread < config::kMinSpreadCounts) {
            f = SignalFault::Flat;                    // niente segnale: il resto non dice nulla
        } else if (s.railFraction > config::kMaxRailFraction) {
            f = SignalFault::Railing;
        } else if (s.autocorr1 < config::kMinAutocorr1) {
            f = SignalFault::Uncorrelated;
        } else {
            f = SignalFault::None;
        }

        if (first || static_cast<int>(f) < static_cast<int>(bestFault)) {
            best      = s;
            bestFault = f;
            first     = false;
        }
    }

    // Il ronzio si misura sulla DERIVAZIONE BIPOLARE, non sui canali singoli:
    // è quella che l'indice consuma, ed è l'unica su cui la domanda ha senso.
    // Sui canali presi uno per uno il modo comune c'è sempre, anche quando la
    // sottrazione lo elimina completamente - misurato: 81% sul canale, 8% sulla
    // differenza, con lo stesso identico segnale.
    const auto bip = rawStats(stft.adc(config::kBipolar), kN);
    best.mainsFraction = mainsFraction(stft.dcFree(config::kBipolar), kN,
                                       static_cast<double>(config::kSampleRate));

    if (bip.spread < config::kMinSpreadCounts) {
        // I due frontali danno lo stesso identico segnale: la differenza è nulla
        // e l'indice non avrebbe niente da leggere. Succede se un elettrodo è
        // scollegato e l'ingresso segue l'altro.
        bestFault = SignalFault::Flat;
    } else if (bestFault == SignalFault::None &&
               best.mainsFraction > config::kMaxMainsFraction) {
        bestFault = SignalFault::Mains;
    }

    q.railFraction  = best.railFraction;
    q.autocorr1     = best.autocorr1;
    q.spreadCounts  = best.spread;
    q.mainsFraction = best.mainsFraction;
    q.fault         = bestFault;

    return q;
}

} // namespace mz::dsp
