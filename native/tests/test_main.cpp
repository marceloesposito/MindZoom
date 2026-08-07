// Test numerici del core nativo. Rispecchiano tests/control-pipeline.test.js e
// tests/stft.test.js: stesse attese sugli stessi valori, così il port si valida
// contro la pipeline JS già tarata sul campo invece che contro sé stesso.

#include "config.hpp"
#include "control/calibration.hpp"
#include "control/tunables.hpp"
#include "control/zoom.hpp"
#include "dsp/decode.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include "util/smoothing.hpp"

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

/** Manopole ai valori di default: è quello che il programma usa all'avvio. */
const control::Tunables kTune{};

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
    nearly(narrow.velocity(5.0, kDt, kTune), 0.0, 1e-12,
           "calibrazione fallita -> velocita' nulla");

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
    const double first = cal.velocity(1.4, kDt, kTune);
    check(first > 0.0, "nessun warm-up: zoom in gia' al primo campione");

    // All'estremo assoluto ampiezza e gate valgono entrambi 1: velocita' piena.
    control::Calibration sat;
    calibrateTo(sat, 0.5, 1.5);
    nearly(sat.velocity(1.5, kDt, kTune), kTune.gain(), 1e-9,
           "saturazione al gain pieno all'estremo assoluto");

    // Zona morta: sul neutro esatto il controllo e' fermo, senza deriva.
    control::Calibration dead;
    calibrateTo(dead, 0.5, 1.5);
    nearly(dead.velocity(1.0, kDt, kTune), 0.0, 1e-12,
           "sul neutro -> velocita' esattamente nulla (zona morta)");

    // Monotonia: piu' ci si allontana dal neutro, piu' si va veloci.
    control::Calibration a, b;
    calibrateTo(a, 0.5, 1.5);
    calibrateTo(b, 0.5, 1.5);
    check(a.velocity(1.15, kDt, kTune) < b.velocity(1.30, kDt, kTune),
          "velocita' monotona nella distanza dal neutro");

    // Sotto il minimo locale si va in zoom out.
    control::Calibration out;
    calibrateTo(out, 0.5, 1.5);
    const double back = out.velocity(0.6, kDt, kTune);
    check(back < 0.0, "sotto il minimo locale -> zoom out");
    nearly(back, -kTune.gain(), 1e-9, "zoom out saturato vicino all'estremo basso");

    // La banda locale non esce mai dagli estremi assoluti.
    control::Calibration clampd;
    calibrateTo(clampd, 0.5, 1.5);
    clampd.velocity(99.0, kDt, kTune);
    check(clampd.localMax() <= 1.5 + 1e-12, "il massimo locale non supera l'assoluto");
    clampd.velocity(-99.0, kDt, kTune);
    check(clampd.localMin() >= 0.5 - 1e-12, "il minimo locale non scende sotto l'assoluto");

    // Carattere fasico conservato: un calo VERO chiude comunque il gate.
    control::Calibration drop;
    calibrateTo(drop, 0.5, 1.5);
    check(drop.velocity(1.4, kDt, kTune) > 0.0, "spinta iniziale -> zoom in");
    nearly(drop.velocity(1.15, kDt, kTune), 0.0, 1e-12,
           "calo netto sotto la banda -> velocita' nulla, il controllo resta fasico");
}

void testTolerance() {
    group("9. Tolleranza di attivazione");

    // Questo e' IL comportamento nuovo. Col bordo netto di prima, un indice che
    // scendeva anche di poco sotto il massimo locale dava esattamente 0: l'uscita
    // era un segnale commutato, non un controllo.
    control::Calibration cal;
    calibrateTo(cal, 0.5, 1.5);
    cal.velocity(1.4, kDt, kTune);                     // fissa il massimo locale
    const double partial = cal.velocity(1.25, kDt, kTune);
    check(partial > 0.0, "poco sotto il massimo locale -> autorita' PARZIALE, non zero");
    check(partial < kTune.gain(), "l'autorita' parziale resta sotto il gain pieno");

    // Con tolleranza nulla si torna esattamente al gradino di prima: e' la prova
    // che la tolleranza e' l'unica cosa che cambia.
    control::Tunables hard = kTune;
    hard.localTolerance = 0.0;
    control::Calibration step;
    calibrateTo(step, 0.5, 1.5);
    step.velocity(1.4, kDt, hard);
    nearly(step.velocity(1.25, kDt, hard), 0.0, 1e-12,
           "tolleranza 0 -> ritorna il gradino netto");

    // Le due componenti sono esposte separatamente: servono a capire QUALE delle
    // due sta bloccando, perche' richiedono correzioni opposte.
    check(cal.gate() > 0.0 && cal.gate() < 1.0, "gate parziale osservabile");
    check(cal.magnitude() > 0.0, "ampiezza osservabile");
}

void testContinuity() {
    group("10. Continuita' dell'uscita");

    // Un indice che oscilla attorno a un valore alto: e' il caso in cui la legge
    // precedente produceva il pettine, alternando zero e velocita' piena.
    control::Calibration cal;
    calibrateTo(cal, 0.5, 1.5);

    int    zeros   = 0;
    double maxJump = 0.0;
    double prev    = 0.0;
    for (int i = 0; i < 120; ++i) {
        const double c = 1.28 + 0.05 * std::sin(2.0 * std::numbers::pi * i / 9.0);
        const double v = cal.velocity(c, kDt, kTune);
        if (i > 12) {                       // scarta il transitorio di aggancio
            if (v == 0.0) ++zeros;
            maxJump = std::max(maxJump, std::fabs(v - prev));
        }
        prev = v;
    }
    check(zeros == 0, "su segnale oscillante l'uscita non collassa mai a zero");
    check(maxJump < 0.15 * kTune.gain(),
          "nessuno scalino oltre il 15% del gain fra campioni adiacenti");
}

void testSmoother() {
    group("7. Denoise dell'indice");

    control::IndexSmoother s;
    check(!s.initialized(), "non inizializzato prima del primo campione");
    nearly(s.push(2.0, kDt), 2.0, 1e-12, "il primo campione inizializza senza transitorio");

    // Reiezione dei campioni anomali: e' il motivo per cui la mediana sta PRIMA
    // dell'EMA. Un EMA da solo lo attenuerebbe ma poi lo spalmerebbe sulla coda.
    control::IndexSmoother spike;
    for (int i = 0; i < config::kIndexMedianTaps; ++i) spike.push(2.0, kDt);
    const double before = spike.value();
    const double after  = spike.push(50.0, kDt);
    nearly(after, before, 1e-9, "un singolo campione anomalo non passa la mediana");

    // Convergenza: su un ingresso costante l'uscita ci arriva.
    control::IndexSmoother conv;
    for (int i = 0; i < 400; ++i) conv.push(3.0, kDt);
    nearly(conv.value(), 3.0, 1e-3, "su ingresso costante l'uscita converge");

    // Due poli: l'uscita non insegue lo scalino quanto un polo singolo.
    control::IndexSmoother slope;
    for (int i = 0; i < config::kIndexMedianTaps; ++i) slope.push(0.0, kDt);
    for (int i = 0; i < config::kIndexMedianTaps; ++i) slope.push(1.0, kDt);
    check(slope.value() > 0.0 && slope.value() < 1.0,
          "l'uscita resta fra i due livelli durante la transizione");
}

void testFrameRateIndependence() {
    group("12. Indipendenza dal frame rate");

    // Stesso tempo simulato, tre frequenze di frame diverse: lo zoom deve
    // arrivare allo stesso punto. Prima le costanti si applicavano PER
    // CHIAMATA, quindi un monitor a 144 Hz correva 2.4 volte piu' veloce.
    const auto runFor = [](double dt, int steps) {
        control::ZoomController z;
        for (int i = 0; i < steps; ++i) {
            z.update(kTune.gain(), dt, control::Phase::Interactive, 0.0, kTune);
        }
        return z.targetFocus();
    };

    const double at60  = runFor(1.0 / 60.0,  600);   // 10 s
    const double at144 = runFor(1.0 / 144.0, 1440);  // 10 s
    const double at30  = runFor(1.0 / 30.0,  300);   // 10 s

    nearly(at144, at60, 1e-6, "144 Hz e 60 Hz percorrono lo stesso focus in 10 s");
    nearly(at30,  at60, 1e-6, "30 Hz e 60 Hz percorrono lo stesso focus in 10 s");
    check(at60 > 0.0, "il focus e' effettivamente avanzato");

    // L'easing di currentFocus segue la stessa regola, ma non puo' combaciare
    // alla cifra: rateAdjust e' esatto quando il bersaglio sta fermo, e qui il
    // bersaglio si muove DENTRO il passo. Il residuo e' del secondo ordine in dt
    // - qui vale lo 0.05% - e non e' percepibile. La tolleranza dice questo, non
    // nasconde un errore.
    const auto easedFor = [](double dt, int steps) {
        control::ZoomController z;
        for (int i = 0; i < steps; ++i) {
            z.update(kTune.gain(), dt, control::Phase::Interactive, 0.0, kTune);
        }
        return z.currentFocus();
    };
    const double eased60  = easedFor(1.0 / 60.0,  600);
    const double eased144 = easedFor(1.0 / 144.0, 1440);
    check(std::fabs(eased144 - eased60) < 0.005 * eased60,
          "l'easing del focus resta entro lo 0.5% fra 144 Hz e 60 Hz");
}

void testSmoothingPrimitives() {
    group("11. Primitive di smoothing");

    // Le costanti di tempo devono dare lo stesso risultato a rate diversi: e'
    // tutto il motivo per cui non si usano gli alfa.
    util::Ema fast, slow;
    for (int i = 0; i < 600; ++i) fast.push(1.0, 1.0 / 60.0, 0.5);
    for (int i = 0; i < 60;  ++i) slow.push(1.0, 1.0 / 6.0,  0.5);
    nearly(fast.value(), slow.value(), 1e-3,
           "stessa costante di tempo, rate diversi -> stesso valore a 10 s");

    nearly(util::smoothstep01(0.0), 0.0, 1e-12, "smoothstep(0) = 0");
    nearly(util::smoothstep01(1.0), 1.0, 1e-12, "smoothstep(1) = 1");
    nearly(util::smoothstep01(0.5), 0.5, 1e-12, "smoothstep(0.5) = 0.5");
    nearly(util::smoothstep01(-3.0), 0.0, 1e-12, "smoothstep satura sotto zero");
    nearly(util::smoothstep01(3.0), 1.0, 1e-12, "smoothstep satura sopra uno");

    util::MedianFilter<5> med;
    med.push(1.0); med.push(1.0); med.push(1.0); med.push(1.0);
    nearly(med.push(99.0), 1.0, 1e-12, "la mediana ignora il valore isolato");

    // Decadimento verso zero, per quando il segnale sorgente manca.
    util::Ema fade;
    fade.set(1.0);
    for (int i = 0; i < 100; ++i) fade.decay(1.0 / 60.0, 0.2);
    check(fade.value() < 0.01, "senza segnale la velocita' decade a zero");
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

    // Detent: l'aggancio richiede PERMANENZA. Un singolo frame sotto soglia non
    // e' un utente fermo, e' un attraversamento dello zero: agganciare subito
    // annullava il movimento appena guadagnato.
    control::ZoomController d;
    const double weak = config::kEnterHoldFrac * kTune.gain() * 0.5;
    d.applyHoldSelect(weak, kDt, kTune);
    check(!d.locked(), "un solo frame sotto soglia NON aggancia");

    double waited = kDt;
    while (waited < config::kEnterHoldDwellS + kDt) {
        d.applyHoldSelect(weak, kDt, kTune);
        waited += kDt;
    }
    check(d.locked(), "dopo la permanenza richiesta il detent aggancia");

    // Sopra la soglia di sgancio si torna liberi.
    const double strong = config::kBreakHoldFrac * kTune.gain() * 1.1;
    nearly(d.applyHoldSelect(strong, kDt, kTune), strong, 1e-12,
           "spinta forte -> sgancio del detent");
    check(!d.locked(), "il detent risulta sganciato");

    // Refrattarieta': dopo lo sgancio si ottiene una finestra di movimento vera,
    // non un singolo frame.
    for (int i = 0; i < 4; ++i) d.applyHoldSelect(weak, kDt, kTune);
    check(!d.locked(), "durante la refrattarieta' il lock non torna");
    check(d.refractory() > 0.0, "la finestra refrattaria e' in corso");

    // Interruttore diagnostico: con hold spento la velocita' passa intatta.
    control::ZoomController off;
    control::Tunables noHold = kTune;
    noHold.holdEnabled = false;
    for (int i = 0; i < 20; ++i) {
        nearly(off.applyHoldSelect(weak, kDt, noHold), weak, 1e-12,
               "hold spento -> la velocita' passa senza gating");
        break;   // una asserzione basta, il resto e' solo per verificare il non-lock
    }
    for (int i = 0; i < 20; ++i) off.applyHoldSelect(weak, kDt, noHold);
    check(!off.locked(), "hold spento -> non aggancia mai");

    // Integrazione: concentrandosi il focus sale e resta nel dominio [0,1].
    control::ZoomController run;
    for (int i = 0; i < 600; ++i) {
        run.update(kTune.gain(), 1.0 / 60.0, control::Phase::Interactive, 0.0, kTune);
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
        outro.update(0.0, 1.0 / 60.0, control::Phase::Outro, 0.0, kTune);
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
    testTolerance();
    testContinuity();
    testFrameRateIndependence();
    testSmoothingPrimitives();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
