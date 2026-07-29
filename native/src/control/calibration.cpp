#include "control/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mz::control {
namespace {

constexpr double clamp01(double x) noexcept {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

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

double IndexSmoother::push(double index) noexcept {
    if (!init_) {
        value_ = index;
        init_  = true;
    } else {
        value_ += (index - value_) * config::kCalibIndexEma;
    }
    return value_;
}

void Calibration::start() {
    peak_   = kNegInf;
    trough_ = kPosInf;
    valid_  = false;
    message_.clear();
    enterStage(CalibStage::Concentrate);
}

void Calibration::enterStage(CalibStage stage) {
    stage_        = stage;
    stageElapsed_ = 0.0;
    doneTimer_    = 0.0;
    runMin_       = kPosInf;
    runMax_       = kNegInf;
    displayTarget_ = 0.5;
}

double Calibration::stageDuration() const noexcept {
    switch (stage_) {
        case CalibStage::Concentrate: return config::kCalibConcentrateS;
        case CalibStage::Relax:       return config::kCalibRelaxS;
        default:                      return 0.0;
    }
}

void Calibration::sample(double c) {
    if (stage_ != CalibStage::Concentrate && stage_ != CalibStage::Relax) return;

    // Il display si auto-scala sul range visto nella fase, così il binario resta
    // leggibile anche prima di conoscere gli estremi assoluti.
    runMin_ = std::min(runMin_, c);
    runMax_ = std::max(runMax_, c);
    displayTarget_ = (runMax_ > runMin_) ? clamp01((c - runMin_) / (runMax_ - runMin_)) : 0.5;

    // Gli estremi ASSOLUTI si registrano solo dopo il lead-in, per non catturare
    // il transitorio di reazione al prompt.
    if (stageElapsed_ < config::kCalibLeadInS) return;

    if (stage_ == CalibStage::Concentrate) {
        peak_ = std::max(peak_, c);
    } else {
        trough_ = std::min(trough_, c);
    }
}

bool Calibration::tick(double dt, bool contactOk) {
    if (stage_ == CalibStage::Done) {
        doneTimer_ += dt;
        return false;
    }
    if (stage_ != CalibStage::Concentrate && stage_ != CalibStage::Relax) return false;

    // Contatto scarso: si mette in pausa il conteggio invece di consumarlo.
    if (!contactOk) return false;

    stageElapsed_ += dt;
    if (stageElapsed_ < stageDuration()) return false;

    if (stage_ == CalibStage::Concentrate) {
        enterStage(CalibStage::Relax);
    } else {
        finalize();
    }
    return true;
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

double Calibration::velocity(double c, double dt) {
    if (!valid_) return 0.0;

    const double span = absMax_ - absMin_;
    if (span <= 0.0) return 0.0;

    // Gli estremi locali decadono verso il segnale a kLocalDecay frazioni di span/s.
    const double step = config::kLocalDecay * dt * span;
    localMax_ = std::min(absMax_, std::max(c, localMax_ - step));
    localMin_ = std::max(absMin_, std::min(c, localMin_ + step));

    constexpr double gain = config::kExtremaGain;
    constexpr double frac = config::kCalibConcFraction;

    if (c >= localMax_) {
        const double denom = frac * (absMax_ - neutralM_);
        return (denom > 0.0) ? gain * clamp01((c - neutralM_) / denom) : 0.0;   // zoom in
    }
    if (c <= localMin_) {
        const double denom = frac * (neutralM_ - absMin_);
        return (denom > 0.0) ? -gain * clamp01((neutralM_ - c) / denom) : 0.0;  // zoom out
    }
    return 0.0;   // dentro la banda locale: fermo
}

} // namespace mz::control
