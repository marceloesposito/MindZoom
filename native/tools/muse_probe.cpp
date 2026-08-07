// Il file di registrazione e' un artefatto locale di diagnostica: fopen basta e
// avanza, e fopen_s non e' portabile.
#define _CRT_SECURE_NO_WARNINGS

// Sonda da riga di comando per il Muse.
//
// Tre modi:
//   mz_probe                        live, stampa una riga [EEG] come il client JS
//   mz_probe --record sess.mzr      live, e salva i campioni grezzi su file
//   mz_probe --replay sess.mzr      rigioca il file nella stessa pipeline DSP
//
// Il replay esiste perche' la legge di controllo si giudica solo su segnale
// vero, e rimettere la fascia ad ogni tentativo rende il confronto inutile:
// quando riprovi non ricordi piu' com'era. Registrata una sessione una volta,
// si confrontano le manopole sullo STESSO segnale:
//
//   mz_probe --replay sess.mzr --tolleranza 0.10
//   mz_probe --replay sess.mzr --tolleranza 0.25
//
// e il riepilogo finale dice quale delle due e' utilizzabile.

#include "ble/muse.hpp"
#include "ble/recording.hpp"
#include "control/calibration.hpp"
#include "control/tunables.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include "util/spsc_ring.hpp"

#include <windows.h>   // MultiByteToWideChar: i percorsi da argv sono narrow

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mz;

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

// Il formato del file vive in ble/recording.hpp, condiviso con l'applicazione:
// le registrazioni fatte da MindZoom.exe e quelle fatte qui sono lo stesso file
// e si aprono con entrambi gli strumenti.

/** I percorsi arrivano da argv, quindi nella codepage della console. */
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// --- analisi condivisa fra live e replay -------------------------------------

/**
 * Statistiche che dicono se il controllo e' USABILE, che e' la domanda vera.
 * La media da sola non basta: un controllo che alterna zero e velocita' piena
 * ha la stessa media di uno continuo a meta', ma il primo e' inguidabile.
 */
struct Stats {
    std::uint64_t frames     = 0;
    std::uint64_t activeFrames = 0;   // con velocita' non nulla
    double        sumAbsVel  = 0.0;
    double        maxAbsVel  = 0.0;
    double        sumJump    = 0.0;   // scarto fra campioni adiacenti
    double        maxJump    = 0.0;
    std::uint64_t jumps      = 0;
    double        prevVel    = 0.0;

    void push(double v) {
        ++frames;
        if (v != 0.0) ++activeFrames;
        const double a = std::fabs(v);
        sumAbsVel += a;
        if (a > maxAbsVel) maxAbsVel = a;
        if (frames > 1) {
            const double j = std::fabs(v - prevVel);
            sumJump += j;
            if (j > maxJump) maxJump = j;
            ++jumps;
        }
        prevVel = v;
    }

    void report(const control::Tunables& t) const {
        if (frames == 0) {
            std::printf("\nNessun frame analizzato.\n");
            return;
        }
        const double active = 100.0 * static_cast<double>(activeFrames) /
                              static_cast<double>(frames);
        std::printf("\n--- riepilogo (%llu frame, ~%.0f s) ---\n",
                    static_cast<unsigned long long>(frames),
                    static_cast<double>(frames) * config::kControlDt);
        std::printf("  sensibilita' %.1fx   tolleranza %.2f   decadimento %.2f\n",
                    t.sensitivity, t.localTolerance, t.localDecay);
        std::printf("  tempo con controllo attivo : %.1f%%   (piu' alto = piu' guidabile)\n",
                    active);
        std::printf("  velocita' media |v|        : %.4f  (max %.4f, gain %.4f)\n",
                    sumAbsVel / static_cast<double>(frames), maxAbsVel, t.gain());
        if (jumps > 0) {
            std::printf("  scalino medio fra campioni : %.4f  (max %.4f)\n",
                        sumJump / static_cast<double>(jumps), maxJump);
            std::printf("                               piu' basso = uscita piu' liscia\n");
        }
    }
};

struct Pipeline {
    dsp::SlidingStft       stft;
    dsp::Gating            gating;
    control::IndexSmoother smoother;
    control::Calibration   calib;
    Stats                  stats;

    double        c            = 0.0;
    double        velocity     = 0.0;
    double        elapsed      = 0.0;   // tempo di segnale consumato
    bool          calibrating  = false;
    std::uint64_t frames       = 0;

    /** @return true se il campione ha completato una finestra STFT. */
    bool feed(const ble::Sample& s, const control::Tunables& t, dsp::Quality* outQ,
              dsp::Bands* outBands, double* outIndex) {
        if (!stft.pushSample(s.uv, s.adc)) return false;

        ++frames;
        elapsed += config::kControlDt;

        const auto quality = gating.assess(stft);
        dsp::Bands bands;
        const auto index = dsp::popeIndex(stft, &bands);

        if (outQ) *outQ = quality;
        if (outBands) *outBands = bands;
        if (outIndex) *outIndex = index ? *index : 0.0;

        if (!index || !quality.contactOk || quality.artifact) return true;

        c = smoother.push(*index, config::kControlDt);

        if (calibrating) {
            calib.sample(c);
            calib.tick(config::kControlDt, quality.contactOk);
            if (calib.stage() == control::CalibStage::Done ||
                calib.stage() == control::CalibStage::Failed) {
                calibrating = false;
                std::printf("\n[CALIB] %s   banda [%.3f .. %.3f]  M=%.3f\n\n",
                            control::toString(calib.stage()),
                            calib.absMin(), calib.absMax(), calib.neutral());
            }
        } else if (calib.valid()) {
            velocity = calib.velocity(c, config::kControlDt, t);
            stats.push(velocity);
        }
        return true;
    }
};

void printLine(const Pipeline& p, const dsp::Quality& q, const dsp::Bands& b, double index,
               const char* state, int pktLen, int pktSmp, std::uint64_t dropped) {
    const char* gate = !q.contactOk ? "CONTATTO" : (q.artifact ? "ARTEFATTO" : "OK");
    std::printf(
        "[EEG] %-11s t=%6.1fs idx=%.3f c=%.3f v=%+.4f | t=%.2f a=%.2f b=%.2f"
        " | ampiezza=%.0fuV gate=%s | pkt=%dB/%dsmp frame=%llu persi=%llu\n",
        state, p.elapsed, index, p.c, p.velocity,
        b.theta, b.alpha, b.beta, q.maxAbsRaw, gate,
        pktLen, pktSmp,
        static_cast<unsigned long long>(p.frames),
        static_cast<unsigned long long>(dropped));
}

// --- registrazione sintetica -------------------------------------------------

/**
 * Rumore pseudocasuale deterministico (xorshift a seme fisso): due invocazioni
 * dello stesso comando devono produrre lo stesso file, altrimenti confrontare
 * due tarature su "la stessa registrazione" non vorrebbe dire niente.
 */
class Noise {
public:
    /** Uniforme in [-1, 1]. */
    double next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 7;
        s_ ^= s_ << 17;
        return static_cast<double>(static_cast<std::int64_t>(s_ >> 11)) /
               static_cast<double>(1ull << 52) - 1.0;
    }

private:
    std::uint64_t s_ = 0x9E3779B97F4A7C15ull;
};

/**
 * Genera una registrazione finta con una modulazione nota: 15 s "concentrato"
 * (beta alta), 15 s "rilassato" (alpha alta), poi un'oscillazione lenta fra i
 * due. Serve a provare replay e taratura senza fascia, e a verificare che il
 * formato del file regga il giro completo.
 *
 * La modulazione e' volutamente MITE e il segnale rumoroso: con sinusoidi pure
 * e modulazione ampia l'indice supera la saturazione quasi sempre, gate e
 * ampiezza restano incollati a 1 e il file non distingue piu' una taratura
 * dall'altra. E' il regime rumoroso quello che conta.
 */
int runGenerate(const std::string& path, double seconds) {
    ble::Recorder rec;
    if (!rec.open(toWide(path))) {
        std::printf("Impossibile scrivere %s\n", path.c_str());
        return 2;
    }

    const auto total = static_cast<long long>(seconds * config::kSampleRate);
    constexpr double kTwoPi = 6.283185307179586;
    Noise noise;

    for (long long n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / config::kSampleRate;

        // Quanto e' "concentrato" il segnale in questo istante.
        double conc;
        if (t < config::kCalibConcentrateS)                              conc = 1.0;
        else if (t < config::kCalibConcentrateS + config::kCalibRelaxS)  conc = 0.0;
        else conc = 0.5 + 0.5 * std::sin(kTwoPi * (t - 30.0) / 20.0);

        const double beta  = (14.0 + 9.0 * conc) * std::sin(kTwoPi * 20.0 * t);
        const double alpha = (26.0 - 7.0 * conc) * std::sin(kTwoPi * 10.0 * t);
        const double theta = 20.0 * std::sin(kTwoPi * 6.0 * t);
        const double v     = beta + alpha + theta + 22.0 * noise.next();

        ble::Sample s{};
        for (std::size_t ch = 0; ch < config::kChannels; ++ch) {
            const bool frontal = (ch == config::kFrontalA || ch == config::kFrontalB);
            s.uv[ch]  = frontal ? v : theta;
            s.adc[ch] = static_cast<std::uint16_t>(
                config::kAdcCenter + static_cast<int>(s.uv[ch] / config::kAdcToMicrovolts));
        }
        rec.write(s);
    }

    rec.close();
    std::printf("Registrazione sintetica scritta: %s (%.0f s, %lld campioni)\n",
                path.c_str(), seconds, total);
    return 0;
}

// --- replay ------------------------------------------------------------------

int runReplay(const std::string& path, const control::Tunables& t, double speed) {
    auto loaded = ble::loadRecording(toWide(path));
    if (!loaded.ok) {
        std::printf("%s: %s\n", path.c_str(), loaded.error.c_str());
        return 3;
    }
    const auto& samples = loaded.samples;

    std::printf("Replay di %s: %zu campioni (~%.1f s)\n", path.c_str(), samples.size(),
                loaded.seconds());
    std::printf("Manopole: sensibilita' %.1fx, tolleranza %.2f, smoothing %.2fs\n\n",
                t.sensitivity, t.localTolerance, t.velTauS);

    Pipeline p;
    // La calibrazione parte subito: si presume che la registrazione cominci con
    // le due fasi, come fa l'applicazione.
    p.calib.start();
    p.calibrating = true;
    std::printf("[CALIB] fasi di calibrazione sui primi %.0f s della registrazione\n",
                config::kCalibConcentrateS + config::kCalibRelaxS);

    double lastPrint = -1e9;
    for (const auto& sample : samples) {
        if (g_stop.load()) break;

        dsp::Quality q;
        dsp::Bands   b;
        double       index = 0.0;
        if (!p.feed(sample, t, &q, &b, &index)) continue;

        if (p.elapsed - lastPrint >= 1.0) {
            lastPrint = p.elapsed;
            printLine(p, q, b, index,
                      p.calibrating ? control::toString(p.calib.stage()) : "REPLAY",
                      0, 0, 0);
        }
        if (speed > 0.0) {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(config::kControlDt / speed));
        }
    }

    p.stats.report(t);
    return 0;
}

// --- live --------------------------------------------------------------------

int runLive(const std::string& recordPath, const control::Tunables& t) {
    ble::Recorder rec;
    if (!recordPath.empty()) {
        if (!rec.open(toWide(recordPath))) {
            std::printf("Impossibile scrivere %s\n", recordPath.c_str());
            return 2;
        }
        std::printf("Registrazione su %s\n", recordPath.c_str());
    }

    // BLE -> DSP: il callback GATT non deve bloccare, quindi scrive solo qui.
    util::SpscRing<ble::Sample, 8192> ring;
    std::atomic<std::uint64_t> dropped{0};

    ble::MuseClient muse;
    muse.onLog([](const std::string& msg) { std::printf("[BLE] %s\n", msg.c_str()); });
    muse.onSample([&](const ble::Sample& s) {
        if (!ring.push(s)) dropped.fetch_add(1, std::memory_order_relaxed);
    });

    Pipeline p;
    // La calibrazione parte subito, come nell'applicazione: senza, la velocita'
    // resterebbe a zero per sempre e la colonna v= non direbbe nulla. Il
    // conteggio avanza solo sui frame buoni, quindi l'attesa della connessione
    // non lo consuma.
    p.calib.start();
    p.calibrating = true;
    std::printf("Calibrazione: %.0f s di CONCENTRAZIONE, poi %.0f s di RILASSAMENTO.\n",
                config::kCalibConcentrateS, config::kCalibRelaxS);
    std::printf("Il conteggio parte quando arrivano i dati dalla fascia.\n\n");

    muse.start();

    auto lastLog = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        ble::Sample s;
        bool worked = false;

        while (ring.pop(s)) {
            worked = true;
            rec.write(s);

            dsp::Quality q;
            dsp::Bands   b;
            double       index = 0.0;
            if (!p.feed(s, t, &q, &b, &index)) continue;

            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog < std::chrono::milliseconds(500)) continue;
            lastLog = now;

            // Durante la calibrazione conta la fase, non lo stato del link:
            // e' quello che dice all'utente cosa deve fare in questo momento.
            const char* label = p.calibrating
                ? control::toString(p.calib.stage())
                : ble::toString(muse.state());

            printLine(p, q, b, index, label,
                      muse.lastPacketLen(), muse.lastPacketSamples(),
                      dropped.load(std::memory_order_relaxed));
        }

        if (!worked) {
            // Con il link aperto ma nessun frame, la coppia grezzi/validi dice
            // subito da che parte guardare: zero grezzi = le notifiche non
            // arrivano, grezzi senza validi = formato non riconosciuto.
            const auto now = std::chrono::steady_clock::now();
            if (muse.streaming() && now - lastLog > std::chrono::seconds(2)) {
                lastLog = now;
                std::printf("[BLE] in attesa di dati - pacchetti: %llu grezzi / %llu validi\n",
                            static_cast<unsigned long long>(muse.rawPackets()),
                            static_cast<unsigned long long>(muse.validPackets()));
            }

            // Watchdog: il flusso BLE può fermarsi senza emettere alcun evento.
            const auto silent = muse.millisSinceLastPacket();
            if (muse.streaming() &&
                silent > static_cast<std::int64_t>(config::kEegWatchdogS * 1000)) {
                std::printf("[EEG] nessun dato da %.1fs: tentativo di ripresa\n", silent / 1000.0);
                muse.resumeStreaming();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::printf("\nchiusura...\n");
    muse.stop();
    p.stats.report(t);
    if (rec.active()) {
        std::printf("\nRegistrazione chiusa: %s (%.0f s)\n",
                    recordPath.c_str(), rec.seconds());
        rec.close();
        std::printf("Puoi ritararla senza rimettere la fascia:\n");
        std::printf("  mz_probe --replay %s --tolleranza 0.30\n", recordPath.c_str());
    }
    return 0;
}

void usage() {
    std::printf(
        "Mind Zoom - sonda Muse\n\n"
        "  mz_probe                          diagnostica live\n"
        "  mz_probe --record FILE            live, salvando i campioni grezzi\n"
        "  mz_probe --replay FILE            rigioca una registrazione\n"
        "  mz_probe --genera FILE            registrazione sintetica, per provare\n"
        "                                    il replay senza fascia\n\n"
        "Opzioni (valide anche in replay, per confrontare le manopole sullo\n"
        "stesso segnale registrato):\n"
        "  --sensibilita N   moltiplicatore del gain      (default 1.0)\n"
        "  --tolleranza  N   ampiezza della rampa         (default %.2f)\n"
        "  --decadimento N   velocita' della banda locale (default %.2f)\n"
        "  --velocita    N   moltiplicatore di replay, 0 = piu' veloce possibile\n",
        config::kLocalTolerance, config::kLocalDecay);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    // Diagnostica live: senza questo, con l'output rediretto le righe restano
    // nel buffer e si perdono se il processo viene interrotto.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string recordPath, replayPath, generatePath;
    control::Tunables tune;
    double speed = 0.0;      // in replay, 0 = niente attesa
    double genSeconds = 90.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if (a == "--record")            recordPath = next();
        else if (a == "--replay")       replayPath = next();
        else if (a == "--genera")       generatePath = next();
        else if (a == "--durata")       genSeconds = std::atof(next().c_str());
        else if (a == "--sensibilita")  tune.sensitivity = std::atof(next().c_str());
        else if (a == "--tolleranza")   tune.localTolerance = std::atof(next().c_str());
        else if (a == "--decadimento")  tune.localDecay = std::atof(next().c_str());
        else if (a == "--velocita")     speed = std::atof(next().c_str());
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::printf("Argomento sconosciuto: %s\n\n", a.c_str()); usage(); return 1; }
    }

    if (!generatePath.empty()) return runGenerate(generatePath, genSeconds);
    if (!replayPath.empty())   return runReplay(replayPath, tune, speed);

    std::printf("Mind Zoom - sonda Muse nativa\n");
    std::printf("Ctrl+C per uscire.\n\n");
    return runLive(recordPath, tune);
}
