#pragma once

// Segnale sintetico plausibile (non EEG vero) per esercitare l'esperienza senza
// fascia collegata: batteria scarica, niente Muse a portata di mano, o solo
// voglia di provare l'interfaccia. Costruito per PASSARE i controlli di
// qualita' (dsp::Gating) e per dare all'indice di Pope un andamento
// chiaramente bimodale, cosi' la calibrazione ha materiale su cui separare le
// due classi.
//
// Condiviso fra tools/gen_test_recording.cpp (scrive un .mzr su disco) e
// app/experience.cpp (tasto D: genera in memoria e lo manda subito al posto
// della fascia) - stessa logica generatrice, un solo posto dove ritararla.

#include "ble/muse.hpp"
#include "config.hpp"
#include "dsp/decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mz::ble::synthetic {

/**
 * Miscela concentrazione/rilassamento nel tempo: due blocchi lunghi all'inizio
 * (per dare alla calibrazione, che li incontra nell'ordine Concentrate poi
 * Relax, materiale pulito su cui separare le classi), poi un'oscillazione
 * lenta per il resto della registrazione - quella e' la parte che mostra lo
 * zoom muoversi avanti e indietro in fase interattiva.
 */
inline double concentrationMix(double t) {
    // Il generatore non sa a che punto e' la calibrazione (e' una funzione pura
    // di t, senza stato), ma le due fasi ora hanno vincoli di durata MOLTO
    // diversi: Concentrate (prima) chiude appena i campioni indipendenti
    // bastano, senza attendere un ciclo - con questo segnale, tipicamente in
    // pochi secondi (il pavimento e' kCalibMinRawForLatch campioni grezzi a
    // kControlHz). Relax (seconda) invece chiude solo a un bordo di ciclo del
    // respiro guidato, minimo kBreathCycleS=16s. I due blocchi qui sotto
    // riflettono questo: corto quello che alimenta Concentrate, abbastanza
    // lungo quello che alimenta Relax da coprire un ciclo pulito con margine.
    constexpr double kConcentrateBlockS = 10.0;
    constexpr double kRelaxBlockS       = 24.0;   // >1 ciclo di respiro (16s) di segnale pulito
    if (t < kConcentrateBlockS) return 0.90;                                     // concentrazione
    if (t < kRelaxBlockS)       return 0.10;                                     // rilassamento
    const double phase = (t - kRelaxBlockS) * (2.0 * M_PI / 24.0);               // ciclo 24s
    return 0.5 + 0.45 * std::sin(phase);
}

/** Rumore deterministico, riproducibile: non serve un RNG vero per questo scopo. */
inline double pseudoNoise(std::uint64_t n, int ch) {
    std::uint64_t x = n * 2654435761u + static_cast<std::uint64_t>(ch) * 40503u + 12345u;
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return (static_cast<double>(x % 2000u) / 1000.0 - 1.0);   // [-1, 1]
}

inline Sample makeSample(std::uint64_t n) {
    const double t = static_cast<double>(n) / config::kSampleRate;
    const double m = concentrationMix(t);

    const double thetaAmp = 12.0 * (1.0 - 0.3 * m);
    const double alphaAmp = 14.0 * (1.0 - 0.6 * m);
    const double betaAmp  = 6.0 + 14.0 * m;

    Sample s;
    for (int ch = 0; ch < config::kChannels; ++ch) {
        // Il fondo (theta+alpha) e' comune a tutti i canali con una piccola
        // differenza di fase per canale, come farebbero elettrodi reali. La
        // componente beta modulata sta SOLO su AF7 (canale 1): la derivazione
        // bipolare AF7-AF8 usata da popeIndex la vede piena, non attenuata da
        // una copia quasi uguale sull'altro frontale.
        const double phase = 0.35 * ch;
        double uv = thetaAmp * std::sin(2.0 * M_PI * 6.0 * t + phase) +
                    alphaAmp * std::sin(2.0 * M_PI * 10.0 * t + phase * 1.7);
        if (ch == config::kFrontalA) {
            uv += betaAmp * std::sin(2.0 * M_PI * 20.0 * t);
        }
        uv += 1.5 * pseudoNoise(n, ch);   // rumore piccolo: non deve rompere l'autocorrelazione

        const int centered = static_cast<int>(std::lround(uv / config::kAdcToMicrovolts));
        const int raw = std::clamp(centered + config::kAdcCenter, 0, 16383);

        s.adc[static_cast<std::size_t>(ch)] = static_cast<std::uint16_t>(raw);
        s.uv[static_cast<std::size_t>(ch)]  =
            dsp::toMicrovolts(dsp::centerSample(s.adc[static_cast<std::size_t>(ch)]));
    }
    return s;
}

/** `seconds` secondi di segnale sintetico, a config::kSampleRate. */
inline std::vector<Sample> generate(double seconds) {
    const auto total = static_cast<std::uint64_t>(std::max(0.0, seconds) * config::kSampleRate);
    std::vector<Sample> out;
    out.reserve(static_cast<std::size_t>(total));
    for (std::uint64_t n = 0; n < total; ++n) out.push_back(makeSample(n));
    return out;
}

} // namespace mz::ble::synthetic
