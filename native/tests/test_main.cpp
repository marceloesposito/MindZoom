// Test numerici del core nativo. Rispecchiano tests/control-pipeline.test.js e
// tests/stft.test.js: stesse attese sugli stessi valori, così il port si valida
// contro la pipeline JS già tarata sul campo invece che contro sé stesso.

#include "config.hpp"
#include "control/calibration.hpp"
#include "control/zoom.hpp"
#include "dsp/decode.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& name) {
    if (ok) {
        ++g_pass;
        std::printf("  PASS  %s\n", name.c_str());
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n", name.c_str());
    }
}

void nearly(double got, double want, double tol, const std::string& name) {
    const bool ok = std::fabs(got - want) <= tol;
    if (ok) {
        ++g_pass;
        std::printf("  PASS  %s (%.6f)\n", name.c_str(), got);
    } else {
        ++g_fail;
        std::printf("  FAIL  %s: atteso %.6f +/- %.6f, ottenuto %.6f\n",
                    name.c_str(), want, tol, got);
    }
}

void group(const char* title) { std::printf("\n[%s]\n", title); }

using namespace mz;

constexpr double kDt = config::kControlDt;   // 48/256 = 0.1875 s

/** Porta una calibrazione a termine con estremi noti, senza passare dal segnale. */
void calibrateTo(control::Calibration& cal, double lo, double hi) {
    cal.start();                       // -> CONCENTRATE
    cal.tick(config::kCalibLeadInS + 0.1, true);
    cal.sample(hi);
    cal.tick(config::kCalibConcentrateS, true);   // -> RELAX
    cal.tick(config::kCalibLeadInS + 0.1, true);
    cal.sample(lo);
    cal.tick(config::kCalibRelaxS, true);         // -> finalize
}

/** Alimenta la STFT con una sinusoide sui frontali finché non emette un frame. */
bool feedSine(dsp::SlidingStft& stft, double freqHz, double amplitudeUv, int samples) {
    bool emitted = false;
    for (int n = 0; n < samples; ++n) {
        const double phase = 2.0 * std::numbers::pi * freqHz * n / config::kSampleRate;
        const double v = amplitudeUv * std::sin(phase);

        std::array<double, config::kChannels> uv{};
        std::array<std::uint16_t, config::kChannels> adc{};
        for (std::size_t ch = 0; ch < config::kChannels; ++ch) {
            const bool frontal = (ch == config::kFrontalA || ch == config::kFrontalB);
            uv[ch] = frontal ? v : 0.0;
            adc[ch] = static_cast<std::uint16_t>(
                config::kAdcCenter + static_cast<int>(v / config::kAdcToMicrovolts));
        }
        if (stft.pushSample(uv, adc)) emitted = true;
    }
    return emitted;
}

// ---------------------------------------------------------------------------

void testDecode() {
    group("1. Decodifica dei pacchetti");

    const std::array<std::uint8_t, 3> full{0xFF, 0x3F, 0x00};
    check(dsp::unpack14(full.data(), full.size(), 0) == 0x3FFF,
          "unpack14 estrae il fondo scala 0x3FFF");

    const std::array<std::uint8_t, 3> zero{0x00, 0x00, 0x00};
    check(dsp::unpack14(zero.data(), zero.size(), 0) == 0,
          "unpack14 estrae zero");

    check(dsp::centerSample(config::kAdcCenter) == 0,
          "il centro ADC (8192) mappa sullo zero");
    check(dsp::centerSample(0) == -config::kAdcCenter,
          "lo zero ADC mappa sul minimo negativo");

    nearly(dsp::toMicrovolts(config::kAdcCenter), 725.0, 1.0,
           "fondo scala positivo ~725 uV");

    // Un pacchetto da 240 B ha 14 B di header -> 226 B di payload.
    // Con dataType=2 (8 canali) sono 16 campioni: combacia coi log sul campo.
    check(dsp::samplesInPayload(226, 8) == 16,
          "226 B su 8 canali -> 16 campioni (come nei log)");
    check(dsp::samplesInPayload(226, 4) == 32,
          "226 B su 4 canali -> 32 campioni");
    check(dsp::samplesInPayload(0, 4) == 0, "payload vuoto -> 0 campioni");
}

void testStft() {
    group("2. STFT e indice di Pope");

    dsp::SlidingStft stft;
    const bool emitted = feedSine(stft, 10.0, 20.0, config::kStftWindow * 3);
    check(emitted, "la STFT emette un frame a finestra piena");

    // Picco spettrale sul bin corrispondente: 1 bin = FS/N = 1 Hz.
    const float* mags = stft.mags(config::kFrontalA);
    int peakBin = 0;
    for (int b = 1; b < dsp::SlidingStft::kBins; ++b) {
        if (mags[b] > mags[peakBin]) peakBin = b;
    }
    check(peakBin == 10, "picco spettrale sul bin 10 per una sinusoide a 10 Hz");

    // Sinusoide in banda alpha -> beta trascurabile -> indice di Pope basso.
    dsp::Bands bands;
    const auto idx = dsp::popeIndex(stft, &bands);
    check(idx.has_value(), "l'indice di Pope e' definito con segnale presente");
    check(bands.alpha > bands.beta, "l'energia alpha domina sulla beta a 10 Hz");
    check(idx.value() < 0.5, "indice di Pope basso su segnale puramente alpha");

    // Il rate di controllo deriva dall'hop.
    nearly(config::kControlHz, 5.333, 0.01, "rate di controllo ~5.33 Hz");
    check(config::kControlHz >= 4.0 && config::kControlHz <= 8.0,
          "rate di controllo nella finestra 4-8 Hz");
    nearly(kDt * 1000.0, 187.5, 0.1, "latenza di hop 187.5 ms (< 300 ms)");
}

void testGating() {
    group("3. Gating contatto e artefatti");

    dsp::SlidingStft stft;
    dsp::Gating gating;

    feedSine(stft, 10.0, 20.0, config::kStftWindow * 2);
    const auto ok = gating.assess(stft);
    check(ok.contactOk, "segnale modulante -> contatto ok");
    check(!ok.artifact, "prima finestra senza storia -> nessun artefatto");

    // Canale piatto = elettrodo staccato.
    dsp::SlidingStft flat;
    feedSine(flat, 10.0, 0.0, config::kStftWindow * 2);
    dsp::Gating flatGating;
    const auto bad = flatGating.assess(flat);
    check(!bad.contactOk, "canale piatto -> contatto assente");
}

void testCalibration() {
    group("4. Calibrazione attiva");

    control::Calibration cal;
    check(cal.stage() == control::CalibStage::Intro, "si parte dalla schermata introduttiva");

    cal.start();
    check(cal.stage() == control::CalibStage::Concentrate, "start -> fase di concentrazione");

    // Prima del lead-in i campioni non contano per gli estremi assoluti.
    cal.sample(99.0);
    cal.tick(config::kCalibLeadInS + 0.1, true);
    cal.sample(2.0);
    cal.tick(config::kCalibConcentrateS, true);
    check(cal.stage() == control::CalibStage::Relax, "a fine tempo si passa al rilassamento");

    cal.tick(config::kCalibLeadInS + 0.1, true);
    cal.sample(1.0);
    cal.tick(config::kCalibRelaxS, true);

    check(cal.stage() == control::CalibStage::Done, "la calibrazione si chiude da sola");
    check(cal.valid(), "calibrazione valida");
    nearly(cal.absMax(), 2.0, 1e-9, "absMax dal picco registrato (scarta il pre-lead-in)");
    nearly(cal.absMin(), 1.0, 1e-9, "absMin dal minimo registrato");
    nearly(cal.neutral(), 1.5, 1e-9, "neutro M = media dei due estremi");
    nearly(cal.localMax(), 1.5, 1e-9, "la banda locale parte dal neutro");
    nearly(cal.localMin(), 1.5, 1e-9, "banda locale inizializzata su M");

    // Il contatto scarso mette in pausa il conteggio invece di consumarlo.
    control::Calibration paused;
    paused.start();
    paused.tick(5.0, false);
    nearly(paused.stageElapsed(), 0.0, 1e-9, "contatto assente -> tempo di fase in pausa");
}

void testCalibrationFailures() {
    group("5. Fallimenti di calibrazione");

    control::Calibration narrow;
    calibrateTo(narrow, 1.00, 1.01);   // span 0.01 su M~1 -> sotto kCalibMinSpanRel
    check(narrow.stage() == control::CalibStage::Failed, "span troppo stretta -> fallimento");
    check(!narrow.valid(), "calibrazione fallita non e' valida");
    nearly(narrow.velocity(5.0, kDt), 0.0, 1e-12, "calibrazione fallita -> velocita' nulla");

    control::Calibration silent;
    silent.start();
    silent.tick(config::kCalibConcentrateS, true);
    silent.tick(config::kCalibRelaxS, true);   // nessun campione mai registrato
    check(silent.stage() == control::CalibStage::Failed, "segnale assente -> fallimento");
}

void testExtremaVelocity() {
    group("6. Legge di controllo a estremi");

    // absMin=0.5, absMax=1.5 -> M=1.0, span=1.0
    control::Calibration cal;
    calibrateTo(cal, 0.5, 1.5);
    check(cal.valid(), "calibrazione di riferimento valida");
    nearly(cal.neutral(), 1.0, 1e-9, "neutro a 1.0");

    // Nessun warm-up: velocita' utile al primo tick dopo la calibrazione.
    const double first = cal.velocity(1.4, kDt);
    check(first > 0.0, "nessun warm-up: zoom in gia' al primo campione");

    // Saturazione al 75% del tragitto M->estremo: c >= 1 + 0.75*0.5 = 1.375
    control::Calibration sat;
    calibrateTo(sat, 0.5, 1.5);
    nearly(sat.velocity(1.5, kDt), config::kExtremaGain, 1e-9,
           "saturazione a EXTREMA_GAIN all'estremo assoluto");

    control::Calibration mid;
    calibrateTo(mid, 0.5, 1.5);
    // c = 1.1875 = M + 0.5*(satFrac*(absMax-M)) -> meta' della velocita' piena
    nearly(mid.velocity(1.1875, kDt), config::kExtremaGain * 0.5, 1e-9,
           "meta' del tragitto di saturazione -> meta' velocita'");

    // Isteresi: scendendo sotto il massimo locale la velocita' si annulla.
    control::Calibration hyst;
    calibrateTo(hyst, 0.5, 1.5);
    check(hyst.velocity(1.4, kDt) > 0.0, "spinta iniziale -> zoom in");
    nearly(hyst.velocity(1.30, kDt), 0.0, 1e-12,
           "sotto il massimo locale -> velocita' nulla (stop dello zoom in)");

    // Sotto il minimo locale si va in zoom out.
    control::Calibration out;
    calibrateTo(out, 0.5, 1.5);
    const double back = out.velocity(0.6, kDt);
    check(back < 0.0, "sotto il minimo locale -> zoom out");
    nearly(back, -config::kExtremaGain, 1e-9, "zoom out saturato vicino all'estremo basso");

    // La banda locale non esce mai dagli estremi assoluti.
    control::Calibration clampd;
    calibrateTo(clampd, 0.5, 1.5);
    clampd.velocity(99.0, kDt);
    check(clampd.localMax() <= 1.5 + 1e-12, "il massimo locale non supera l'assoluto");
    clampd.velocity(-99.0, kDt);
    check(clampd.localMin() >= 0.5 - 1e-12, "il minimo locale non scende sotto l'assoluto");
}

void testSmoother() {
    group("7. Denoise dell'indice");

    control::IndexSmoother s;
    check(!s.initialized(), "non inizializzato prima del primo campione");
    nearly(s.push(2.0), 2.0, 1e-12, "il primo campione inizializza senza transitorio");
    nearly(s.push(3.0), 2.0 + 1.0 * config::kCalibIndexEma, 1e-12,
           "EMA di denoise sul secondo campione");
}

void testZoom() {
    group("8. Rate control, detent e crossfade");

    control::ZoomController z;

    // In onboarding l'autorita' e' nulla: lo zoom resta fermo.
    nearly(z.authorityVelocity(1.0, control::Phase::Onboarding, 0.0), 0.0, 1e-12,
           "onboarding -> autorita' nulla");
    nearly(z.authorityVelocity(1.0, control::Phase::Interactive, 0.0), 1.0, 1e-12,
           "interattiva -> comanda l'input");

    // Crossfade di autorita' in handover: da auto a input.
    nearly(z.authorityVelocity(0.0, control::Phase::Handover, 0.0),
           config::kHookAutoVelocity, 1e-12, "handover a t=0 -> tutta autorita' scriptata");
    nearly(z.authorityVelocity(0.0, control::Phase::Handover, config::kPhaseHandoverS),
           0.0, 1e-12, "handover a fine -> tutta autorita' all'input");

    // Detent: sotto la soglia di aggancio la velocita' viene azzerata.
    control::ZoomController d;
    const double weak = config::kEnterHoldFrac * config::kExtremaGain * 0.5;
    nearly(d.applyHoldSelect(weak, kDt), 0.0, 1e-12, "velocita' debole -> aggancio del detent");
    check(d.locked(), "il detent risulta agganciato");

    // Sopra la soglia di sgancio si torna liberi.
    const double strong = config::kBreakHoldFrac * config::kExtremaGain * 1.1;
    nearly(d.applyHoldSelect(strong, kDt), strong, 1e-12, "spinta forte -> sgancio del detent");
    check(!d.locked(), "il detent risulta sganciato");

    // Integrazione: concentrandosi il focus sale e resta nel dominio [0,1].
    control::ZoomController run;
    for (int i = 0; i < 600; ++i) {
        run.update(config::kExtremaGain, 1.0 / 60.0, control::Phase::Interactive, 0.0);
    }
    check(run.targetFocus() > 0.0, "velocita' positiva sostenuta -> il focus avanza");
    check(run.targetFocus() <= 1.0 && run.currentFocus() <= 1.0, "il focus resta in [0,1]");

    // Crossfade coerente col focus.
    const auto cf = run.crossfade();
    check(cf.activeIndex >= 0 && cf.activeIndex <= config::kTotalImages - 2,
          "indice dello sprite attivo nel range valido");
    nearly(cf.activeAlpha + cf.nextAlpha, 1.0, 1e-12, "gli alpha del crossfade sommano a 1");
    check(cf.magnification >= config::kScaleLabels[0],
          "magnificazione non inferiore al livello di partenza");

    // L'outro e' scriptato: converge a prescindere dall'input.
    control::ZoomController outro;
    for (int i = 0; i < 3000; ++i) {
        outro.update(0.0, 1.0 / 60.0, control::Phase::Outro, 0.0);
    }
    nearly(outro.targetFocus(), config::kOutroTargetFocus, 0.05,
           "l'outro converge sul focus finale");
}

} // namespace

int main() {
    std::printf("Mind Zoom - test del core nativo\n");

    testDecode();
    testStft();
    testGating();
    testCalibration();
    testCalibrationFailures();
    testExtremaVelocity();
    testSmoother();
    testZoom();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
