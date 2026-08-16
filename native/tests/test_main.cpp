// Test numerici del core nativo. Rispecchiano tests/control-pipeline.test.js e
// tests/stft.test.js: stesse attese sugli stessi valori, così il port si valida
// contro la pipeline JS già tarata sul campo invece che contro sé stesso.

#include "app/displays.hpp"
#include "ble/recording.hpp"
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

/**
 * Alimenta la STFT con una sinusoide, finché non emette un frame.
 *
 * Lo stesso ritmo arriva ai due frontali con AMPIEZZA DIVERSA (AF8 al 30% di
 * AF7). Prima arrivava identico su entrambi, il che e' fisicamente impossibile
 * - sono due elettrodi in due punti diversi del cranio - e con la derivazione
 * bipolare si annulla per costruzione: i test misuravano una situazione che non
 * puo' esistere, e sono crollati appena la derivazione e' diventata
 * differenziale. Ampiezze diverse sullo stesso ritmo e' cio' che succede
 * davvero, e lascia alla differenza uno spettro della stessa forma.
 */
inline constexpr double kAf8Rapporto = 0.30;

bool feedSine(dsp::SlidingStft& stft, double freqHz, double amplitudeUv, int samples) {
    bool emitted = false;
    for (int n = 0; n < samples; ++n) {
        const double phase = 2.0 * std::numbers::pi * freqHz * n / config::kSampleRate;
        const double v = amplitudeUv * std::sin(phase);

        std::array<double, config::kChannels> uv{};
        std::array<std::uint16_t, config::kChannels> adc{};
        for (std::size_t ch = 0; ch < config::kChannels; ++ch) {
            double q = 0.0;
            if (ch == static_cast<std::size_t>(config::kFrontalA))      q = v;
            else if (ch == static_cast<std::size_t>(config::kFrontalB)) q = v * kAf8Rapporto;

            uv[ch]  = q;
            adc[ch] = static_cast<std::uint16_t>(
                config::kAdcCenter + static_cast<int>(q / config::kAdcToMicrovolts));
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
    // Il punto di prova si ricava dalla rampa stessa, non da un numero magico:
    // cosi' il test non si rompe cambiando il decadimento della banda.
    const double tol   = kTune.localTolerance * (1.5 - 1.0);   // semi-span verso l'alto
    const double probe = 1.4 - 0.4 * tol;                      // dentro la rampa

    control::Calibration cal;
    calibrateTo(cal, 0.5, 1.5);
    cal.velocity(1.4, kDt, kTune);                     // fissa il massimo locale
    const double partial = cal.velocity(probe, kDt, kTune);
    check(partial > 0.0, "dentro la rampa di tolleranza -> autorita' PARZIALE, non zero");
    check(partial < kTune.gain(), "l'autorita' parziale resta sotto il gain pieno");

    // Con tolleranza nulla, lo STESSO ingresso da' esattamente zero: e' la prova
    // che a cambiare le cose e' la tolleranza e nient'altro.
    control::Tunables hard = kTune;
    hard.localTolerance = 0.0;
    control::Calibration step;
    calibrateTo(step, 0.5, 1.5);
    step.velocity(1.4, kDt, hard);
    nearly(step.velocity(probe, kDt, hard), 0.0, 1e-12,
           "tolleranza 0 sullo stesso ingresso -> ritorna il gradino netto");

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

    // La legge precedente, sullo stesso ingresso, alternava gain pieno e zero:
    // scalini del 100% fra campioni adiacenti. La soglia qui sotto non e' un
    // valore notevole, e' un ordine di grandezza: serve a dire che il pettine
    // non c'e' piu'. Il segnale di prova e' volutamente aggressivo (oscillazione
    // ampia quanto la tolleranza, periodo 1.7 s), piu' mosso di un indice reale.
    check(maxJump < 0.25 * kTune.gain(),
          "nessuno scalino oltre il 25% del gain fra campioni adiacenti");
    check(maxJump > 0.0, "il controllo si muove davvero (test non degenere)");
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

void testRecording() {
    group("13. Registrazione: giro completo scrittura -> lettura");

    const std::wstring path = L"mz_test_recording.mzr";

    // Campioni con valori tutti diversi: se il layout su file fosse sbagliato la
    // rilettura non darebbe un errore, darebbe dati plausibili ma falsi. Solo un
    // confronto campione per campione lo intercetta.
    std::vector<ble::Sample> written;
    for (int i = 0; i < 500; ++i) {
        ble::Sample s{};
        for (std::size_t ch = 0; ch < config::kChannels; ++ch) {
            s.uv[ch]  = i * 0.25 + static_cast<double>(ch);
            s.adc[ch] = static_cast<std::uint16_t>(config::kAdcCenter + i + ch);
        }
        written.push_back(s);
    }

    // Il file nasce al PRIMO campione: una sessione senza segnale non deve
    // lasciare un file vuoto, che poi ricompare nell'elenco e viene rifiutato.
    {
        const std::wstring empty = L"mz_test_vuota.mzr";
        _wremove(empty.c_str());
        {
            ble::Recorder rec;
            check(rec.arm(empty), "la registrazione si arma");
            check(!rec.hasData(), "armata ma senza dati finche' non arriva un campione");
        }
        check(ble::detail::openFile(empty, L"rb") == nullptr,
              "nessun campione -> nessun file creato");
    }

    {
        ble::Recorder rec;
        check(rec.arm(path), "la registrazione si arma sul percorso dato");
        for (const auto& s : written) rec.write(s);
        check(rec.hasData(), "dopo il primo campione la registrazione ha dati");
        check(rec.count() == written.size(), "il contatore segue i campioni scritti");
        nearly(rec.seconds(), static_cast<double>(written.size()) / config::kSampleRate,
               1e-9, "la durata si ricava dal numero di campioni");
    }   // il distruttore chiude il file

    const auto loaded = ble::loadRecording(path);
    check(loaded.ok, "la registrazione si rilegge");
    check(loaded.samples.size() == written.size(), "stesso numero di campioni");

    bool identical = (loaded.samples.size() == written.size());
    for (std::size_t i = 0; identical && i < written.size(); ++i) {
        for (std::size_t ch = 0; ch < config::kChannels; ++ch) {
            if (loaded.samples[i].uv[ch] != written[i].uv[ch] ||
                loaded.samples[i].adc[ch] != written[i].adc[ch]) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "ogni campione torna identico, canale per canale");

    // Un file estraneo va RIFIUTATO, non interpretato: leggere campioni con un
    // layout diverso produrrebbe EEG credibile e completamente inventato.
    {
        std::FILE* f = ble::detail::openFile(L"mz_test_garbage.mzr", L"wb");
        if (f) {
            const char junk[] = "questo non e' una registrazione";
            std::fwrite(junk, 1, sizeof(junk), f);
            std::fclose(f);
        }
    }
    const auto bad = ble::loadRecording(L"mz_test_garbage.mzr");
    check(!bad.ok, "un file estraneo viene rifiutato");
    check(!bad.error.empty(), "il rifiuto dice il motivo");

    const auto missing = ble::loadRecording(L"mz_test_non_esiste.mzr");
    check(!missing.ok, "un file inesistente viene rifiutato");

    // Un file con la sola intestazione: ben formato ma senza niente da
    // rigiocare. Va distinto da un file estraneo, perche' la causa e' un'altra
    // (sessione aperta senza fascia) e chi legge deve poterlo capire.
    {
        const std::wstring headerOnly = L"mz_test_solo_intestazione.mzr";
        std::FILE* f = ble::detail::openFile(headerOnly, L"wb");
        if (f) {
            const auto hdr = ble::makeRecordHeader();
            std::fwrite(&hdr, sizeof(hdr), 1, f);
            std::fclose(f);
        }
        const auto emptyRec = ble::loadRecording(headerOnly);
        check(!emptyRec.ok, "una registrazione senza campioni viene rifiutata");
        check(emptyRec.error != bad.error,
              "il motivo distingue 'vuota' da 'file estraneo'");
        _wremove(headerOnly.c_str());
    }

    _wremove(path.c_str());
    _wremove(L"mz_test_vuota.mzr");
    _wremove(L"mz_test_garbage.mzr");
}

/** Riempie la STFT con valori ADC dati da `gen`, finché non emette un frame. */
template <typename Gen>
void feedAdc(dsp::SlidingStft& stft, int samples, Gen gen) {
    for (int n = 0; n < samples; ++n) {
        std::array<double, config::kChannels> uv{};
        std::array<std::uint16_t, config::kChannels> adc{};
        for (std::size_t ch = 0; ch < config::kChannels; ++ch) {
            const std::uint16_t a = gen(n, static_cast<int>(ch));
            adc[ch] = a;
            uv[ch]  = dsp::toMicrovolts(dsp::centerSample(a));
        }
        stft.pushSample(uv, adc);
    }
}

/** Impacchetta valori a 14 bit LSB-first, come fa il dispositivo. */
void pack14(std::vector<std::uint8_t>& out, const std::vector<std::uint16_t>& vals) {
    std::size_t bit = 0;
    for (const auto v : vals) {
        for (int k = 0; k < 14; ++k) {
            const std::size_t byteIdx = (bit >> 3);
            if (out.size() <= byteIdx) out.resize(byteIdx + 1, 0);
            if (v & (1u << k)) out[byteIdx] |= static_cast<std::uint8_t>(1u << (bit & 7));
            ++bit;
        }
    }
}

void testAthenaPackets() {
    group("16. Formato dei pacchetti Athena");

    // Dimensioni MISURATE sui pacchetti veri (registrazione del 2026-08-16),
    // non dedotte dai bit che servirebbero. Le deduzioni erano sbagliate:
    // OPT16 "3 x 16 x 20 bit" darebbe 120, il pacchetto vero ne porta 40, e un
    // solo tipo di dimensione sbagliata spezza la catena anche per l'EEG.
    check(dsp::payloadBytes(dsp::PacketType::Eeg8) == 28, "EEG 8 canali -> 28 byte di payload");
    check(dsp::payloadBytes(dsp::PacketType::Eeg4) == 14, "EEG 4 canali -> 14 byte");
    check(dsp::payloadBytes(dsp::PacketType::Imu) == 36, "IMU -> 36 byte (misurato)");
    check(dsp::payloadBytes(dsp::PacketType::Optics16) == 40, "ottica 16 canali -> 40 byte (misurato)");
    check(dsp::payloadBytes(dsp::PacketType::DrlRef) == 24, "DRL/REF -> 24 byte (misurato)");
    check(dsp::payloadBytes(dsp::PacketType::Battery) == 20, "batteria -> 20 byte (misurato)");

    check(dsp::packetType(0x12) == dsp::PacketType::Eeg8,
          "etichetta 0x12 -> EEG 8 canali a 256 Hz");
    check(dsp::isEeg(dsp::packetType(0x12)), "0x12 e' EEG");
    check(!dsp::isEeg(dsp::packetType(0x17)), "0x17 e' IMU, non EEG");

    // Notifica sintetica: prefisso ignoto, poi EEG, IMU, EEG in catena.
    // È il punto centrale: il parser deve TROVARE dove comincia, perché
    // sbagliare il prefisso non produce un errore ma campioni falsi.
    const std::vector<std::uint16_t> eeg = {
        100, 200, 300, 400, 500, 600, 700, 800,
        150, 250, 350, 450, 550, 650, 750, 850};

    for (const std::size_t prefix : {std::size_t{9}, std::size_t{12}, std::size_t{21}}) {
        std::vector<std::uint8_t> pkt(prefix, 0xAA);   // prefisso qualunque

        pkt.push_back(0x12);                            // EEG 8 canali
        pkt.insert(pkt.end(), 4, 0x00);
        std::vector<std::uint8_t> body;
        pack14(body, eeg);
        body.resize(28, 0);
        pkt.insert(pkt.end(), body.begin(), body.end());

        pkt.push_back(0x17);                            // IMU
        pkt.insert(pkt.end(), 4, 0x00);
        pkt.insert(pkt.end(), 36, 0x33);

        pkt.push_back(0x12);                            // altro EEG
        pkt.insert(pkt.end(), 4, 0x00);
        pkt.insert(pkt.end(), body.begin(), body.end());

        // L'offset si decide osservando piu' notifiche, non una sola.
        dsp::ChainLocator loc;
        for (int k = 0; k < dsp::ChainLocator::kNeeded; ++k) loc.offer(pkt.data(), pkt.size());

        check(loc.locked(), "prefisso di " + std::to_string(prefix) +
                            " byte: l'offset viene deciso");
        check(loc.offset() == prefix, "l'offset deciso e' quello giusto");

        int samples = 0, packets = 0;
        const std::size_t used =
            dsp::walkPackets(pkt.data(), pkt.size(), loc.offset(), &samples, &packets);
        check(used == pkt.size() - prefix, "la catena consuma la notifica fino in fondo");
        check(packets == 3, "tre pacchetti percorsi: EEG, IMU, EEG");
        check(samples == 4, "due pacchetti EEG -> quattro campioni");
    }

    // I valori devono tornare identici a quelli impacchettati.
    {
        std::vector<std::uint8_t> body;
        pack14(body, eeg);
        bool same = true;
        for (std::size_t k = 0; k < eeg.size(); ++k) {
            if (dsp::unpack14(body.data(), body.size(), k * 14) != eeg[k]) same = false;
        }
        check(same, "i campioni a 14 bit tornano identici dopo il giro completo");
    }

    // Su rumore l'offset non deve MAI stabilizzarsi. E' il vincolo che rende la
    // cosa decidibile: su una notifica sola le coincidenze sono frequenti (piu'
    // di meta' delle volte), ma cadono ogni volta in un punto diverso, mentre
    // l'offset vero e' sempre lo stesso.
    {
        int locked = 0;
        std::uint32_t seed = 99;
        for (int trial = 0; trial < 40; ++trial) {
            dsp::ChainLocator loc;
            for (int k = 0; k < dsp::ChainLocator::kNeeded * 4; ++k) {
                std::vector<std::uint8_t> junk(64);
                for (auto& b : junk) {
                    seed = seed * 1664525u + 1013904223u;
                    b = static_cast<std::uint8_t>(seed >> 16);
                }
                loc.offer(junk.data(), junk.size());
            }
            if (loc.locked()) ++locked;
        }
        std::printf("        (offset stabilizzati su rumore: %d su 40)\n", locked);
        check(locked == 0, "su byte casuali l'offset non si stabilizza mai");
    }
}

void testPlausibility() {
    group("15. Plausibilita' fisica del segnale");

    // È il controllo che mancava. Una sessione reale è stata calibrata su
    // rumore: l'indice di Pope su rumore bianco vale 17/8 = 2.1, che sembra un
    // valore normale, e nessuna statistica a valle se ne accorgeva.

    // 1. Sinusoide: forma d'onda vera, fortemente correlata fra campioni.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        feedSine(stft, 10.0, 60.0, config::kStftWindow * 2);
        const auto q = gating.assess(stft);
        check(q.fault == dsp::SignalFault::None, "una forma d'onda vera passa il controllo");
        check(q.autocorr1 > 0.9, "autocorrelazione alta su segnale reale");
    }

    // 2. Rumore uniforme su tutto il fondo scala: è ESATTAMENTE quello che si è
    //    presentato sul campo. Campioni indipendenti, distribuzione piatta.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        std::uint32_t seed = 12345;
        feedAdc(stft, config::kStftWindow * 2, [&](int, int) {
            seed = seed * 1664525u + 1013904223u;      // deterministico
            return static_cast<std::uint16_t>((seed >> 8) & 0x3FFF);
        });
        const auto q = gating.assess(stft);
        check(q.fault == dsp::SignalFault::Uncorrelated,
              "rumore su tutto il fondo scala -> bocciato come 'non un segnale'");
        check(q.autocorr1 < 0.5, "autocorrelazione prossima a zero sul rumore");
        check(q.railFraction > 0.0, "il rumore tocca i fondo scala");
    }

    // 3. Canale piatto: elettrodo staccato.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        feedAdc(stft, config::kStftWindow * 2, [](int, int) {
            return static_cast<std::uint16_t>(config::kAdcCenter);
        });
        const auto q = gating.assess(stft);
        check(q.fault == dsp::SignalFault::Flat, "canale costante -> bocciato come piatto");
    }

    // 4. Onda vera ma satura: ampiezza oltre il fondo scala.
    //    I due frontali saturano a frequenze diverse: se saturasse solo uno, il
    //    criterio "il migliore dei due" prenderebbe l'altro e direbbe OK - che
    //    e' il comportamento voluto, ma non e' quello sotto esame qui.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        feedAdc(stft, config::kStftWindow * 2, [](int n, int ch) {
            const double f = (ch == config::kFrontalB) ? 7.0 : 6.0;
            const double s = std::sin(2.0 * std::numbers::pi * f * n / config::kSampleRate);
            const double v = config::kAdcCenter + s * config::kAdcCenter * 1.6;
            return static_cast<std::uint16_t>(std::clamp(v, 0.0, 16383.0));
        });
        const auto q = gating.assess(stft);
        check(q.fault == dsp::SignalFault::Railing, "onda che sbatte sui limiti -> saturo");
    }

    // 5. Il criterio non deve dipendere dal canale peggiore: un elettrodo storto
    //    non e' un formato sbagliato, e confonderli manderebbe a cercare il
    //    problema nel posto sbagliato.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        std::uint32_t seed = 777;
        feedAdc(stft, config::kStftWindow * 2, [&](int n, int ch) {
            if (ch == config::kFrontalA) {
                const double s = std::sin(2.0 * std::numbers::pi * 10.0 * n / config::kSampleRate);
                return static_cast<std::uint16_t>(config::kAdcCenter + s * 600.0);
            }
            seed = seed * 1664525u + 1013904223u;
            return static_cast<std::uint16_t>((seed >> 8) & 0x3FFF);
        });
        const auto q = gating.assess(stft);
        check(q.fault == dsp::SignalFault::None,
              "un frontale buono e uno rotto -> il segnale resta utilizzabile");
    }

    // 6. Solo ronzio di rete: e' il caso che passava indisturbato.
    //    Una sinusoide a 50 Hz campionata a 256 Hz ha autocorrelazione
    //    cos(2*pi*50/256) = 0.335, cioe' SOPRA kMinAutocorr1: il controllo
    //    precedente la dichiarava buona. Misurato sul campo il 2026-08-16:
    //    quattro canali su otto, dall'87% al 99% di potenza a 50 Hz, tutti con
    //    autocorrelazione 0.332.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        feedSine(stft, config::kMainsHz, 300.0, config::kStftWindow * 2);
        const auto q = gating.assess(stft);
        check(q.mainsFraction > 0.9, "ronzio puro: quasi tutta la potenza a 50 Hz");
        check(q.autocorr1 > config::kMinAutocorr1,
              "l'autocorrelazione da sola NON lo boccia: e' il motivo del controllo");
        check(q.fault == dsp::SignalFault::Mains,
              "solo rete -> bocciato come 'RETE 50 Hz'");
    }

    // 7. IL CASO CHE CONTA: rete in MODO COMUNE - identica sui due frontali,
    //    com'e' nella realta' - sovrapposta a un EEG differenziale.
    //
    //    Misurato sul campo il 2026-08-16: fra AF7 e AF8 il ronzio ha 1,4 gradi
    //    di sfasamento e rapporto di ampiezza 1,01. La derivazione bipolare lo
    //    cancella, e infatti sui dati veri la quota di rete passa dall'81%
    //    all'8%. Qui la rete e' DIECI VOLTE l'EEG: sui canali presi
    //    singolarmente dominerebbe, sulla differenza deve sparire.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        feedAdc(stft, config::kStftWindow * 2, [](int n, int ch) {
            const double t    = static_cast<double>(n) / config::kSampleRate;
            const double rete = std::sin(2.0 * std::numbers::pi * config::kMainsHz * t) * 3000.0;
            const double eeg  = (ch == config::kFrontalA)
                                    ? std::sin(2.0 * std::numbers::pi * 10.0 * t) * 300.0
                                    : 0.0;
            return static_cast<std::uint16_t>(
                std::clamp(config::kAdcCenter + eeg + rete, 0.0, 16383.0));
        });
        const auto q = gating.assess(stft);
        check(q.mainsFraction < 0.10,
              "rete di modo comune: sulla derivazione bipolare quasi sparisce");
        check(q.fault == dsp::SignalFault::None,
              "rete dieci volte l'EEG, ma di modo comune -> segnale utilizzabile");
    }

    // 8. Rete di modo comune SENZA alcun EEG: la differenza e' nulla e non c'e'
    //    niente da leggere. Deve dirlo, non inventarsi un indice.
    {
        dsp::SlidingStft stft;
        dsp::Gating gating;
        feedAdc(stft, config::kStftWindow * 2, [](int n, int) {
            const double t = static_cast<double>(n) / config::kSampleRate;
            return static_cast<std::uint16_t>(std::clamp(
                config::kAdcCenter +
                    std::sin(2.0 * std::numbers::pi * config::kMainsHz * t) * 3000.0,
                0.0, 16383.0));
        });
        const auto q = gating.assess(stft);
        check(q.fault != dsp::SignalFault::None,
              "solo rete di modo comune -> nessun segnale utilizzabile");
    }
}

void testDisplays() {
    group("14. Enumerazione degli schermi");

    const auto displays = app::enumerateDisplays();
    std::printf("        (rilevati %zu schermi su questa macchina)\n", displays.size());

    check(!displays.empty(), "almeno uno schermo viene rilevato");
    if (displays.empty()) return;

    check(displays[0].primary, "il primario e' il primo: e' il candidato per l'operatore");

    bool sane = true;
    for (const auto& d : displays) {
        if (d.width() <= 0 || d.height() <= 0 || d.deviceName.empty()) sane = false;
    }
    check(sane, "ogni schermo ha nome e dimensioni sensate");
    check(!displays[0].describe().empty(), "la descrizione non e' vuota");

    // La firma deve cambiare col layout: e' cio' su cui si decide se la scelta
    // memorizzata e' ancora valida. Se non cambiasse, si proietterebbe su uno
    // schermo che non e' piu' quello di prima.
    const auto sig = app::layoutSignature(displays);
    check(!sig.empty(), "la firma del layout non e' vuota");
    check(app::layoutSignature(displays) == sig, "la firma e' stabile a parita' di layout");

    auto moved = displays;
    moved[0].bounds.right += 1;
    check(app::layoutSignature(moved) != sig, "la firma cambia se cambia una risoluzione");

    auto fewer = displays;
    fewer.pop_back();
    check(app::layoutSignature(fewer) != sig, "la firma cambia se cambia il numero di schermi");
}

void testNotch() {
    group("17. Notch sulla frequenza di rete");

    // Il difetto che questo test avrebbe intercettato: i coefficienti portati
    // dal JavaScript erano un notch a 55 Hz, e a 50 Hz toglievano 1 dB su 20
    // possibili. Erano scritti come costanti, quindi nessuno li ha mai
    // MISURATI - si e' guardato il nome della variabile, non la sua risposta.
    // Qui si misura il comportamento, che e' l'unica cosa che conta.

    auto guadagno = [](double freq) {
        auto  filtro = dsp::makeNotch();
        double picco = 0.0;
        const int n = config::kSampleRate * 4;      // 4 s: il transitorio si esaurisce
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / config::kSampleRate;
            const double y = filtro.process(std::sin(2.0 * std::numbers::pi * freq * t));
            if (i > config::kSampleRate) picco = std::max(picco, std::fabs(y));
        }
        return picco;
    };

    check(guadagno(config::kMainsHz) < 0.05,
          "la frequenza di rete viene attenuata di almeno 26 dB");

    // E, altrettanto importante, la banda che l'indice usa NON deve essere
    // toccata: un notch troppo largo falserebbe l'indice invece di pulirlo.
    for (const double f : {4.0, 10.0, 20.0, 30.0}) {
        check(guadagno(f) > 0.95,
              "a " + std::to_string(static_cast<int>(f)) +
                  " Hz il segnale passa quasi intatto");
    }

    // Il vecchio filtro passava questo confronto solo perche' nessuno lo faceva.
    check(guadagno(config::kMainsHz) < guadagno(30.0) * 0.1,
          "la rete e' attenuata almeno dieci volte piu' del bordo della banda beta");
}

} // namespace

int main() {
    // Senza questo, con l'output rediretto su file la printf e' bufferizzata a
    // blocchi: se un test fa crashare il processo non si vede NIENTE, nemmeno i
    // test passati prima, e non si sa da dove cominciare a guardare.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

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
    testRecording();
    testAthenaPackets();
    testPlausibility();
    testNotch();
    testDisplays();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
