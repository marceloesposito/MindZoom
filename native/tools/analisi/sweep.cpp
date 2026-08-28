// Quanto deve essere lunga la finestra? Stessa registrazione, stessa legge,
// solo la finestra cambia. La banda e' ricalcolata qui invece di usare
// AdaptiveBand, cosi' si puo' spazzare senza ricompilare la libreria.
#include "ble/recording.hpp"
#include "config.hpp"
#include "control/calibration.hpp"
#include "control/tunables.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string p8 = argv[1];
    auto loaded = mz::ble::loadRecording(std::wstring(p8.begin(), p8.end()));
    if (!loaded.ok) { std::fprintf(stderr, "errore\n"); return 2; }
    constexpr double dt = mz::config::kControlDt;
    const mz::control::Tunables tune{};

    // Indice condizionato, una volta sola: la catena a monte non dipende dalla finestra.
    std::vector<double> cs;
    { mz::dsp::SlidingStft stft; mz::dsp::Gating g; mz::control::IndexSmoother sm;
      for (const auto& s : loaded.samples) {
        if (!stft.pushSample(s.uv, s.adc)) continue;
        const auto q = g.assess(stft);
        const auto idx = mz::dsp::popeIndex(stft, nullptr);
        if (!idx || !q.contactOk || q.artifact || q.fault != mz::dsp::SignalFault::None) continue;
        cs.push_back(sm.push(*idx, dt));
      } }

    std::printf("%-10s %8s %8s %8s %9s\n", "finestra", "sopra%", "in%", "out%", "netto");
    for (const double winS : {20.0, 30.0, 45.0, 60.0, 90.0, 150.0}) {
        const auto cap = static_cast<std::size_t>(winS * mz::config::kControlHz);
        const auto warm = static_cast<std::size_t>(std::min(25.0, winS) * mz::config::kControlHz);
        std::deque<double> win;
        mz::control::Calibration legge;
        std::vector<double> scratch;
        int in = 0, out = 0, zero = 0, sopra = 0, n = 0;
        double sIn = 0, sOut = 0;
        for (const double c : cs) {
            win.push_back(c);
            if (win.size() > cap) win.pop_front();
            if (win.size() < warm) continue;
            scratch.assign(win.begin(), win.end());
            const auto at = [&](double q) {
                auto k = static_cast<std::size_t>(q * (scratch.size() - 1));
                std::nth_element(scratch.begin(), scratch.begin() + (long)k, scratch.end());
                return scratch[k];
            };
            const double lo = at(0.15), mid = at(0.5), hi = at(0.85);
            if (!(hi > lo)) continue;
            legge.adoptBand(lo, hi, mid);
            ++n; if (c >= mid) ++sopra;
            const double v = legge.velocity(c, dt, tune);
            if (v > 1e-9) { ++in; sIn += v; } else if (v < -1e-9) { ++out; sOut += -v; } else ++zero;
        }
        if (!n) { std::printf("%-10.0f  (mai pronta)\n", winS); continue; }
        std::printf("%-10.0f %7.1f%% %7.1f%% %7.1f%% %9.1f\n", winS,
                    100.0 * sopra / n, 100.0 * in / n, 100.0 * out / n, sIn - sOut);
    }
    return 0;
}
