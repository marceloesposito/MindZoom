#include "control/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mz::control {
namespace {

using util::clamp01;
using util::smoothstep01;

constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kPosInf =  std::numeric_limits<double>::infinity();

} // namespace

const char* toString(CalibStage stage) noexcept {
    switch (stage) {
        case CalibStage::Intro:       return "INTRO";
        case CalibStage::Concentrate: return "CONCENTRATE";
        case CalibStage::Relax:       return "RELAX";
        case CalibStage::Done:        return "DONE";
        case CalibStage::Failed:      return "FAILED";
    }
    return "?";
}

double IndexSmoother::push(double index, double dt) noexcept {
    // Mediana prima, EMA dopo: invertendoli il campione anomalo entrerebbe
    // comunque nell'EMA e ne uscirebbe spalmato sulla coda invece che rimosso.
    return ema_.push(median_.push(index), dt, config::kIndexTauS);
}

// ---------------------------------------------------------------------------
// Statistiche di fase
// ---------------------------------------------------------------------------

void PhaseStats::push(double x) noexcept {
    if (hasLast) sumProd += last * x;
    last = x;
    hasLast = true;
    ++n;
    sum   += x;
    sumSq += x * x;
}

double PhaseStats::mean() const noexcept {
    return (n > 0) ? (sum / n) : 0.0;
}

double PhaseStats::variance() const noexcept {
    if (n < 2) return 0.0;
    const double m = mean();
    const double v = (sumSq / n) - (m * m);
    return (v > 0.0) ? v : 0.0;
}

double PhaseStats::autocorr1() const noexcept {
    if (n < 3) return 0.0;
    const double m = mean();
    const double var = variance();
    if (var <= 1e-12) return 0.0;
    // Stimatore per grandi n: i termini di bordo (primo e ultimo campione)
    // pesano O(1/n) e si trascurano. Con le decine di campioni in gioco lo
    // scarto è irrilevante rispetto alla decisione che questo numero guida.
    const double cov = (sumProd / (n - 1)) - (m * m);
    const double r   = cov / var;
    return std::clamp(r, 0.0, 0.99);
}

double PhaseStats::effectiveN() const noexcept {
    if (n <= 0) return 0.0;
    const double rho = autocorr1();
    const double eff = n * (1.0 - rho) / (1.0 + rho);
    return std::clamp(eff, 1.0, static_cast<double>(n));
}

double separationT(const PhaseStats& a, const PhaseStats& b) noexcept {
    const double na = a.effectiveN();
    const double nb = b.effectiveN();
    if (na < 2.0 || nb < 2.0) return 0.0;
    const double delta = std::fabs(a.mean() - b.mean());
    const double se = std::sqrt(a.variance() / na + b.variance() / nb);
    if (se <= 1e-12) {
        // Due fasi perfettamente costanti con medie diverse sono separate in
        // modo perfetto, non indeterminato. Restituire zero qui manderebbe la
        // calibrazione a sbattere contro la rete di sicurezza proprio nel caso
        // piu' pulito possibile.
        return (delta > 1e-12) ? 1e6 : 0.0;
    }
    return delta / se;
}

// ---------------------------------------------------------------------------

void Calibration::start() {
    peak_   = kNegInf;
    trough_ = kPosInf;
    valid_  = false;
    message_.clear();
    concStats_.reset();
    relaxStats_.reset();
    enterStage(CalibStage::Concentrate);
}

void Calibration::enterStage(CalibStage stage) {
    stage_        = stage;
    stageElapsed_ = 0.0;
    doneTimer_    = 0.0;
    runMin_       = kPosInf;
    runMax_       = kNegInf;
    displayTarget_ = 0.5;
    phaseSamples_  = 0;
}

double Calibration::effectiveSamples() const noexcept {
    if (stage_ == CalibStage::Concentrate) return concStats_.effectiveN();
    if (stage_ == CalibStage::Relax)       return relaxStats_.effectiveN();
    return 0.0;
}

double Calibration::separation() const noexcept {
    if (stage_ != CalibStage::Relax) return 0.0;
    return separationT(concStats_, relaxStats_);
}

double Calibration::progress() const noexcept {
    if (stage_ == CalibStage::Concentrate) {
        return clamp01(concStats_.effectiveN() / config::kCalibTargetEffSamples);
    }
    if (stage_ == CalibStage::Relax) {
        const double perCampioni = relaxStats_.effectiveN() / config::kCalibTargetEffSamples;
        const double perSepar    = separationT(concStats_, relaxStats_) /
                                   config::kCalibMinSeparationT;
        return clamp01(std::min(perCampioni, perSepar));
    }
    return (stage_ == CalibStage::Done) ? 1.0 : 0.0;
}

void Calibration::sample(double c, bool usable) {
    if (stage_ != CalibStage::Concentrate && stage_ != CalibStage::Relax) return;
    if (!usable) return;   // non si conta ciò che non vale

    ++phaseSamples_;

    // Il display si auto-scala sul range visto nella fase, così il binario resta
    // leggibile anche prima di conoscere gli estremi assoluti.
    runMin_ = std::min(runMin_, c);
    runMax_ = std::max(runMax_, c);
    displayTarget_ = (runMax_ > runMin_) ? clamp01((c - runMin_) / (runMax_ - runMin_)) : 0.5;

    // I primi campioni sono il transitorio di reazione al prompt, non lo stato
    // da misurare: si scartano, come faceva il lead-in a tempo.
    if (phaseSamples_ <= config::kCalibLeadInSamples) return;

    if (stage_ == CalibStage::Concentrate) {
        concStats_.push(c);
        peak_ = std::max(peak_, c);
    } else {
        relaxStats_.push(c);
        trough_ = std::min(trough_, c);
    }
}

bool Calibration::tick(double dt, bool usable) {
    if (stage_ == CalibStage::Done) {
        doneTimer_ += dt;
        return false;
    }
    if (stage_ != CalibStage::Concentrate && stage_ != CalibStage::Relax) return false;

    // Il tempo si accumula solo mentre il segnale vale: serve alla rete di
    // sicurezza, che deve misurare quanto si è provato davvero, non quanto si è
    // aspettato con la fascia storta.
    if (usable) stageElapsed_ += dt;

    if (stage_ == CalibStage::Concentrate) {
        if (concStats_.effectiveN() >= config::kCalibTargetEffSamples) {
            enterStage(CalibStage::Relax);
            return true;
        }
    } else {
        const bool abbastanza = relaxStats_.effectiveN() >= config::kCalibTargetEffSamples;
        const bool separate   = separationT(concStats_, relaxStats_) >=
                                config::kCalibMinSeparationT;
        if (abbastanza && separate) {
            finalize();
            return true;
        }
    }

    // Rete di sicurezza. Nella prima fase non si può ancora parlare di
    // separazione, quindi il motivo è diverso e va detto diversamente.
    if (stageElapsed_ > config::kCalibMaxPhaseS) {
        if (stage_ == CalibStage::Concentrate) {
            fail("Il segnale non si stabilizza abbastanza da poterci misurare "
                 "qualcosa. Controlla il contatto della fascia e riprova.");
        } else {
            fail("Modulazione troppo debole: le due fasi non si distinguono. "
                 "Marca di piu' la differenza fra concentrazione e rilassamento.");
        }
        return true;
    }
    return false;
}

void Calibration::finalize() {
    if (!std::isfinite(peak_) || !std::isfinite(trough_)) {
        fail("Segnale assente durante la calibrazione.");
        return;
    }

    const double M    = (peak_ + trough_) / 2.0;
    const double span = peak_ - trough_;
    const double minSpan = config::kCalibMinSpanRel * std::max(1e-6, std::fabs(M));

    if (span < minSpan) {
        fail("Modulazione troppo debole: marca di più la differenza fra "
             "concentrazione e rilassamento.");
        return;
    }

    absMax_   = peak_;
    absMin_   = trough_;
    neutralM_ = M;
    localMax_ = M;   // la banda locale parte dal neutro
    localMin_ = M;
    valid_    = true;
    stage_    = CalibStage::Done;
    doneTimer_ = 0.0;
    message_  = "Calibrazione completata";
}

void Calibration::fail(std::string reason) {
    valid_   = false;
    stage_   = CalibStage::Failed;
    message_ = std::move(reason);
}

double Calibration::velocity(double c, double dt, const Tunables& t) {
    lastGate_ = 0.0;
    lastMag_  = 0.0;
    if (!valid_) return 0.0;

    const double span = absMax_ - absMin_;
    if (span <= 0.0) return 0.0;

    // Gli estremi locali decadono verso il segnale a localDecay frazioni di span/s.
    const double step = t.localDecay * dt * span;
    localMax_ = std::min(absMax_, std::max(c, localMax_ - step));
    localMin_ = std::max(absMin_, std::min(c, localMin_ + step));

    const bool   up   = (c >= neutralM_);
    const double half = up ? (absMax_ - neutralM_) : (neutralM_ - absMin_);
    if (half <= 0.0) return 0.0;

    // --- 1. AMPIEZZA: distanza dal neutro in unità personali ---
    // u vale 1 alla saturazione, cioè a concFraction del tragitto M->estremo.
    // Sotto la zona morta l'ampiezza è nulla, e la rampa liscia evita che il
    // bordo della zona morta diventi a sua volta un gradino.
    const double u   = clamp01(std::fabs(c - neutralM_) / (t.concFraction * half));
    const double mag = (u <= t.deadzone)
        ? 0.0
        : smoothstep01((u - t.deadzone) / (1.0 - t.deadzone));

    // --- 2. GATE: quanto la banda locale lascia passare ---
    // Il bordo non è più `c >= localMax_` ma una rampa larga `tol`: stare VICINO
    // al proprio massimo recente dà già autorità parziale. Con tolerance = 0 si
    // ritorna esattamente al gradino di prima.
    const double tol  = std::max(1e-9, t.localTolerance * half);
    const double gate = up ? smoothstep01((c - (localMax_ - tol)) / tol)
                           : smoothstep01(((localMin_ + tol) - c) / tol);

    lastGate_ = gate;
    lastMag_  = mag;

    return (up ? 1.0 : -1.0) * t.gain() * mag * gate;
}

} // namespace mz::control
