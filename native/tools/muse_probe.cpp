// Sonda da riga di comando: connette il Muse, fa girare il DSP nativo e stampa
// una riga di diagnostica nello stesso formato del log [EEG] del client JS.
// Serve a confrontare i due porti sullo stesso segnale.

#include "ble/muse.hpp"
#include "control/calibration.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include "util/spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

} // namespace

int main() {
    using namespace mz;

    std::signal(SIGINT, onSignal);
    // Diagnostica live: senza questo, con l'output rediretto le righe restano
    // nel buffer e si perdono se il processo viene interrotto.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("Mind Zoom - sonda Muse nativa\n");
    std::printf("Ctrl+C per uscire.\n\n");

    // BLE -> DSP: il callback GATT non deve bloccare, quindi scrive solo qui.
    util::SpscRing<ble::Sample, 8192> ring;
    std::atomic<std::uint64_t> dropped{0};

    ble::MuseClient muse;
    muse.onLog([](const std::string& msg) { std::printf("[BLE] %s\n", msg.c_str()); });
    muse.onSample([&](const ble::Sample& s) {
        if (!ring.push(s)) dropped.fetch_add(1, std::memory_order_relaxed);
    });

    dsp::SlidingStft   stft;
    dsp::Gating        gating;
    control::IndexSmoother smoother;

    muse.start();

    auto lastLog = std::chrono::steady_clock::now();
    std::uint64_t frames = 0;

    while (!g_stop.load()) {
        ble::Sample s;
        bool worked = false;

        while (ring.pop(s)) {
            worked = true;
            if (!stft.pushSample(s.uv, s.adc)) continue;

            ++frames;
            const auto quality = gating.assess(stft);

            dsp::Bands bands;
            const auto index = dsp::popeIndex(stft, &bands);
            if (!index) continue;

            const double c = smoother.push(*index, config::kControlDt);

            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog < std::chrono::milliseconds(500)) continue;
            lastLog = now;

            const char* gate = !quality.contactOk ? "CONTATTO"
                             : (quality.artifact ? "ARTEFATTO" : "OK");

            std::printf(
                "[EEG] %-11s idx=%.3f c=%.3f | t=%.2f a=%.2f b=%.2f | ampiezza=%.0fuV gate=%s"
                " | adc=[%.0f..%.0f] m=%.0f | pkt=%dB/%dsmp frame=%llu persi=%llu\n",
                ble::toString(muse.state()), *index, c,
                bands.theta, bands.alpha, bands.beta,
                quality.maxAbsRaw, gate,
                quality.adcMin, quality.adcMax, quality.adcMean,
                muse.lastPacketLen(), muse.lastPacketSamples(),
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(dropped.load(std::memory_order_relaxed)));
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
            if (muse.streaming() && silent > static_cast<std::int64_t>(config::kEegWatchdogS * 1000)) {
                std::printf("[EEG] nessun dato da %.1fs: tentativo di ripresa\n", silent / 1000.0);
                muse.resumeStreaming();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::printf("\nchiusura...\n");
    muse.stop();
    return 0;
}
