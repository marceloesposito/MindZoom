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
        case CalibStage::Prepare:     return "PREPARE";
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
    usingFallback_ = false;
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
    phaseSamples_    = 0;
    relaxBestEffN_   = 0.0;
    concBestEffN_    = 0.0;
    bestSeparation_  = 0.0;
}

double Calibration::effectiveSamples() const noexcept {
    // Il massimo storico, non il valore dal vivo, in entrambe le fasi: quello
    // che l'utente vede non deve mai calare, anche se la stima al volo
    // oscilla - vedi relaxBestEffN_/concBestEffN_.
    if (stage_ == CalibStage::Concentrate) return concBestEffN_;
    if (stage_ == CalibStage::Relax)       return relaxBestEffN_;
    return 0.0;
}

double Calibration::separation() const noexcept {
    // Ha senso solo a Relax ormai fatta: e' la SECONDA fase adesso, quella che
    // confronta la propria statistica con concStats_ (gia' completa a questo
    // punto). Prima (durante Concentrate) relaxStats_ e' ancora vuota e
    // separationT tornerebbe 0 comunque, ma dirlo esplicitamente evita di
    // doverci ripensare quando si legge qui. Il massimo storico, non il
    // valore dal vivo - stessa ragione di sopra.
    if (stage_ != CalibStage::Relax) return 0.0;
    return bestSeparation_;
}

double Calibration::progress() const noexcept {
    if (stage_ == CalibStage::Concentrate) {
        // Prima fase ora: nessun ciclo naturale da rispettare (non c'e' un
        // respiro da non interrompere a meta'), quindi la barra segue solo il
        // massimo storico dei campioni indipendenti.
        const double perCampioni = concBestEffN_ / config::kCalibTargetEffSamples;
        return clamp01(perCampioni);
    }
    if (stage_ == CalibStage::Prepare) {
        return clamp01(stageElapsed_ / config::kCalibPrepareS);
    }
    if (stage_ == CalibStage::Relax) {
        // Seconda fase ora: oltre ai campioni serve la separazione dalla
        // concentrazione appena fatta, e la barra non deve comunque sembrare
        // finita a meta' respiro - tre criteri, il piu' indietro dei tre frena.
        const double perCampioni = relaxBestEffN_ / config::kCalibTargetEffSamples;
        const double perSepar    = bestSeparation_ / config::kCalibMinSeparationT;
        const double perCiclo    = std::fmod(stageElapsed_, config::kBreathCycleS) /
                                   config::kBreathCycleS;
        return clamp01(std::min({perCampioni, perSepar, perCiclo}));
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
    if (stage_ == CalibStage::Prepare) {
        // Schermata di istruzioni, non di misura: il tempo scorre comunque,
        // buono o cattivo che sia il segnale in questo istante - non c'e'
        // niente da scartare, e restare bloccati qui perche' la fascia balla
        // sarebbe solo una punizione.
        stageElapsed_ += dt;
        if (stageElapsed_ >= config::kCalibPrepareS) {
            enterStage(CalibStage::Relax);
            return true;
        }
        return false;
    }
    if (stage_ != CalibStage::Concentrate && stage_ != CalibStage::Relax) return false;

    // Il tempo si accumula solo mentre il segnale vale: serve alla rete di
    // sicurezza, che deve misurare quanto si è provato davvero, non quanto si è
    // aspettato con la fascia storta. In Relax e' anche l'orologio del respiro
    // guidato (breathPhase()): se il segnale si interrompe l'animazione si
    // ferma con lui, invece di scorrere su una misura che nel frattempo non
    // sta contando niente.
    const double prevElapsed = stageElapsed_;
    if (usable) stageElapsed_ += dt;

    if (stage_ == CalibStage::Concentrate) {
        // Prima fase ora. concStats_ NON si azzera mai: i campioni buoni si
        // accumulano per tutta la fase, punto. Sotto kCalibMinRawForLatch
        // campioni grezzi non si aggiorna il massimo storico: con n piccolo la
        // stima di autocorrelazione e' instabile e puo' regalare un picco
        // fasullo. Nessun bordo di ciclo da rispettare qui (a differenza di
        // Relax, piu' sotto): il bersaglio che cresce non ha un respiro da non
        // interrompere a meta', quindi si chiude appena il traguardo e' pieno.
        if (concStats_.n >= config::kCalibMinRawForLatch) {
            concBestEffN_ = std::max(concBestEffN_, concStats_.effectiveN());
        }
        if (concBestEffN_ >= config::kCalibTargetEffSamples) {
            enterStage(CalibStage::Prepare);
            return true;
        }
    } else {
        // Relax, seconda fase ora: oltre ai campioni serve la separazione da
        // concStats_ (gia' completa, raccolta nella fase precedente). Massimi
        // storici, non i valori dal vivo: sono due condizioni che oscillano
        // indipendentemente, e pretendere che siano vere nello STESSO istante
        // e' un bersaglio molto piu' stretto che chiederle vere ciascuna in un
        // momento qualsiasi - osservato sul campo: effN superava il traguardo
        // piu' volte (fino a oltre il doppio) ma la calibrazione falliva
        // comunque perche' la separazione non coincideva mai nello stesso
        // tick. Un picco statistico su pochissimi campioni non conta: con n
        // piccolo la varianza si stima male e un t altissimo può uscire per
        // puro rumore, non perche' le due fasi siano davvero separate.
        if (relaxStats_.n >= config::kCalibMinRawForLatch) {
            relaxBestEffN_  = std::max(relaxBestEffN_, relaxStats_.effectiveN());
            bestSeparation_ = std::max(bestSeparation_, separationT(relaxStats_, concStats_));
        }

        // Si CHIUDE pero' solo a fine ciclo, mai a meta' respiro: altrimenti la
        // fase potrebbe interrompersi durante un'inspirazione, il che sarebbe
        // sia brutto da vedere sia in contraddizione col prompt appena dato.
        const auto cicloPrima = static_cast<int>(prevElapsed / config::kBreathCycleS);
        const auto cicloOra   = static_cast<int>(stageElapsed_ / config::kBreathCycleS);
        const bool abbastanza = relaxBestEffN_ >= config::kCalibTargetEffSamples;
        const bool separate   = bestSeparation_ >= config::kCalibMinSeparationT;
        if (cicloOra > cicloPrima && abbastanza && separate) {
            finalize();
            return true;
        }
    }

    // Rete di sicurezza: MAI un fallimento bloccante che rimanda l'utente a
    // ricominciare da zero (vedi il divieto di reset, anche nascosto, delle
    // statistiche di calibrazione: qui si applica lo stesso principio al
    // fallimento - si accetta il meglio raccolto finora invece di buttarlo
    // via). Oltre il tempo massimo per fase si procede comunque: in
    // Concentrate verso Prepare/Relax anche sotto soglia, in Relax dritti a
    // finalize(), che a sua volta sintetizza un margine minimo se la
    // modulazione misurata e' troppo debole per reggere da sola - vedi
    // finalize(). L'unico modo per finire davvero in Failed resta l'assenza
    // TOTALE di segnale utilizzabile per l'intera calibrazione (vedi
    // finalize()), che e' un problema di hardware/contatto, non di sforzo.
    if (stageElapsed_ > config::kCalibMaxPhaseS) {
        if (stage_ == CalibStage::Concentrate) {
            enterStage(CalibStage::Prepare);
        } else {
            finalize();
        }
        return true;
    }
    return false;
}

void Calibration::finalize() {
    if (!std::isfinite(peak_) || !std::isfinite(trough_)) {
        // L'UNICO fallimento rimasto: zero campioni utilizzabili in tutta la
        // calibrazione. Non e' "non hai marcato abbastanza la differenza", e'
        // "la fascia non ha mai dato un campione buono" - un problema di
        // contatto, non di sforzo, e l'unico caso in cui rimandare la persona
        // indietro e' onesto invece che punitivo.
        fail("Segnale assente durante la calibrazione.");
        return;
    }

    const double M    = (peak_ + trough_) / 2.0;
    double       span = peak_ - trough_;
    const double minSpan = config::kCalibMinSpanRel * std::max(1e-6, std::fabs(M));

    // Modulazione debole non e' piu' un fallimento: si sintetizza un margine
    // minimo intorno al neutro misurato, cosi' la legge di controllo ha
    // comunque una banda su cui lavorare invece di rimandare la persona a
    // ricominciare la calibrazione da capo. Stesso principio del divieto di
    // reset (anche nascosto) delle statistiche: si accetta il meglio raccolto
    // invece di buttarlo via. Il controllo che ne risulta sara' meno reattivo
    // (span minimo = meno margine fra gli estremi), ma FUNZIONA - ed e' quello
    // che serve per arrivare all'esperienza vera invece di restare bloccati
    // qui.
    if (span < minSpan) span = minSpan;

    absMax_   = M + span / 2.0;
    absMin_   = M - span / 2.0;
    neutralM_ = M;
    localMax_ = M;   // la banda locale parte dal neutro
    localMin_ = M;
    valid_    = true;
    stage_    = CalibStage::Done;
    doneTimer_ = 0.0;
    message_  = "Calibrazione completata";
}

void Calibration::useFallbackProfile() {
    if (stage_ != CalibStage::Failed) return;

    const double M    = config::kCalibFallbackNeutral;
    const double span = M * config::kCalibFallbackSpanFrac * 2.0;

    absMax_   = M + span / 2.0;
    absMin_   = M - span / 2.0;
    neutralM_ = M;
    localMax_ = M;
    localMin_ = M;
    valid_    = true;
    usingFallback_ = true;
    stage_    = CalibStage::Done;
    doneTimer_ = 0.0;
    message_  = "Calibrazione sostituita da una banda generica (non personale)";
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
