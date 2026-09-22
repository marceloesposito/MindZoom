// Genera una registrazione .mzr sintetica, per esercitare l'esperienza senza
// fascia collegata (test dello shell macOS in assenza dell'hardware: batteria
// scarica, niente Muse a portata di mano, eccetera).
//
// NON e' EEG vero: e' un segnale plausibile - la logica che lo costruisce e'
// in ble/synthetic.hpp, condivisa con il tasto D dentro l'esperienza stessa.
//
// Uso: mz_gen_test_recording <file.mzr> [secondi]

#include "ble/recording.hpp"
#include "ble/synthetic.hpp"
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
std::wstring toWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s <file.mzr> [secondi=180]\n", argv[0]);
        return 1;
    }
    const std::wstring path = toWide(argv[1]);
    const double seconds = (argc >= 3) ? std::atof(argv[2]) : 180.0;
    const auto total = static_cast<std::uint64_t>(seconds * mz::config::kSampleRate);

    mz::ble::Recorder rec;
    if (!rec.arm(path)) {
        std::fprintf(stderr, "impossibile preparare %s\n", argv[1]);
        return 2;
    }
    for (std::uint64_t n = 0; n < total; ++n) rec.write(mz::ble::synthetic::makeSample(n));
    rec.close();

    std::printf("scritti %llu campioni (%.0f s) in %s\n",
                static_cast<unsigned long long>(total), seconds, argv[1]);
    return 0;
}
