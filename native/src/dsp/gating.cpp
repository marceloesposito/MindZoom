#include "dsp/gating.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mz::dsp {

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

    return q;
}

} // namespace mz::dsp
