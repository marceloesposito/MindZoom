// Confronto diretto: la stessa registrazione, guidata dalla banda adattiva
// invece che dalla calibrazione a due fasi. Riporta gli stessi indicatori
// dell'altro strumento, cosi' i due numeri si possono mettere accanto.
#include "ble/recording.hpp"
#include "config.hpp"
#include "control/adaptive_band.hpp"
#include "control/calibration.hpp"
#include "control/tunables.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: %s <file.mzr>\n", argv[0]); return 1; }
    const std::string p8 = argv[1];
    auto loaded = mz::ble::loadRecording(std::wstring(p8.begin(), p8.end()));
    if (!loaded.ok) { std::fprintf(stderr, "errore: %s\n", loaded.error.c_str()); return 2; }

    mz::dsp::SlidingStft       stft;
    mz::dsp::Gating            gating;
    mz::control::IndexSmoother smoother;
    mz::control::AdaptiveBand  banda;
    mz::control::Calibration   legge;
    const mz::control::Tunables tune{};
    constexpr double dt = mz::config::kControlDt;

    int vIn = 0, vOut = 0, vZero = 0, sopra = 0, guidati = 0;
    double sommaIn = 0, sommaOut = 0;
    std::vector<double> cGuidati;
    double primaPronta = -1.0, tempo = 0.0;

    for (const auto& s : loaded.samples) {
        if (!stft.pushSample(s.uv, s.adc)) continue;
        tempo += dt;
        const auto q = gating.assess(stft);
        const auto idx = mz::dsp::popeIndex(stft, nullptr);
        const bool usable = q.contactOk && !q.artifact &&
                            q.fault == mz::dsp::SignalFault::None;
        if (!idx || !usable) continue;

        const double c = smoother.push(*idx, dt);
        banda.push(c);
        if (!banda.ready()) continue;
        if (primaPronta < 0) primaPronta = tempo;
        legge.adoptBand(banda.lo(), banda.hi(), banda.mid());

        ++guidati;
        cGuidati.push_back(c);
        if (c >= banda.mid()) ++sopra;
        const double v = legge.velocity(c, dt, tune);
        if (v > 1e-9)       { ++vIn;  sommaIn  += v; }
        else if (v < -1e-9) { ++vOut; sommaOut += -v; }
        else                  ++vZero;
    }

    if (guidati == 0) { std::printf("mai pronta\n"); return 3; }
    const double n = guidati;
    std::printf("riscaldamento finito dopo %.0f s di segnale utile\n", primaPronta);
    std::printf("banda finale [%.3f .. %.3f]  neutro M=%.3f\n", banda.lo(), banda.hi(), banda.mid());
    std::printf("tempo SOPRA il neutro: %.1f%%   SOTTO: %.1f%%\n",
                100.0 * sopra / n, 100.0 * (n - sopra) / n);
    std::printf("zoom IN  : %5.1f%%  velocita' media %.4f\n",
                100.0 * vIn / n, vIn ? sommaIn / vIn : 0.0);
    std::printf("zoom OUT : %5.1f%%  velocita' media %.4f\n",
                100.0 * vOut / n, vOut ? sommaOut / vOut : 0.0);
    std::printf("fermo    : %5.1f%%\n", 100.0 * vZero / n);
    std::printf("spostamento netto (in - out): %.2f\n", sommaIn - sommaOut);
    return 0;
}
