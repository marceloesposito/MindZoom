#pragma once

// STFT a finestra scorrevole + indice di engagement di Pope.
// Port 1:1 di MuseBluetooth.js (_runFFTAndEmit, hannWindow, _computeRadix2FFT)
// e di computePopeIndex in app.js.

#include "config.hpp"
#include "dsp/filters.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace mz::dsp {

/** Potenze di banda sommate sui canali frontali (AF7+AF8). */
struct Bands {
    double theta = 0.0;
    double alpha = 0.0;
    double beta  = 0.0;
};

/**
 * Catena per canale (DC -> notch -> passa-basso) + ring buffer scorrevoli +
 * FFT ogni kStftHop campioni una volta riempita la finestra.
 *
 * Si alimenta un campione multi-canale alla volta; pushSample() torna true
 * quando è stato prodotto un nuovo frame spettrale (~5.33 volte al secondo).
 * Nessuna allocazione nel percorso caldo: tutti i buffer sono preallocati.
 */
class SlidingStft {
public:
    static constexpr int kN    = config::kStftWindow;
    static constexpr int kBins = config::kStftBins;

    SlidingStft();

    /**
     * @param uv     campioni in µV (già decodificati e centrati), per canale
     * @param rawAdc valori ADC unsigned grezzi, solo per diagnostica del decode
     * @return true se questo campione ha completato un hop e il frame è pronto
     */
    bool pushSample(const std::array<double, config::kChannels>& uv,
                    const std::array<std::uint16_t, config::kChannels>& rawAdc);

    /** Magnitudini spettrali del canale, kBins valori. Valide dopo un frame. */
    const float* mags(int ch) const noexcept { return mags_[static_cast<std::size_t>(ch)].data(); }

    /** Segnale filtrato in banda (ring non riordinato): usato dal gating. */
    const float* filtered(int ch) const noexcept { return filtered_[static_cast<std::size_t>(ch)].data(); }

    /** Segnale con la sola DC rimossa: è su questo che ha senso il gating d'ampiezza. */
    const float* dcFree(int ch) const noexcept { return dcFree_[static_cast<std::size_t>(ch)].data(); }

    /** ADC unsigned grezzo: diagnostica del decode (atteso stretto attorno a 8192). */
    const float* adc(int ch) const noexcept { return adc_[static_cast<std::size_t>(ch)].data(); }

    /** Campioni totali processati: sotto kN la finestra non è ancora piena. */
    std::uint64_t totalSamples() const noexcept { return totalSamples_; }

    /**
     * Posizione dell'ULTIMO campione scritto nei ring filtered()/dcFree()/adc():
     * il piu' vecchio sta subito dopo, a (writeIndex+1) % kN. Serve a chi vuole
     * la forma d'onda in ordine di tempo (il pannello operatore).
     */
    int writeIndex(int ch) const noexcept { return writeIndex_[static_cast<std::size_t>(ch)]; }

    void reset();

private:
    void runFft();

    /** Stato di filtraggio di un canale. */
    struct Chain {
        DcRemover dc{};
        Biquad    notch = makeNotch();
        Biquad    lp    = makeLowpass();
    };

    template <typename T, std::size_t N>
    using PerChannel = std::array<std::array<T, N>, config::kStftChannels>;

    std::array<Chain, config::kStftChannels> chains_{};

    PerChannel<float, kN>    filtered_{};
    PerChannel<float, kN>    dcFree_{};
    PerChannel<float, kN>    adc_{};
    PerChannel<float, kBins> mags_{};

    std::array<int, config::kStftChannels> writeIndex_{};

    std::array<float, kN>     hann_{};
    std::array<float, kN * 2> twiddle_{};
    std::array<float, kN>     re_{};
    std::array<float, kN>     im_{};

    std::uint64_t totalSamples_ = 0;
    int           hopCounter_   = 0;
};

/**
 * Indice di engagement di Pope (1995): beta / (alpha + theta) sui frontali.
 * 1 bin = FS/N = 1 Hz, quindi i bin coincidono con le frequenze in Hz.
 * Torna nullopt se il denominatore è nullo (nessun segnale).
 */
std::optional<double> popeIndex(const SlidingStft& stft, Bands* outBands = nullptr);

} // namespace mz::dsp
