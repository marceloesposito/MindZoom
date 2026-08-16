#pragma once

// Valutazione della qualità della finestra corrente. Port 1:1 di _assessQuality
// in MuseBluetooth.js.

#include "config.hpp"
#include "dsp/stft.hpp"

#include <array>
#include <cstddef>

namespace mz::dsp {

/**
 * Perché il segnale non è utilizzabile. Sono guasti diversi, con rimedi diversi:
 * l'ordine dell'enum è anche l'ordine di gravità con cui si sceglie quale
 * canale rappresenta la fascia.
 */
enum class SignalFault {
    None = 0,
    Mains,         // quasi tutta la potenza a 50 Hz: l'elettrodo non tocca la pelle
    Railing,       // sbatte contro i fondo scala: amplificatore saturo
    Flat,          // canale piatto: elettrodo staccato
    Uncorrelated   // campioni indipendenti fra loro: NON è una forma d'onda
};

const char* toString(SignalFault f) noexcept;

struct Quality {
    double maxAbsRaw = 0.0;   // picco |µV| sui frontali, dopo la sola rimozione DC
    double medianPeak = 0.0;  // mediana dei picchi recenti (riferimento del gating)
    bool   artifact   = false; // finestra da scartare (blink, mascella, movimento)
    bool   contactOk  = true;  // proxy di qualità del contatto
    double adcMin = 0.0, adcMax = 0.0, adcMean = 0.0;  // diagnostica del decode

    // --- plausibilità fisica del segnale ---
    // Il gating sopra risponde a "questa finestra è sporca?". Questi campi
    // rispondono alla domanda precedente, che nessuno faceva: "quello che sto
    // ricevendo è un segnale biologico?". Una sessione intera è stata calibrata
    // su rumore senza che niente se ne accorgesse.
    double      railFraction = 0.0;  // frazione di campioni ai fondo scala
    double      autocorr1    = 1.0;  // correlazione fra campioni adiacenti
    double      spreadCounts = 0.0;  // deviazione standard in conteggi ADC
    double      mainsFraction = 0.0; // quota di potenza attorno ai 50 Hz
    SignalFault fault        = SignalFault::None;
};

/**
 * Gating RELATIVO agli artefatti: una soglia fissa in µV non funziona con
 * elettrodi dry consumer, dove il picco su 1 s sta normalmente su centinaia di
 * µV e varia con la persona e col contatto. Una finestra è artefatto se il suo
 * picco supera kArtifactRelMult volte la mediana dei picchi recenti E sta sopra
 * un pavimento assoluto di sicurezza.
 *
 * contactOk è un PROXY basato sulla deviazione standard, non l'Horseshoe
 * Indicator del Muse (che richiede una characteristic dedicata, non ancora
 * implementata). Solo il canale piatto è diagnosticabile in assoluto: significa
 * elettrodo staccato. Un limite superiore fisso boccerebbe un dry normale.
 */
class Gating {
public:
    Quality assess(const SlidingStft& stft);
    void    reset();

private:
    double medianPeak() const;

    std::array<double, config::kArtifactPeakHistory> peaks_{};
    std::size_t count_    = 0;
    std::size_t writeIdx_ = 0;
};

} // namespace mz::dsp
