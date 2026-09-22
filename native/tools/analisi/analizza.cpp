// Strumento d'analisi usa-e-getta: rigioca una registrazione .mzr attraverso
// esattamente la stessa catena dell'applicazione (STFT -> gating -> indice di
// Pope -> smoother -> calibrazione -> legge di controllo) e riporta dove sta
// l'indice rispetto alla banda calibrata, separando il tempo passato in zoom
// in da quello in zoom out.
//
// Serve a rispondere a una domanda precisa: "e' plausibile non essere
// riusciti a fare zoom out?". Non fa parte del programma, vive nel tmp del job.

#include "ble/recording.hpp"
#include "config.hpp"
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
    const std::wstring path(p8.begin(), p8.end());

    auto loaded = mz::ble::loadRecording(path);
    if (!loaded.ok) { std::fprintf(stderr, "errore: %s\n", loaded.error.c_str()); return 2; }
    std::printf("campioni=%zu  secondi=%.1f  versione=%u\n",
                loaded.samples.size(), loaded.seconds(), loaded.version);

    mz::dsp::SlidingStft       stft;
    mz::dsp::Gating            gating;
    mz::control::IndexSmoother smoother;
    mz::control::Calibration   calib;
    const mz::control::Tunables tune{};
    constexpr double dt = mz::config::kControlDt;

    calib.start();

    // La calibrazione parte quando l'utente preme INVIO; qui si fa partire
    // subito, quindi i tempi non coincidono con la sessione vera. Cio' che
    // interessa e' la BANDA che ne esce e come si distribuisce l'indice dopo.
    std::vector<double> cDopo;        // indice condizionato dopo la calibrazione
    bool calibChiusa = false;
    double absMin = 0, absMax = 0, M = 0;

    int finestre = 0, gated = 0;
    for (const auto& s : loaded.samples) {
        if (!stft.pushSample(s.uv, s.adc)) continue;
        ++finestre;

        const auto q = gating.assess(stft);
        mz::dsp::Bands bands;
        const auto idx = mz::dsp::popeIndex(stft, &bands);
        const bool usable = q.contactOk && !q.artifact &&
                            q.fault == mz::dsp::SignalFault::None;
        if (!usable) ++gated;
        if (!idx) continue;

        const double c = smoother.push(*idx, dt);

        if (!calibChiusa) {
            calib.sample(c, usable);
            calib.tick(dt, usable);
            if (calib.stage() == mz::control::CalibStage::Done && calib.valid()) {
                calibChiusa = true;
                absMin = calib.absMin(); absMax = calib.absMax(); M = calib.neutral();
                std::printf("\ncalibrazione chiusa: banda [%.3f .. %.3f]  neutro M=%.3f  span=%.3f\n",
                            absMin, absMax, M, absMax - absMin);
                std::printf("  semi-ampiezza verso l'alto  (absMax-M) = %.3f\n", absMax - M);
                std::printf("  semi-ampiezza verso il basso (M-absMin) = %.3f\n", M - absMin);
            } else if (calib.stage() == mz::control::CalibStage::Failed) {
                std::printf("calibrazione FALLITA: %s\n", calib.message().c_str());
                return 3;
            }
        } else {
            cDopo.push_back(c);
        }
    }
    std::printf("finestre=%d  scartate dal gating=%d (%.1f%%)\n",
                finestre, gated, 100.0 * gated / std::max(1, finestre));

    if (!calibChiusa) { std::printf("\nla calibrazione non si e' chiusa entro il file\n"); return 4; }
    if (cDopo.empty()) { std::printf("\nnessun campione dopo la calibrazione\n"); return 5; }

    // Distribuzione dell'indice rispetto al neutro.
    int sopra = 0;
    for (const double c : cDopo) if (c >= M) ++sopra;
    auto ord = cDopo;
    std::sort(ord.begin(), ord.end());
    const auto pct = [&](double q) { return ord[static_cast<std::size_t>(q * (ord.size() - 1))]; };

    std::printf("\n--- indice DOPO la calibrazione (%zu campioni, %.0fs) ---\n",
                cDopo.size(), cDopo.size() * dt);
    std::printf("  min=%.3f  p5=%.3f  p25=%.3f  mediana=%.3f  p75=%.3f  p95=%.3f  max=%.3f\n",
                ord.front(), pct(0.05), pct(0.25), pct(0.50), pct(0.75), pct(0.95), ord.back());
    std::printf("  tempo SOPRA il neutro M: %.1f%%   SOTTO: %.1f%%\n",
                100.0 * sopra / cDopo.size(), 100.0 * (cDopo.size() - sopra) / cDopo.size());
    std::printf("  il neutro M=%.3f cade al percentile %.1f della distribuzione\n", M,
                100.0 * (std::lower_bound(ord.begin(), ord.end(), M) - ord.begin()) / ord.size());

    // Deriva: l'indice sale/scende lentamente nel corso della sessione? Se
    // si', una banda fissata all'inizio e' destinata a stare fuori posto piu'
    // avanti, indipendentemente da come la si e' misurata.
    {
        const std::size_t t = cDopo.size() / 3;
        const auto media = [&](std::size_t a, std::size_t b) {
            double s = 0; for (std::size_t i = a; i < b; ++i) s += cDopo[i];
            return s / static_cast<double>(b - a);
        };
        std::printf("  deriva nel tempo: primo terzo %.3f -> ultimo terzo %.3f  (M=%.3f)\n",
                    media(0, t), media(2 * t, cDopo.size()), M);
    }

    // Legge di controllo vera: quanto tempo la velocita' sarebbe stata
    // positiva, negativa o nulla.
    mz::control::Calibration sim = calib;   // stessa banda, banda locale ripartita da M
    int vIn = 0, vOut = 0, vZero = 0;
    double sommaIn = 0, sommaOut = 0;
    for (const double c : cDopo) {
        const double v = sim.velocity(c, dt, tune);
        if (v > 1e-9)      { ++vIn;  sommaIn  += v; }
        else if (v < -1e-9){ ++vOut; sommaOut += -v; }
        else                 ++vZero;
    }
    const double n = static_cast<double>(cDopo.size());
    std::printf("\n--- legge di controllo (gain=%.2f) ---\n", tune.gain());
    std::printf("  zoom IN  : %5.1f%% del tempo   velocita' media %.4f\n",
                100.0 * vIn / n, vIn ? sommaIn / vIn : 0.0);
    std::printf("  zoom OUT : %5.1f%% del tempo   velocita' media %.4f\n",
                100.0 * vOut / n, vOut ? sommaOut / vOut : 0.0);
    std::printf("  fermo    : %5.1f%% del tempo\n", 100.0 * vZero / n);
    std::printf("\n  spostamento netto (in - out), in unita' di velocita'*campione: %.2f\n",
                sommaIn - sommaOut);
    return 0;
}
