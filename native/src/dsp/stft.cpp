#include "dsp/stft.hpp"

#include <cmath>
#include <numbers>
#include <utility>

namespace mz::dsp {

SlidingStft::SlidingStft() {
    // Finestra di Hann: con finestre sovrapposte contiene lo spectral leakage.
    for (int i = 0; i < kN; ++i) {
        hann_[static_cast<std::size_t>(i)] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * i / (kN - 1))));
    }

    // Tabella dei twiddle factor, precalcolata: e^(-2πi·k/N).
    constexpr double kTwoPiNeg = -2.0 * std::numbers::pi;
    for (int i = 0; i < kN; ++i) {
        const double angle = i * (kTwoPiNeg / kN);
        twiddle_[static_cast<std::size_t>(i) * 2]     = static_cast<float>(std::cos(angle));
        twiddle_[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(std::sin(angle));
    }
}

void SlidingStft::reset() {
    for (auto& c : chains_) {
        c.dc.reset();
        c.notch.reset();
        c.lp.reset();
    }
    filtered_ = {};
    dcFree_   = {};
    adc_      = {};
    mags_     = {};
    writeIndex_.fill(0);
    totalSamples_ = 0;
    hopCounter_   = 0;
}

bool SlidingStft::pushSample(const std::array<double, config::kChannels>& uv,
                             const std::array<std::uint16_t, config::kChannels>& rawAdc) {
    // Il canale bipolare è una DERIVAZIONE, non un elettrodo: la differenza fra
    // i due frontali. Il ronzio di rete arriva identico su entrambi (modo
    // comune) e qui si cancella; l'attività cerebrale, che sui due elettrodi è
    // diversa, resta. Va costruita PRIMA dei filtri, sui µV grezzi, perché due
    // catene di filtri separate introdurrebbero differenze di fase che poi la
    // sottrazione trasformerebbe in segnale inventato.
    const double bipolarUv =
        uv[static_cast<std::size_t>(config::kFrontalA)] -
        uv[static_cast<std::size_t>(config::kFrontalB)];
    const auto bipolarAdc = static_cast<double>(config::kAdcCenter) +
                            static_cast<double>(rawAdc[static_cast<std::size_t>(config::kFrontalA)]) -
                            static_cast<double>(rawAdc[static_cast<std::size_t>(config::kFrontalB)]);

    for (std::size_t ch = 0; ch < config::kStftChannels; ++ch) {
        Chain& chain = chains_[ch];

        const bool   bip = (ch == static_cast<std::size_t>(config::kBipolar));
        const double in  = bip ? bipolarUv : uv[ch];

        // 1. Rimozione DC. Il segnale a valle di questa e a monte dei filtri in
        //    banda è quello su cui ha senso il gating d'ampiezza.
        const double dcFree = chain.dc.process(in);

        // 2-3. Notch (rete elettrica) e passa-basso.
        const double filtered = chain.lp.process(chain.notch.process(dcFree));

        const int idx = (writeIndex_[ch] + 1) % kN;
        writeIndex_[ch] = idx;

        const auto i = static_cast<std::size_t>(idx);
        filtered_[ch][i] = static_cast<float>(filtered);
        dcFree_[ch][i]   = static_cast<float>(dcFree);
        adc_[ch][i]      = static_cast<float>(bip ? bipolarAdc : rawAdc[ch]);
    }

    // Un tick di hop per campione (non per canale): i 4 canali avanzano insieme.
    ++totalSamples_;
    ++hopCounter_;

    if (totalSamples_ >= static_cast<std::uint64_t>(kN) && hopCounter_ >= config::kStftHop) {
        hopCounter_ = 0;
        runFft();
        return true;
    }
    return false;
}

void SlidingStft::runFft() {
    for (std::size_t ch = 0; ch < config::kStftChannels; ++ch) {
        // Il ring va riordinato: il campione più vecchio sta subito dopo la testa
        // di scrittura. La finestra di Hann si applica in ordine temporale.
        const int tail = writeIndex_[ch];
        const int head = (tail + 1) % kN;
        const auto& src = filtered_[ch];

        for (int i = 0; i < kN; ++i) {
            const auto s = static_cast<std::size_t>((head + i) % kN);
            re_[static_cast<std::size_t>(i)] = src[s] * hann_[static_cast<std::size_t>(i)];
            im_[static_cast<std::size_t>(i)] = 0.0f;
        }

        // --- FFT radix-2 in place ---
        // Permutazione bit-reverse.
        int j = 0;
        for (int i = 0; i < kN - 1; ++i) {
            if (i < j) {
                std::swap(re_[static_cast<std::size_t>(i)], re_[static_cast<std::size_t>(j)]);
                std::swap(im_[static_cast<std::size_t>(i)], im_[static_cast<std::size_t>(j)]);
            }
            int k = kN >> 1;
            while (k <= j) {
                j -= k;
                k >>= 1;
            }
            j += k;
        }

        // Farfalle.
        for (int size = 2; size <= kN; size <<= 1) {
            const int half = size >> 1;
            for (int m = 0; m < half; ++m) {
                const auto lut = static_cast<std::size_t>(m * (kN / size)) * 2;
                const float wRe = twiddle_[lut];
                const float wIm = twiddle_[lut + 1];

                for (int i = m; i < kN; i += size) {
                    const auto a = static_cast<std::size_t>(i);
                    const auto b = static_cast<std::size_t>(i + half);
                    const float tRe = wRe * re_[b] - wIm * im_[b];
                    const float tIm = wRe * im_[b] + wIm * re_[b];
                    re_[b] = re_[a] - tRe;
                    im_[b] = im_[a] - tIm;
                    re_[a] += tRe;
                    im_[a] += tIm;
                }
            }
        }

        for (int i = 0; i < kBins; ++i) {
            const auto k = static_cast<std::size_t>(i);
            mags_[ch][k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]) / kN;
        }
    }
}

std::optional<double> popeIndex(const SlidingStft& stft, Bands* outBands) {
    double theta = 0.0, alpha = 0.0, beta = 0.0;

    // Sulla derivazione bipolare AF7-AF8, non sulla somma dei due canali presi
    // singolarmente. Sommandoli si sommava anche il ronzio di rete, che sui due
    // elettrodi è lo stesso segnale: misurato, l'81% della potenza. Sulla
    // differenza scende all'8%, e la potenza in banda aumenta.
    //
    // L'indice è adimensionale e la calibrazione misura gli estremi PERSONALI,
    // quindi il cambio di derivazione viene assorbito dalla calibrazione: non
    // c'è nessuna soglia assoluta da ritarare.
    {
        const float* m = stft.mags(config::kBipolar);
        // Bin inclusivi come nel JS: 1 bin = 1 Hz con N=256 @256 Hz.
        for (int b = config::kThetaLo; b <= config::kThetaHi; ++b) theta += m[b];
        for (int b = config::kAlphaLo; b <= config::kAlphaHi; ++b) alpha += m[b];
        for (int b = config::kBetaLo;  b <= config::kBetaHi;  ++b) beta  += m[b];
    }

    if (outBands) *outBands = Bands{theta, alpha, beta};

    const double denom = alpha + theta;
    if (denom == 0.0) return std::nullopt;
    return beta / denom;
}

} // namespace mz::dsp
