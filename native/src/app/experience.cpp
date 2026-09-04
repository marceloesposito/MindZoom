#include "app/experience.hpp"

#include "app/platform.hpp"
#include "ble/muse.hpp"
#include "ble/recording.hpp"
#include "ble/synthetic.hpp"
#include "config.hpp"
#include "control/adaptive_band.hpp"
#include "control/calibration.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include "util/double_buffer.hpp"
#include "util/smoothing.hpp"
#include "util/spsc_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mz::app::experience {
namespace {

using namespace mz;

// --- geometria della scheda di calibrazione (specchio del CSS della versione web) ---
//
// La zona dei cerchi non ha piu' un'altezza riservata dentro il pannello: le
// fasi attive disegnano l'elemento centrato sullo schermo intero, senza
// pannello attorno (vedi drawCalibrationCard).

constexpr render::Color kBg        {0.02f, 0.02f, 0.03f, 1.0f};
constexpr render::Color kInk       {0.92f, 0.94f, 0.98f, 1.0f};
constexpr render::Color kMuted     {0.62f, 0.66f, 0.73f, 1.0f};
constexpr render::Color kAccent    {0.24f, 0.76f, 0.95f, 1.0f};   // ciano: fase di concentrazione
constexpr render::Color kAccent2   {0.40f, 0.80f, 0.38f, 1.0f};   // verde: fase di rilassamento
constexpr render::Color kOk        {0.30f, 0.85f, 0.55f, 1.0f};
constexpr render::Color kWarn      {0.96f, 0.62f, 0.26f, 1.0f};
constexpr render::Color kBad       {0.94f, 0.36f, 0.36f, 1.0f};
constexpr render::Color kPanel     {0.075f, 0.08f, 0.115f, 0.95f};   // base dei pannelli a sfumatura
constexpr render::Color kShadow    {0.0f, 0.0f, 0.0f, 0.5f};

/**
 * Stessa tinta, luminanza leggermente piu' bassa: l'estremo scuro delle
 * sfumature diagonali. Scala uniformemente i tre canali invece di spostare la
 * tinta, cosi' il colore resta "lo stesso" percettivamente, solo piu' in ombra.
 */
constexpr render::Color darken(render::Color c, float amount) noexcept {
    const float k = 1.0f - amount;
    return {c.r * k, c.g * k, c.b * k, c.a};
}

// Tavolozza per l'effetto caustico/particellare dei cerchi animati della
// calibrazione (drawCausticSphere, usata da drawBreathingCircles e
// drawFocusTarget): verde-blu invece della sfumatura "alla Siri" di prima.
constexpr render::Color kSiriBlue {0.25f, 0.55f, 1.00f, 1.0f};
constexpr render::Color kSiriGreen{0.35f, 0.90f, 0.50f, 1.0f};

/** Pallino numerato (badge) per i due passaggi della calibrazione: cerchio pieno + cifra. */
void drawStepBadge(render::Renderer& r, render::Point c, float radius, int number, bool active,
                   bool done, render::Color tint) {
    if (done) {
        r.fillCircle(c, radius, kOk);
    } else if (active) {
        r.fillCircle(c, radius, tint);
    } else {
        r.fillCircle(c, radius, {1.0f, 1.0f, 1.0f, 0.10f});
    }
    const render::Color numColor = (active || done) ? render::Color{0.04f, 0.05f, 0.07f, 1.0f}
                                                     : kMuted;
    r.drawTextCentered(std::to_wstring(number), c, radius * 1.1f, numColor, true);
}

/** Pulsante a pillola: sfumatura diagonale sottile sullo stesso tono. */
void drawPillButton(render::Renderer& r, render::Rect box, const std::wstring& label,
                    render::Color fill) {
    const float radius = box.height() * 0.5f;
    r.fillRectShadow(box, fill, radius, {fill.r, fill.g, fill.b, 0.35f}, 18.0f, {0.0f, 6.0f});
    r.fillRectGradient(box, fill, darken(fill, 0.16f), radius);
    r.drawTextCentered(label, {(box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f},
                       box.height() * 0.4f, {0.03f, 0.04f, 0.06f, 1.0f}, true);
}

/**
 * Una notifica Bluetooth cosi' com'e' arrivata. Dimensione fissa per non
 * allocare nel callback GATT, che non deve mai bloccarsi.
 */
struct RawPacket {
    std::uint16_t len = 0;
    std::uint8_t  data[256]{};
};

struct Shared {
    util::SpscRing<ble::Sample, 16384> ring;
    util::SpscRing<RawPacket, 2048>    rawRing;
    util::DoubleBuffer<app::ControlState> state;

    control::Tunables                     tune;
    util::DoubleBuffer<control::Tunables> tunables;

    void publishTunables() { tunables.publish(tune); }

    std::atomic<int>           command{static_cast<int>(app::Command::None)};
    std::atomic<bool>          running{true};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool>          hudVisible{true};
    std::atomic<bool>          quitRequested{false};
    // Pagina d'ingresso: la prima cosa che si vede all'avvio, prima ancora
    // della calibrazione. Non e' uno stato della calibrazione - riguarda
    // l'esperienza intera - quindi vive qui e non in CalibStage, che altrimenti
    // finirebbe per descrivere cose che con la calibrazione non c'entrano.
    std::atomic<bool>          landingVisible{true};
    // Banda adattiva invece della calibrazione a due fasi (vedi StartOptions).
    // Letto dal thread DSP e dal disegno, scritto una volta sola in start().
    std::atomic<bool>          adaptiveBand{true};
    // Modale di gestione Bluetooth (tasto B): stato + riconnetti/disconnetti a
    // comando, invece del solo testo passivo del pannello diagnostico.
    std::atomic<bool>          bleModalVisible{false};
    // Overlay di debug (tasto V): grafici di campioni effettivi/autocorrelazione
    // durante la calibrazione, per capire perche' una fase si allunga senza
    // dover leggere il log da terminale.
    std::atomic<bool>          calibDebugVisible{false};

    ble::Recorder recorder;

    std::atomic<bool> replaying{false};
    std::atomic<bool> replayCancel{false};
    std::thread       replayWorker;
    std::wstring      replayName;
    std::atomic<bool> resetHistory{false};

    std::mutex                bleLogMutex;
    std::deque<std::wstring>  bleLog;

    // File di diagnostica: un log completo per sessione, non solo le ultime 4
    // righe del pannello H. Flush ad ogni riga apposta - deve sopravvivere
    // anche a un crash, non solo a una chiusura pulita, perche' e' proprio nei
    // crash che serve di piu'.
    std::mutex    debugLogMutex;
    std::ofstream debugLog;

    void openDebugLog(const std::wstring& dir) {
        if (dir.empty()) return;
        platform::ensureDirectory(dir);

        const std::time_t t = std::time(nullptr);
        const std::tm* lt = std::localtime(&t);
        wchar_t name[64]{};
        std::swprintf(name, 64, L"/mindzoom-debug-%04d%02d%02d-%02d%02d%02d.log",
                      lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                      lt->tm_hour, lt->tm_min, lt->tm_sec);
        const std::string path(std::string(dir.begin(), dir.end()) +
                               std::string(name, name + std::wcslen(name)));

        std::lock_guard<std::mutex> lock(debugLogMutex);
        debugLog.open(path, std::ios::out | std::ios::trunc);
        if (debugLog.is_open()) {
            debugLog << "=== Mind Zoom - log di diagnostica (macOS) ===\n"
                     << "build: " << __DATE__ << " " << __TIME__ << "\n"
                     << "kLocalTolerance=" << config::kLocalTolerance
                     << " kSampleRate=" << config::kSampleRate
                     << " kChannels=" << config::kChannels << "\n";
            debugLog.flush();
        }
    }

    void writeDebugLog(const std::string& line) {
        std::lock_guard<std::mutex> lock(debugLogMutex);
        if (!debugLog.is_open()) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        debugLog << "[" << ms << "] " << line << "\n";
        debugLog.flush();
    }

    void closeDebugLog(const std::string& footer) {
        std::lock_guard<std::mutex> lock(debugLogMutex);
        if (!debugLog.is_open()) return;
        debugLog << "=== " << footer << " ===\n";
        debugLog.close();
    }

    void pushBleLog(const std::string& msg) {
        writeDebugLog(msg);
        std::wstring w(msg.begin(), msg.end());
        std::lock_guard<std::mutex> lock(bleLogMutex);
        bleLog.push_back(std::move(w));
        while (bleLog.size() > 4) bleLog.pop_front();
    }

    std::vector<std::wstring> bleLogSnapshot() {
        std::lock_guard<std::mutex> lock(bleLogMutex);
        return {bleLog.begin(), bleLog.end()};
    }
};

Shared g;
ble::MuseClient g_muse;
std::thread     g_dspThread;

std::wstring fixed(double v, int decimals) {
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"%.*f", decimals, v);
    return buf;
}

/** Nome del file senza il percorso: e' quello che ha senso mostrare nel pannello. */
std::wstring fileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

/** <recordingsDir>/sessione-AAAAMMGG-HHMMSS.mzr */
std::wstring newRecordingPath(const std::wstring& recordingsDir) {
    const std::time_t t = std::time(nullptr);
    // std::localtime non e' thread-safe, ma qui si chiama una volta sola
    // all'avvio, prima che gli altri thread partano.
    const std::tm* lt = std::localtime(&t);

    wchar_t name[64]{};
    std::swprintf(name, 64, L"/sessione-%04d%02d%02d-%02d%02d%02d.mzr",
                  lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                  lt->tm_hour, lt->tm_min, lt->tm_sec);
    return recordingsDir + name;
}

// ---------------------------------------------------------------------------
// Thread 1-bis: riproduzione da file al posto della fascia
// ---------------------------------------------------------------------------

/**
 * Rigioca una registrazione nel ring, allo stesso ritmo dell'hardware.
 * Scrive esattamente dove scriverebbe il callback GATT: tutto cio' che sta a
 * valle non sa che il segnale viene da un file.
 */
void replayThread(std::vector<ble::Sample> samples) {
    if (samples.empty()) return;

    const auto start = std::chrono::steady_clock::now();
    std::size_t sent = 0;

    while (g.running.load(std::memory_order_acquire) &&
           !g.replayCancel.load(std::memory_order_acquire) &&
           sent < samples.size()) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto due = static_cast<std::size_t>(elapsed * config::kSampleRate);

        while (sent < samples.size() && sent < due) {
            if (!g.ring.push(samples[sent])) {
                g.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            ++sent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    if (sent >= samples.size()) {
        g.pushBleLog("riproduzione terminata: registrazione esaurita");
    }
}

void stopReplayInternal() {
    g.replayCancel.store(true, std::memory_order_release);
    if (g.replayWorker.joinable()) g.replayWorker.join();
    g.replayCancel.store(false, std::memory_order_release);
    g.replaying.store(false, std::memory_order_release);
}

/** Comune a riproduzione da file e a dati simulati: sostituisce la fascia col ring. */
void beginReplay(std::wstring name, std::vector<ble::Sample> samples) {
    const double seconds = static_cast<double>(samples.size()) / config::kSampleRate;

    stopReplayInternal();
    g_muse.stop();
    g.ring.clear();

    g.replayName = std::move(name);
    g.replaying.store(true, std::memory_order_release);
    g.resetHistory.store(true, std::memory_order_release);
    g.command.store(static_cast<int>(app::Command::RestartSession), std::memory_order_release);

    g.pushBleLog("riproduzione: " + std::to_string(static_cast<int>(seconds)) + "s");

    g.replayWorker = std::thread(replayThread, std::move(samples));
}

/** @return messaggio d'errore, vuoto se e' andata. */
std::string startReplayInternal(const std::wstring& path) {
    auto loaded = ble::loadRecording(path);
    if (!loaded.ok) return loaded.error;
    beginReplay(fileNameOf(path), std::move(loaded.samples));
    return {};
}

/**
 * Tasto D: dati sintetici plausibili al posto della fascia, generati sul
 * momento (ble/synthetic.hpp) - la stessa via che alimenta --riproduci, senza
 * dover prima passare da mz_gen_test_recording e un file su disco. Pensata per
 * la schermata di calibrazione quando non c'e' segnale: vedi handleKey.
 */
void startSyntheticInternal() {
    constexpr double kSyntheticSeconds = 600.0;   // 10 minuti: non si esaurisce a meta' prova
    beginReplay(L"dati simulati", ble::synthetic::generate(kSyntheticSeconds));
}

/**
 * Tasto C nella modale Bluetooth: chiude un'eventuale riproduzione/dati
 * sintetici e forza un nuovo tentativo di connessione alla fascia reale.
 */
void reconnectInternal() {
    stopReplayInternal();
    g_muse.stop();
    g_muse.start();
    g.resetHistory.store(true, std::memory_order_release);
    g.command.store(static_cast<int>(app::Command::RestartSession), std::memory_order_release);
    g.pushBleLog("riconnessione richiesta dall'utente");
}

/**
 * Tasto X nella modale Bluetooth: disconnessione voluta, niente riaggancio
 * automatico finche' non si chiede di riconnettersi (vedi MuseClient::stop).
 */
void disconnectInternal() {
    g_muse.stop();
    g.pushBleLog("disconnessione richiesta dall'utente");
}

// ---------------------------------------------------------------------------
// Thread 2: DSP
// ---------------------------------------------------------------------------

void dspThread() {
    dsp::SlidingStft       stft;
    dsp::Gating            gating;
    control::IndexSmoother smoother;
    control::Calibration   calib;
    control::AdaptiveBand  adaptive;

    // Con la banda adattiva non c'e' onboarding da attraversare: l'esperienza
    // comincia subito, e il controllo resta fermo da solo finche' la banda non
    // e' pronta (velocity() torna 0 finche' non le si e' adottata una banda).
    const bool adattiva = g.adaptiveBand.load(std::memory_order_relaxed);

    // Con la banda adattiva l'esperienza NON passa mai da Onboarding: e' la
    // fase da cui dipende il disegno della scheda di calibrazione. Va usata
    // ovunque si riparta da capo (avvio, RestartSession, ricalibrazione),
    // altrimenti un solo punto dimenticato rimette in scena una calibrazione
    // che in questa modalita' non esiste - preso proprio cosi': --riproduci
    // emette RestartSession e faceva ricomparire la scheda.
    const auto faseIniziale =
        adattiva ? control::Phase::Interactive : control::Phase::Onboarding;

    auto phase = faseIniziale;
    double phaseElapsed = 0.0;
    double doneHold     = 0.0;
    double velocityRaw  = 0.0;
    int    gatedWindows = 0;
    bool   contactOk    = false;
    bool   signalPlausible = false;
    double flushTimer      = 0.0;
    double calibDiagTimer  = 0.0;   // diagnostica temporanea, vedi sotto

    util::Ema velocitySmooth;
    // Smoothing elastico: solo sul ramo che guida lo zoom, vedi kElasticTauS.
    util::TwoPoleEma elastic;

    auto lastTick    = std::chrono::steady_clock::now();
    auto lastFrameAt = lastTick;

    app::ControlState st;
    st.phase = static_cast<int>(phase);

    constexpr double dt = config::kControlDt;

    while (g.running.load(std::memory_order_acquire)) {
        const auto cmd = static_cast<app::Command>(
            g.command.exchange(static_cast<int>(app::Command::None), std::memory_order_acq_rel));
        if (cmd == app::Command::StartCalibration || cmd == app::Command::RetryCalibration) {
            calib.start();
            smoother.reset();
            elastic.reset();
            velocityRaw = 0.0;
            velocitySmooth.reset();
            // Anche la fase torna indietro: il tasto K arriva da un'esperienza
            // gia' in corso (Interactive), dove senza questo la scheda di
            // calibrazione non verrebbe mai mostrata. Dall'introduzione, unico
            // altro punto che emette StartCalibration, la fase e' gia' questa
            // e la riga non cambia nulla.
            phase        = faseIniziale;
            phaseElapsed = 0.0;
            doneHold     = 0.0;
            // In modalita' adattiva "ricalibrare" vuol dire dimenticare la
            // banda e rifare il riscaldamento: non c'e' nessuna calibrazione
            // da rifare, ma la richiesta ha comunque senso (un'altra persona
            // che prova dopo, la fascia sistemata meglio).
            if (adattiva) adaptive.reset();
        } else if (cmd == app::Command::UseFallbackProfile) {
            calib.useFallbackProfile();
        } else if (cmd == app::Command::RestartSession) {
            calib = control::Calibration{};
            stft  = dsp::SlidingStft{};
            gating.reset();
            smoother.reset();
            elastic.reset();
            velocityRaw = 0.0;
            velocitySmooth.reset();
            phase        = faseIniziale;
            phaseElapsed = 0.0;
            doneHold     = 0.0;
            gatedWindows = 0;
            contactOk    = false;
            // Cambia la sorgente del segnale: la banda vecchia non vale piu'.
            if (adattiva) adaptive.reset();
        }

        const control::Tunables tune = g.tunables.read();

        RawPacket raw;
        while (g.rawRing.pop(raw)) {
            g.recorder.writeRaw(raw.data, raw.len);
        }

        ble::Sample s;
        bool consumed = false;

        while (g.ring.pop(s)) {
            consumed = true;
            g.recorder.write(s);

            if (!stft.pushSample(s.uv, s.adc)) continue;

            ++st.frames;
            lastFrameAt = std::chrono::steady_clock::now();

            const auto quality = gating.assess(stft);
            dsp::Bands bands;
            const auto index = dsp::popeIndex(stft, &bands);
            if (index) st.rawIndex = *index;

            contactOk = quality.contactOk;

            if (!quality.contactOk) {
                velocityRaw = 0.0;
                ++gatedWindows;
            } else if (quality.artifact) {
                ++gatedWindows;
                if (gatedWindows > config::kArtifactHoldMaxS * config::kControlHz) {
                    velocityRaw *= 0.85;
                }
            } else if (index) {
                gatedWindows = 0;
                const double c = smoother.push(*index, dt);
                st.smoothedIndex = c;
                // Stadio in piu', solo per chi guida lo zoom: la banda
                // adattiva e la calibrazione misurano `c` vero, altrimenti la
                // stima degli estremi si restringerebbe insieme allo smoothing
                // invece di restare fedele al segnale.
                const double cZoom = elastic.push(c, dt, tune.elasticTauS);

                if (adattiva) {
                    // La banda insegue il segnale: si alimenta con gli stessi
                    // campioni su cui poi si guida, e la si riadotta a ogni
                    // giro (adoptBand conserva l'isteresi, vedi li').
                    adaptive.push(c);
                    if (adaptive.ready()) {
                        calib.adoptBand(adaptive.lo(), adaptive.hi(), adaptive.mid());
                    }
                    velocityRaw = calib.velocity(cZoom, dt, tune);
                } else if (phase == control::Phase::Onboarding) {
                    calib.sample(c, quality.contactOk &&
                                    quality.fault == dsp::SignalFault::None);
                } else {
                    velocityRaw = calib.velocity(cZoom, dt, tune);
                }
            }

            st.theta = bands.theta; st.alpha = bands.alpha; st.beta = bands.beta;
            st.maxAbsRaw = quality.maxAbsRaw;
            st.contactOk = quality.contactOk;
            st.artifact  = quality.artifact;
            st.signalFault  = static_cast<int>(quality.fault);
            st.autocorr1     = quality.autocorr1;
            st.railFraction  = quality.railFraction;
            st.spreadCounts  = quality.spreadCounts;
            st.mainsFraction = quality.mainsFraction;
            signalPlausible = (quality.fault == dsp::SignalFault::None);
        }

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;

        const bool signalFresh =
            st.frames > 0 && std::chrono::duration<double>(now - lastFrameAt).count() < 1.0;

        if (adattiva) {
            // Nessuna fase da attraversare: si resta in Interactive dal primo
            // istante. Il tempo qui non serve a niente, ma la diagnostica sul
            // riscaldamento si', e va scritta con lo stesso ritmo del resto.
            calibDiagTimer += elapsed;
            if (calibDiagTimer >= 2.0) {
                calibDiagTimer = 0.0;
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    "adattiva: pronta=%d riscaldamento=%.0f%% campioni=%zu "
                    "banda=[%.3f .. %.3f] M=%.3f signalFresh=%d contactOk=%d plausible=%d",
                    adaptive.ready() ? 1 : 0, 100.0 * adaptive.warmupProgress(), adaptive.size(),
                    adaptive.lo(), adaptive.hi(), adaptive.mid(),
                    signalFresh, contactOk, signalPlausible);
                g.pushBleLog(buf);
            }
        } else if (phase == control::Phase::Onboarding) {
            const auto stagePrima = calib.stage();
            calib.tick(elapsed, signalFresh && contactOk && signalPlausible);

            // Diagnostica temporanea: un utente ha segnalato che in Relax i
            // campioni non sembrano accumularsi ("softbloccato"). Un log per
            // cambio di fase e uno ogni ~2s bastano a vedere se e' davvero
            // fermo o solo lento, senza dover leggere lo schermo.
            calibDiagTimer += elapsed;
            if (calib.stage() != stagePrima || calibDiagTimer >= 2.0) {
                calibDiagTimer = 0.0;
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                    "calib: stage=%s effN=%.2f/%d raw=%d elapsed=%.1fs signalFresh=%d "
                    "contactOk=%d plausible=%d artifact=%d gated=%d autocorr1=%.3f",
                    control::toString(calib.stage()), calib.effectiveSamples(),
                    static_cast<int>(config::kCalibTargetEffSamples), calib.rawRelaxSamples(),
                    calib.stageElapsed(), signalFresh, contactOk, signalPlausible,
                    st.artifact ? 1 : 0, gatedWindows, st.autocorr1);
                g.pushBleLog(buf);
            }

            if (calib.stage() == control::CalibStage::Done) {
                doneHold += elapsed;
                if (doneHold >= config::kCalibDoneHoldS) {
                    phase = config::kEnableHookPhase ? control::Phase::Hook
                                                     : control::Phase::Interactive;
                    phaseElapsed = 0.0;
                }
            }
        } else {
            phaseElapsed += elapsed;
            if (phase == control::Phase::Hook && phaseElapsed >= config::kPhaseHookS) {
                phase = control::Phase::Handover;
                phaseElapsed = 0.0;
            } else if (phase == control::Phase::Handover &&
                       phaseElapsed >= config::kPhaseHandoverS) {
                phase = control::Phase::Interactive;
                phaseElapsed = 0.0;
            }
        }

        const bool replaying = g.replaying.load(std::memory_order_acquire);
        const auto silent = g_muse.millisSinceLastPacket();
        const bool stalled = !replaying && g_muse.streaming() && silent > 0 &&
                             silent > static_cast<std::int64_t>(config::kEegWatchdogS * 1000);
        if (stalled) {
            velocityRaw = 0.0;
            g_muse.resumeStreaming();
        }

        double velocity = 0.0;
        if (signalFresh) {
            velocity = velocitySmooth.push(velocityRaw, elapsed, tune.velTauS);
        } else {
            velocityRaw = 0.0;
            velocity = velocitySmooth.decay(elapsed, config::kVelStaleTauS);
        }

        st.phase        = static_cast<int>(phase);
        st.phaseElapsed = phaseElapsed;
        st.calibStage   = static_cast<int>(calib.stage());
        st.calibDisplayTarget = calib.displayTarget();
        st.calibShowTarget    = calib.showTarget();
        st.calibBreathPhase   = calib.breathPhase();
        st.calibProgress      = calib.progress();
        st.calibEffN          = calib.effectiveSamples();
        st.calibSeparation    = calib.separation();
        st.calibValid = calib.valid();
        st.calibUsingFallback = calib.usingFallback();
        st.adaptiveActive = adattiva;
        st.adaptiveReady  = adaptive.ready();
        st.adaptiveWarmup = adaptive.warmupProgress();
        st.absMin   = calib.absMin();
        st.absMax   = calib.absMax();
        st.neutral  = calib.neutral();
        st.localMin = calib.localMin();
        st.localMax = calib.localMax();
        st.velocity    = velocity;
        st.velocityRaw = velocityRaw;
        st.gate        = calib.gate();
        st.magnitude   = calib.magnitude();
        st.signalFresh = signalFresh;

        if (calib.stage() == control::CalibStage::Failed) {
            st.failReason = static_cast<int>(
                calib.absMax() == calib.absMin() ? app::FailReason::NoSignal
                                                 : app::FailReason::WeakModulation);
        } else if (st.frames > 0 && !signalPlausible) {
            st.failReason = static_cast<int>(app::FailReason::ImplausibleSignal);
        } else {
            st.failReason = static_cast<int>(app::FailReason::None);
        }

        st.stalled        = stalled;
        st.bleState       = static_cast<int>(g_muse.state());
        st.packetLen      = g_muse.lastPacketLen();
        st.packetSamples  = g_muse.lastPacketSamples();
        st.rawPackets     = g_muse.rawPackets();
        st.validPackets   = g_muse.validPackets();

        flushTimer += elapsed;
        if (flushTimer >= 2.0) {
            flushTimer = 0.0;
            g.recorder.flush();
        }

        st.droppedSamples = g.dropped.load(std::memory_order_relaxed);
        st.replaying       = replaying;
        st.recording       = g.recorder.hasData();
        st.recordedSamples = g.recorder.count();
        g.state.publish(st);

        if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---------------------------------------------------------------------------
// Disegno
// ---------------------------------------------------------------------------

/** Riga di stato in fondo alla scheda: la stessa striscia serve a tutte le fasi. */
void drawStatusHint(render::Renderer& r, render::Rect box, const std::wstring& text,
                    render::Color color) {
    r.drawTextBody(text, box, 14.0f, color, render::TextAlign::Center);
}

/** Cosa dire e quanto contare alla rovescia, per il lato corrente del box breathing. */
struct BreathPrompt {
    const wchar_t* label;
    int            countdown;   // 4..1: quanto manca a fine lato
};

BreathPrompt breathPromptAt(double phaseS) {
    const int    seg  = static_cast<int>(phaseS / config::kBreathBoxS) % 4;
    const double frac = std::fmod(phaseS, config::kBreathBoxS) / config::kBreathBoxS;
    const int    left = std::clamp(4 - static_cast<int>(frac * 4.0), 1, 4);
    switch (seg) {
        case 0:  return {L"Inspira",   left};
        case 1:  return {L"Trattieni", left};
        case 2:  return {L"Espira",    left};
        default: return {L"Trattieni", left};
    }
}

/**
 * Raggio del cerchio del respiro: sinusoidale (raised-cosine, un mezzo coseno
 * da 0 a 1) durante inspirazione/espirazione, fermo ai due "trattieni" - un
 * box breathing vero ha soste, un'onda continua no. E' il compromesso fra le
 * due richieste.
 */
float breathRadius(double phaseS, float rMin, float rMax) {
    const int    seg   = static_cast<int>(phaseS / config::kBreathBoxS) % 4;
    const double frac  = std::fmod(phaseS, config::kBreathBoxS) / config::kBreathBoxS;
    const double ease  = 0.5 - 0.5 * std::cos(M_PI * frac);
    switch (seg) {
        case 0:  return static_cast<float>(rMin + (rMax - rMin) * ease);   // inspira
        case 1:  return rMax;                                              // trattieni pieno
        case 2:  return static_cast<float>(rMax - (rMax - rMin) * ease);   // espira
        default: return rMin;                                              // trattieni vuoto
    }
}

/**
 * Rampa verde -> blu a piu' fermate, per i puntini dello sciame.
 *
 * Una lerp diretta fra i due estremi (com'era prima) attraversa i toni
 * intermedi in linea retta nello spazio RGB: quel cammino passa vicino al
 * grigio-verdastro e i toni di mezzo escono tutti simili fra loro, cosi' a
 * occhio lo sciame sembra bicolore invece che sfumato. Le fermate qui sotto
 * piegano il cammino verso smeraldo/turchese/ciano - la stessa distanza
 * percettiva, ma coperta da tinte distinguibili.
 */
inline render::Color swarmTint(float u) noexcept {
    static constexpr render::Color kStops[] = {
        kSiriGreen,                    // verde, estremo della rampa
        {0.20f, 0.88f, 0.62f, 1.0f},   // smeraldo
        {0.15f, 0.82f, 0.78f, 1.0f},   // turchese
        {0.18f, 0.72f, 0.92f, 1.0f},   // ciano
        kSiriBlue,                     // blu, estremo opposto
    };
    constexpr int kCount = static_cast<int>(sizeof(kStops) / sizeof(kStops[0]));

    const float x   = std::clamp(u, 0.0f, 1.0f) * (kCount - 1);
    const int   i   = std::min(static_cast<int>(x), kCount - 2);
    const float f   = x - static_cast<float>(i);
    const auto& a   = kStops[i];
    const auto& b   = kStops[i + 1];
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1.0f};
}

/**
 * PRNG deterministico locale allo sciame: serve un seme stabile per ogni
 * particella (stesso indice = stesso seme), non vera casualita'.
 */
inline float causticHash(std::uint32_t n) noexcept {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= (n >> 4);
    n *= 0x27d4eb2du;
    n ^= (n >> 15);
    return static_cast<float>(n & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

/**
 * Sciame di puntini con dinamica boids (separazione/allineamento/coesione) piu'
 * un campo ondoso di disturbo, contenuto in un disco unitario - al posto della
 * sfumatura piena "alla Siri" usata prima qui, e dei filamenti a spirale
 * (parametrici, quindi prevedibili: la stessa forma ogni volta) del tentativo
 * precedente. Stato PERSISTENTE fra un frame e l'altro, non una funzione pura
 * di t: e' quello che rende il moto vivo invece che una traiettoria che si
 * ripete identica. Le frequenze del campo ondoso sono incommensurabili fra
 * loro e diverse per particella, cosi' anche a regime il moto non si
 * stabilizza mai in un ciclo osservabile.
 *
 * Coordinate memorizzate in unita' di raggio (-1..1 circa), cosi' la stessa
 * istanza serve sia al respiro (raggio che cambia) sia al bersaglio (raggio
 * che cresce): e' il centro e il raggio passati a draw() che cambiano, non lo
 * sciame sotto.
 */
class CausticSwarm {
public:
    /**
     * Quante particelle usa questo sciame e quanto grandi sono i puntini.
     *
     * Serve perche' i tre sciami non fanno lo stesso mestiere: quelli di
     * respiro e bersaglio riempiono un disco di poco piu' di duecento pixel,
     * quello della pagina d'ingresso deve coprire lo SCHERMO INTERO e con una
     * grana molto piu' fine - decine di migliaia di puntini invece di qualche
     * centinaio. Da qui in giu' quasi tutte le distanze sono espresse in
     * MULTIPLI DELLA SPAZIATURA MEDIA fra particelle, non in valori assoluti:
     * e' l'unico modo perche' le stesse regole diano lo stesso aspetto a
     * densita' che differiscono di due ordini di grandezza.
     */
    void configure(int n, float dotScale) {
        n         = std::max(1, n);
        dotScale_ = dotScale;
        if (n != n_) { n_ = n; seeded_ = false; }
    }

    /**
     * @param t orologio monotono (mai la fase del respiro, che si ripete ogni
     *        ciclo: usarla qui renderebbe il moto prevedibile un ciclo dopo
     *        l'altro, esattamente cio' che non si vuole).
     * @param fill [0,1]: quante particelle sono visibili, non la loro
     *        dimensione - e' cosi' che il bersaglio "si riempie" restando
     *        fatto di puntini, mai di un disco pieno.
     * @param seedBase distingue sciami diversi (respiro vs bersaglio) che
     *        userebbero altrimenti lo stesso pattern di semi.
     * @param gather [0,1]: 0 = sparso su tutto il riquadro, 1 = raccolto nella
     *        sfera. Non e' un'interpolazione fra due disegni: e' il CONTENIMENTO
     *        che cambia forma, e le particelle ci arrivano dentro con la loro
     *        fisica - separazione, allineamento, coesione, campo ondoso. Il
     *        passaggio si vede come uno stormo che si stringe, non come una
     *        dissolvenza.
     * @param boxHalfW,boxHalfH semiampiezze del riquadro da coprire quando
     *        gather=0, in unita' di `radius`. A gather=1 non contano.
     *
     * La tinta non e' un parametro: viene tutta da swarmTint, la stessa rampa
     * verde-blu per entrambi gli sciami.
     */
    void draw(render::Renderer& r, render::Point c, float radius, double t, float fill,
             std::uint32_t seedBase, float gather = 1.0f,
             float boxHalfW = 1.0f, float boxHalfH = 1.0f) {
        if (radius < 2.0f || fill <= 0.0f) return;
        const float bx = std::max(1.0f, boxHalfW);
        const float by = std::max(1.0f, boxHalfH);
        const float g  = std::clamp(gather, 0.0f, 1.0f);
        ensureSeeded(seedBase, bx, by);

        const auto tf = static_cast<float>(t);
        const float dt = lastT_.has_value() ? std::clamp(tf - *lastT_, 0.0f, 0.1f) : 0.0f;
        lastT_ = tf;
        step(dt, tf, seedBase, bx, by, g);

        const int visible = std::clamp(
            static_cast<int>(std::ceil(n_ * std::clamp(fill, 0.0f, 1.0f))), 0, n_);

        // I puntini si accumulano in secchielli di tinta e opacita' e si
        // disegnano un secchiello alla volta. Con decine di migliaia di
        // elementi il costo non e' riempirli - sono grandi un pixel - ma
        // impostare un colore e avviare un tracciato per ognuno: cosi' quel
        // costo si paga una volta per secchiello. Vedi Renderer::fillRects.
        for (auto& b : buckets_) b.clear();
        buckets_.resize(kTintSteps * kAlphaSteps);

        for (int i = 0; i < visible; ++i) {
            const auto  si = static_cast<std::size_t>(i);
            const float fade = fadeOf(life_[si]);
            if (fade <= 0.01f) continue;

            const std::uint32_t h = hashSeed(seedBase, i);

            // Tinta: posizione FISSA per particella lungo la rampa verde-blu,
            // piu' una deriva lenta e di ampiezza diversa per ciascuna. Senza
            // la deriva l'insieme e' un mosaico immobile di colori; con una
            // deriva comune respirerebbe tutto all'unisono, che si legge come
            // un lampeggio.
            const float tintSeed = causticHash(h + 29u);
            const float drift    = 0.10f + 0.16f * causticHash(h + 77u);
            const float mixv     = tintSeed +
                                  drift * std::sin(tf * 0.23f + tintSeed * kTau * 3.0f);

            // Profondita': una "distanza" fissa per particella governa INSIEME
            // opacita' e dimensione, invece di due valori scorrelati. E' il
            // modo in cui l'occhio legge la terza dimensione in una nuvola -
            // cio' che e' lontano e' insieme piu' piccolo e piu' sbiadito.
            const float depth = std::pow(causticHash(h + 8u), 1.6f);

            // Ombreggiatura sferica: la nuvola raccolta e' confinata nel
            // cerchio unitario, e z = sqrt(1 - r^2) e' la quota che quel punto
            // avrebbe sulla sfera di cui il cerchio e' la silhouette. E' quello
            // che trasforma un disco di puntini in una sfera, senza nessuna
            // geometria 3D vera dietro.
            //
            // Da sparsa pero' non c'e' nessuna sfera da ombreggiare: lo stesso
            // calcolo spegnerebbe tutto cio' che sta lontano dal centro, cioe'
            // i bordi dello schermo. L'ombreggiatura entra quindi insieme alla
            // raccolta, e da sparsa resta la sola profondita'.
            const float rr2  = px_[si] * px_[si] + py_[si] * py_[si];
            const float lim  = limitAt(px_[si], py_[si], bx, by, g);
            const float unit = std::min(1.0f, rr2 / std::max(lim * lim, 1e-6f));
            const float z    = std::sqrt(1.0f - unit);
            const float sh   = depth * ((0.30f + 0.70f * z) * g + (1.0f - g));

            // Soglia sul raggio: sotto il mezzo punto un quadratino
            // antialiasato si spalma su un pixel solo e sbiadisce fino a
            // sparire. Rimpicciolire oltre questo limite non rende i puntini
            // piu' fini, li cancella - misurato a schermo.
            const float dotR  = std::max(kDotFloor, (0.35f + 0.95f * sh) * dotScale_) * fade;
            const float alpha = (0.12f + 0.78f * sh) * fade;
            if (alpha <= 0.01f) continue;

            const int ti = std::clamp(static_cast<int>(mixv * kTintSteps), 0, kTintSteps - 1);
            const int ai = std::clamp(static_cast<int>(alpha * kAlphaSteps), 0, kAlphaSteps - 1);

            const float x = c.x + px_[si] * radius;
            const float y = c.y + py_[si] * radius;
            buckets_[static_cast<std::size_t>(ti * kAlphaSteps + ai)].push_back(
                render::rect(x - dotR, y - dotR, x + dotR, y + dotR));
        }

        for (int ti = 0; ti < kTintSteps; ++ti) {
            const render::Color base = swarmTint((ti + 0.5f) / kTintSteps);
            for (int ai = 0; ai < kAlphaSteps; ++ai) {
                const auto& b = buckets_[static_cast<std::size_t>(ti * kAlphaSteps + ai)];
                if (b.empty()) continue;
                const float a = (ai + 0.5f) / kAlphaSteps;
                r.fillRects(b.data(), static_cast<int>(b.size()), {base.r, base.g, base.b, a});
            }
        }
    }

private:
    static constexpr float kTau = 6.28318530718f;
    static constexpr float kPi  = 3.14159265359f;
    // Secchielli di colore per il disegno raggruppato. Abbastanza da non far
    // vedere gradini sulla rampa di tinta, pochi da restare qualche centinaio
    // di chiamate per fotogramma.
    static constexpr int kTintSteps  = 16;
    static constexpr int kAlphaSteps = 12;
    static constexpr int kMaxDim     = 256;   // tetto alle celle per lato

    /** Seme di una particella, che cambia a ogni sua rinascita. */
    std::uint32_t hashSeed(std::uint32_t seedBase, int i) const {
        return seedBase + static_cast<std::uint32_t>(i) * 2654435761u +
               gen_[static_cast<std::size_t>(i)] * 7919u;
    }

    /**
     * Inviluppo di comparsa e scomparsa lungo la vita della particella.
     *
     * Serve a far respirare il campo: senza, i puntini sono sempre gli stessi
     * per sempre e l'insieme, per quanto si muova, resta lo stesso insieme. Con
     * la rinascita invece qualcuno svanisce e qualcun altro compare altrove, e
     * la nuvola si rinnova di continuo. E' anche cio' che tiene la copertura
     * uniforme: i boids, lasciati a se', si raggruppano e lasciano vuoti.
     */
    static float fadeOf(float life) {
        const float in  = std::clamp(life / 0.10f, 0.0f, 1.0f);
        const float out = std::clamp((1.0f - life) / 0.15f, 0.0f, 1.0f);
        const float e   = std::min(in, out);
        return e * e * (3.0f - 2.0f * e);   // smoothstep
    }

    /**
     * Limite radiale del contenimento nella direzione di (x,y).
     *
     * A gather=1 vale sempre 1: il cerchio unitario di sempre. A gather=0 e' la
     * distanza dal centro al bordo del riquadro in quella direzione, cioe' lo
     * schermo intero. In mezzo la forma passa dal rettangolo al cerchio con
     * continuita': e' questa forma che si stringe a trascinarsi dietro le
     * particelle.
     */
    static float limitAt(float x, float y, float bx, float by, float g) {
        // Distanza dal centro al bordo del rettangolo lungo la direzione di
        // (x,y). Il fattore r c'e' perche' (x,y) e' una POSIZIONE, non un
        // versore: bx/|x| e' il fattore di scala che porta il punto sul bordo,
        // e va moltiplicato per la distanza del punto per diventare una
        // distanza. Senza, il contenimento non era il rettangolo dello schermo
        // ma una figura che si stringeva man mano che ci si allontanava dal
        // centro - il campo restava un ovale con i margini neri attorno.
        const float r = std::sqrt(x * x + y * y);
        if (r <= 1e-6f) {   // al centro non c'e' direzione
            const float d = std::max(bx, by);
            return d + (1.0f - d) * g;
        }
        const float ax = std::fabs(x), ay = std::fabs(y);
        const float tx = ax > 1e-6f ? bx * r / ax : 1e30f;
        const float ty = ay > 1e-6f ? by * r / ay : 1e30f;
        const float dbox = std::min(tx, ty);
        return dbox + (1.0f - dbox) * g;
    }

    float rnd() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return static_cast<float>((rng_ >> 8) & 0xFFFFFFu) / 16777216.0f;
    }

    /** Posizione nuova, sparsa nel contenimento attuale. */
    void placeAt(std::size_t si, float bx, float by, float g) {
        const float x = (rnd() * 2.0f - 1.0f) * bx;
        const float y = (rnd() * 2.0f - 1.0f) * by;
        const float lim = limitAt(x, y, bx, by, g);
        const float rr  = std::sqrt(x * x + y * y);
        if (rr > lim && rr > 1e-6f) {
            const float k = lim / rr;
            px_[si] = x * k; py_[si] = y * k;
        } else {
            px_[si] = x; py_[si] = y;
        }
        const float va = rnd() * kTau;
        vx_[si] = std::cos(va) * 0.10f;
        vy_[si] = std::sin(va) * 0.10f;
    }

    void ensureSeeded(std::uint32_t seedBase, float bx, float by) {
        if (seeded_) return;
        const auto n = static_cast<std::size_t>(n_);
        px_.assign(n, 0.0f); py_.assign(n, 0.0f);
        vx_.assign(n, 0.0f); vy_.assign(n, 0.0f);
        life_.assign(n, 0.0f); span_.assign(n, 1.0f); gen_.assign(n, 0u);
        sax_.assign(n, 0.0f); say_.assign(n, 0.0f);
        rng_ = seedBase * 2246822519u + 1u;

        const bool spread = (bx > 1.001f || by > 1.001f);
        for (int i = 0; i < n_; ++i) {
            const auto  si = static_cast<std::size_t>(i);
            const auto  ui = static_cast<std::uint32_t>(i);
            if (spread) {
                // Sparse su tutto il riquadro fin dal primo fotogramma: la
                // pagina d'ingresso deve aprirsi gia' piena, non riempirsi
                // sotto gli occhi di chi guarda.
                px_[si] = (causticHash(seedBase + ui * 97u + 1u)  * 2.0f - 1.0f) * bx;
                py_[si] = (causticHash(seedBase + ui * 131u + 7u) * 2.0f - 1.0f) * by;
            } else {
                const float a  = causticHash(seedBase + ui * 97u + 1u) * kTau;
                const float rr = 0.20f + 0.70f * causticHash(seedBase + ui * 131u + 7u);
                px_[si] = std::cos(a) * rr;
                py_[si] = std::sin(a) * rr;
            }
            const float va = causticHash(seedBase + ui * 211u + 3u) * kTau;
            vx_[si] = std::cos(va) * 0.10f;
            vy_[si] = std::sin(va) * 0.10f;
            // Vite gia' sfasate all'avvio, o morirebbero tutte insieme e il
            // campo lampeggerebbe invece di rinnovarsi con continuita'.
            life_[si] = causticHash(seedBase + ui * 307u + 11u);
            span_[si] = kLifeMin + (kLifeMax - kLifeMin) * causticHash(seedBase + ui * 401u + 19u);
        }
        seeded_ = true;
    }

    /**
     * Griglia uniforme ricostruita a ogni passo, con ordinamento per conteggio.
     *
     * Senza, il passo dei boids e' O(n^2): con le poche centinaia di particelle
     * di prima passava, con le decine di migliaia che servono a coprire lo
     * schermo sarebbero centinaia di milioni di coppie per fotogramma. Con la
     * griglia ogni particella guarda solo le 9 celle attorno a se', e il costo
     * torna proporzionale al numero di particelle.
     */
    void buildGrid(float bx, float by, float g, float sense) {
        // La griglia copre il contenimento ATTUALE, non il riquadro: da
        // raccolte le particelle stanno in un cerchio unitario, e tenere celle
        // grandi quanto lo schermo le ammasserebbe tutte in poche caselle,
        // annullando il vantaggio della griglia.
        const float hx = (bx + (1.0f - bx) * g) + 0.5f;
        const float hy = (by + (1.0f - by) * g) + 0.5f;
        const float spanX = 2.0f * hx, spanY = 2.0f * hy;
        minX_ = -hx; minY_ = -hy;
        gw_ = std::clamp(static_cast<int>(std::ceil(spanX / sense)), 1, kMaxDim);
        gh_ = std::clamp(static_cast<int>(std::ceil(spanY / sense)), 1, kMaxDim);
        sx_ = static_cast<float>(gw_) / spanX;
        sy_ = static_cast<float>(gh_) / spanY;

        const int cells = gw_ * gh_;
        cellStart_.assign(static_cast<std::size_t>(cells) + 1, 0);
        cellOf_.resize(static_cast<std::size_t>(n_));
        order_.resize(static_cast<std::size_t>(n_));

        for (int i = 0; i < n_; ++i) {
            const auto si = static_cast<std::size_t>(i);
            const int gx = std::clamp(static_cast<int>((px_[si] - minX_) * sx_), 0, gw_ - 1);
            const int gy = std::clamp(static_cast<int>((py_[si] - minY_) * sy_), 0, gh_ - 1);
            const int ci = gy * gw_ + gx;
            cellOf_[si] = ci;
            ++cellStart_[static_cast<std::size_t>(ci) + 1];
        }
        for (int ci = 0; ci < cells; ++ci) {
            cellStart_[static_cast<std::size_t>(ci) + 1] +=
                cellStart_[static_cast<std::size_t>(ci)];
        }
        cursor_.assign(cellStart_.begin(), cellStart_.end() - 1);
        for (int i = 0; i < n_; ++i) {
            const auto si = static_cast<std::size_t>(i);
            order_[static_cast<std::size_t>(cursor_[static_cast<std::size_t>(cellOf_[si])]++)] = i;
        }
    }

    /** Un passo di simulazione: regole boids leggere + onde + contenimento. */
    void step(float dt, float t, std::uint32_t seedBase, float bx, float by, float g) {
        if (dt <= 0.0f) return;

        // Tutte le distanze delle regole si misurano in spaziature medie. La
        // spaziatura la detta l'area del contenimento attuale: raccogliendosi
        // l'area crolla da tutto lo schermo al cerchio unitario, e con essa
        // devono rimpicciolirsi spazio vitale e raggio di percezione - altrimenti
        // da raccolte le particelle si vedrebbero tutte fra loro e si
        // respingerebbero a vicenda fino a far esplodere la sfera.
        const float areaBox = 4.0f * bx * by;
        const float area    = areaBox + (kPi - areaBox) * g;
        const float spacing = std::sqrt(std::max(area, 1e-4f) / static_cast<float>(n_));
        const float sense   = 2.0f * spacing;
        const float sense2  = sense * sense;

        buildGrid(bx, by, g, sense);

        // La ricerca dei vicini e' di gran lunga la parte cara del passo, ed e'
        // anche quella che cambia piu' lentamente: a campo lento le forze
        // sociali di un fotogramma e del successivo sono quasi identiche. Se ne
        // aggiorna quindi meta' per volta - le pari in un fotogramma, le dispari
        // in quello dopo - e nel frattempo ciascuna riusa l'ultima calcolata.
        // Ogni particella resta aggiornata ogni due fotogrammi, cioe' ogni 33 ms
        // a 60 Hz: sotto la soglia di quello che si vede, e vale il doppio delle
        // particelle a parita' di costo. L'integrazione e il campo ondoso, che
        // sono cio' che si vede muoversi, restano a ogni fotogramma.
        ++parity_;
        const std::uint32_t turno = parity_ & 1u;

        for (int i = 0; i < n_; ++i) {
            const auto si = static_cast<std::size_t>(i);

            // --- ciclo di vita ---
            life_[si] += dt / span_[si];
            if (life_[si] >= 1.0f) {
                life_[si] = 0.0f;
                span_[si] = kLifeMin + (kLifeMax - kLifeMin) * rnd();
                ++gen_[si];                 // tinta e profondita' nuove
                placeAt(si, bx, by, g);
                continue;                   // rinata: niente forze in questo passo
            }

            const std::uint32_t h = hashSeed(seedBase, i);

            // Ogni particella ha il PROPRIO spazio vitale e la propria forza
            // di separazione: con un solo raggio uguale per tutte lo sciame si
            // dispone a grana uniforme, che e' esattamente cio' che non si
            // vuole. Cosi' invece alcune tengono gli altri a distanza molto
            // piu' del necessario e altre quasi per niente, e la nuvola si
            // organizza da sola in vuoti e addensamenti - la trama "a
            // caustica" che si cerca, che nasce dalla disomogeneita'.
            const float roomSeed  = causticHash(h + 13u);
            const float personalR = (0.66f + 2.60f * roomSeed * roomSeed) * spacing;
            const float sepGain   = 1.1f + 2.2f * roomSeed;

            const bool aggiorna = ((static_cast<std::uint32_t>(i) & 1u) == turno);
            if (aggiorna) {

            // Separazione/allineamento/coesione contro i vicini entro il raggio
            // di percezione, cercati nelle 9 celle attorno invece che fra tutte.
            float sepX = 0.0f, sepY = 0.0f, aliX = 0.0f, aliY = 0.0f, cohX = 0.0f, cohY = 0.0f;
            int   neighbors = 0;
            const int gx = std::clamp(static_cast<int>((px_[si] - minX_) * sx_), 0, gw_ - 1);
            const int gy = std::clamp(static_cast<int>((py_[si] - minY_) * sy_), 0, gh_ - 1);
            for (int oy = -1; oy <= 1; ++oy) {
                const int cy2 = gy + oy;
                if (cy2 < 0 || cy2 >= gh_) continue;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int cx2 = gx + ox;
                    if (cx2 < 0 || cx2 >= gw_) continue;
                    const int ci   = cy2 * gw_ + cx2;
                    const int from = cellStart_[static_cast<std::size_t>(ci)];
                    const int to   = cellStart_[static_cast<std::size_t>(ci) + 1];
                    for (int k = from; k < to; ++k) {
                        const int j = order_[static_cast<std::size_t>(k)];
                        if (i == j) continue;
                        const auto  sj = static_cast<std::size_t>(j);
                        const float dx = px_[si] - px_[sj], dy = py_[si] - py_[sj];
                        const float d2 = dx * dx + dy * dy;
                        if (d2 < sense2 && d2 > 1e-12f) {
                            const float d   = std::sqrt(d2);
                            const float inv = 1.0f / d;
                            if (d < personalR) {
                                const float push =
                                    std::min(6.0f, (personalR - d) / std::max(d, 0.01f * spacing));
                                sepX += dx * inv * push; sepY += dy * inv * push;
                            }
                            aliX += vx_[sj];  aliY += vy_[sj];
                            cohX += px_[sj];  cohY += py_[sj];
                            ++neighbors;
                        }
                    }
                }
            }
            float sx = sepX * sepGain * spacing * kSepScale;
            float sy = sepY * sepGain * spacing * kSepScale;
            if (neighbors > 0) {
                const float inv = 1.0f / static_cast<float>(neighbors);
                // L'allineamento fa le correnti, la coesione tiene insieme il
                // gruppo. Con la coesione troppo bassa lo sciame si sfilaccia e
                // il moto sembra agitato invece che coeso. La coesione pero'
                // cresce con la raccolta: da sparse tirerebbe le particelle in
                // grumi, lasciando vuoto lo schermo che devono coprire.
                sx += (aliX * inv - vx_[si]) * 0.30f;
                sy += (aliY * inv - vy_[si]) * 0.30f;
                const float coes = 0.16f * (0.25f + 0.75f * g);
                sx += (cohX * inv - px_[si]) * coes;
                sy += (cohY * inv - py_[si]) * coes;
            }
                sax_[si] = sx;
                say_[si] = sy;
            }

            float ax = sax_[si];
            float ay = say_[si];

            // Campo ondoso a due scale: una lunga che trascina in correnti
            // ampie, una corta che le increspa. Frequenze incommensurabili fra
            // loro e fase diversa per particella, quindi il moto non si ripete
            // mai in modo osservabile - a differenza di un'unica frequenza
            // comune, che si leggerebbe come un battito.
            // Il campo si calma man mano che la sfera si forma. Sparso deve
            // essere vivo - e' quello che si e' scelto guardandolo - ma la
            // stessa agitazione dentro il cerchio unitario increspa la
            // silhouette e la sfera non si chiude mai davvero: raccolta, la
            // forma conta piu' del movimento.
            const float vivacita = kDynamism * (1.0f - 0.60f * g);
            const float seed = causticHash(h + 5u);
            ax += (std::sin(py_[si] * 3.1f + t * 0.28f + seed * kTau) * 0.45f +
                   std::sin(py_[si] * 9.7f - t * 0.95f + seed * kTau * 2.1f) * 0.12f) * vivacita;
            ay += (std::cos(px_[si] * 2.7f + t * 0.24f + seed * kTau * 1.3f) * 0.45f +
                   std::cos(px_[si] * 8.3f - t * 0.85f + seed * kTau * 0.7f) * 0.12f) * vivacita;

            const float rr  = std::sqrt(px_[si] * px_[si] + py_[si] * py_[si]);
            const float lim = limitAt(px_[si], py_[si], bx, by, g);

            // Richiamo verso la sfera finale: agisce SOLO su cio' che sta gia'
            // fuori dal cerchio unitario, con forza proporzionale a quanto la
            // raccolta e' avanzata. E' la mano che stringe lo stormo. A raccolta
            // completa non c'e' piu' niente fuori, quindi si annulla da sola e
            // la sfera resta identica a com'era prima che tutto questo esistesse.
            if (g > 0.0f && rr > 1.0f) {
                const float inv  = 1.0f / std::max(rr, 1e-6f);
                const float pull = g * g * std::min(3.0f, rr - 1.0f) * 2.2f;
                ax -= px_[si] * inv * pull;
                ay -= py_[si] * inv * pull;
            }

            // Contenimento morbido vicino al bordo (il muro netto arriva dopo).
            // La soglia si sposta verso il bordo man mano che la sfera si forma:
            // ferma a 0.92 le particelle si accampavano su quell'anello e la
            // sfera veniva fuori cava, un guscio invece di un corpo. Spinta
            // quasi contro il muro, la separazione ha spazio per distribuirle in
            // tutto il volume e il centro si riempie.
            const float soglia = 0.92f + 0.07f * g;
            if (rr > soglia * lim) { ax -= px_[si] * 2.6f; ay -= py_[si] * 2.6f; }
            // Spinta verso fuori vicino al centro: evita il collasso al centro
            // della SFERA. Da sparse non deve esistere, o scaverebbe un buco in
            // mezzo allo schermo proprio dove il campo deve essere pieno.
            if (g > 0.0f && rr < 0.05f) { ax += px_[si] * 0.8f * g; ay += py_[si] * 0.8f * g; }

            // Smorzamento: e' anche il freno che decide la velocita' di regime.
            // 0.88 tiene un moto lento e continuo, che e' quello che serve a una
            // schermata su cui si deve stare calmi.
            vx_[si] = (vx_[si] + ax * dt) * 0.88f;
            vy_[si] = (vy_[si] + ay * dt) * 0.88f;
            px_[si] += vx_[si] * dt;
            py_[si] += vy_[si] * dt;

            // Muro netto sul contenimento: nessuna particella lo attraversa
            // mai, cosi' la nuvola raccolta ha una silhouette circolare pulita e
            // si legge come una SFERA invece che come una macchia sfrangiata. La
            // componente radiale di velocita' viene annullata (non riflessa):
            // chi arriva al bordo ci scivola sopra in tangenziale, come su una
            // superficie, invece di rimbalzare verso il centro.
            const float r2   = std::sqrt(px_[si] * px_[si] + py_[si] * py_[si]);
            const float lim2 = limitAt(px_[si], py_[si], bx, by, g);
            if (r2 > lim2 && r2 > 1e-6f) {
                const float inv = 1.0f / r2;
                const float nx = px_[si] * inv, ny = py_[si] * inv;
                px_[si] = nx * lim2;
                py_[si] = ny * lim2;
                const float radial = vx_[si] * nx + vy_[si] * ny;
                if (radial > 0.0f) { vx_[si] -= radial * nx; vy_[si] -= radial * ny; }
            }
        }
    }

    // Durata di vita di una particella, in secondi. Abbastanza lunga da non
    // vedere un formicolio, abbastanza corta da rinnovare il campo di continuo.
    static constexpr float kLifeMin = 6.0f;
    static constexpr float kLifeMax = 18.0f;
    // La separazione era tarata sulle distanze assolute di prima; ora che si
    // misura in spaziature va riportata alla stessa scala di forza.
    static constexpr float kSepScale = 11.0f;
    // Quanto e' vivo il moto. E' il campo ondoso a dare il dinamismo, e la
    // velocita' di regime gli e' proporzionale a smorzamento fissato: questo
    // fattore moltiplica quindi, in pratica, la velocita' delle particelle.
    static constexpr float kDynamism = 1.6f;
    // Raggio minimo, in punti. Con l'antialiasing spento (vedi
    // Renderer::fillRects) un rettangolo piu' stretto di un pixel del
    // dispositivo puo' arrotondarsi a niente e il puntino sparisce a
    // intermittenza: questo e' il limite sotto cui non si scende.
    static constexpr float kDotFloor = 0.40f;

    std::vector<float>         px_, py_, vx_, vy_, life_, span_;
    // Forze "sociali" (separazione/allineamento/coesione) dell'ultimo calcolo:
    // vedi step() per il perche' non si ricalcolino a ogni fotogramma.
    std::vector<float>         sax_, say_;
    std::uint32_t              parity_ = 0;
    std::vector<std::uint32_t> gen_;
    int                        n_        = 380;
    float                      dotScale_ = 1.0f;
    bool                       seeded_   = false;
    std::optional<float>       lastT_;
    std::uint32_t              rng_ = 1u;

    // --- griglia spaziale, vedi buildGrid ---
    std::vector<int> cellOf_, cellStart_, order_, cursor_;
    float            minX_ = 0.0f, minY_ = 0.0f, sx_ = 1.0f, sy_ = 1.0f;
    int              gw_ = 1, gh_ = 1;

    // --- disegno raggruppato ---
    std::vector<std::vector<render::Rect>> buckets_;
};

// Pagina d'ingresso: molte piu' particelle degli altri due sciami, e piu'
// piccole. Deve coprire tutto lo schermo, e su quella superficie il conteggio
// del disco di calibrazione si leggerebbe come una spruzzata rada. I valori
// sono stati scelti guardando la schermata, non a intuito.
constexpr int   kLandingDots     = 15000;
constexpr float kLandingDotScale = 0.48f;

CausticSwarm gBreathSwarm;
CausticSwarm gFocusSwarm;
CausticSwarm gLandingSwarm;

/**
 * Cerchi che respirano: lo sciame si espande e si contrae col respiro dentro
 * un disco di fondo FISSO, la scritta del lato e il conto alla rovescia stanno
 * al centro e sopra, in posizione FISSA - dentro un cerchio che si restringe
 * fino a 44px il testo non ci starebbe leggibile.
 *
 * Il fondo e' l'opposto esatto del bersaglio della concentrazione: li' un
 * anello vuoto che lo sciame deve raggiungere, qui un disco pieno dentro cui
 * lo sciame respira. Le due fasi si distinguono cosi' a colpo d'occhio, prima
 * ancora di leggere il titolo. Il raggio e' quello MASSIMO del respiro, non
 * quello corrente: un fondo che pulsa insieme ai puntini toglierebbe il
 * riferimento fermo rispetto a cui si vede che stanno respirando.
 */
void drawBreathingCircles(render::Renderer& r, render::Point c, double phaseS, double animT) {
    constexpr float kRMin = 44.0f;
    constexpr float kRMax = 92.0f;
    const float radius = breathRadius(phaseS, kRMin, kRMax);

    constexpr render::Color kBreathBackdrop{1.0f, 1.0f, 1.0f, 0.08f};
    r.fillCircle(c, kRMax, kBreathBackdrop);
    gBreathSwarm.draw(r, c, radius, animT, 1.0f, 0xB2EA7Fu);

    const auto prompt = breathPromptAt(phaseS);
    r.drawTextCentered(std::to_wstring(prompt.countdown), c, 28.0f, kInk, true);
    r.drawTextCentered(prompt.label, {c.x, c.y - kRMax - 44.0f}, 20.0f, kAccent2, true);
}

/**
 * Bersaglio della concentrazione: un anello esterno fisso e uno sciame interno
 * che cresce con `focusFrac` [0,1]. L'obiettivo e' che lo sciame arrivi a
 * toccare l'anello, al posto del vecchio quadratino su un binario.
 *
 * Lo sciame resta visibile fino in fondo, anche a bersaglio raggiunto: e' la
 * sua estensione a dire quanto si e' vicini, e sostituirlo con un anello
 * piatto proprio sul finale (come faceva prima) toglieva l'unica cosa che si
 * stava guardando. Solo l'assenza di contatto lo spegne, perche' li' non c'e'
 * davvero niente "in ascolto" da mostrare.
 */
void drawFocusTarget(render::Renderer& r, render::Point c, float outerR, float focusFrac,
                     bool contactOk, double animT) {
    const float frac   = std::clamp(focusFrac, 0.0f, 1.0f);
    const float innerR = std::max(10.0f, outerR * frac);

    // Il bersaglio e' solo un contorno: bianco al 30%, molto spesso, senza
    // riempimento. Il disco verde pieno che c'era prima faceva da fondo
    // colorato allo sciame e ne spegneva le tinte - e non serviva a niente,
    // visto che la meta' e' il BORDO, non l'area. Spesso e trasparente invece
    // che sottile e opaco: a parita' di presenza visiva non compete con i
    // puntini, che sono l'elemento da guardare.
    //
    // Vira al verde quando lo sciame arriva a riempirlo: e' il segnale di
    // "ci sei", dato dal bersaglio stesso invece che da un elemento in piu'.
    // La transizione parte dal 75% e non da un valore secco, cosi' si vede
    // ARRIVARE - un cambio istantaneo sull'ultimo pixel non si nota, e questa
    // e' l'unica conferma che la persona riceve mentre sta facendo lo sforzo.
    constexpr render::Color kTargetRing{1.0f, 1.0f, 1.0f, 0.15f};
    constexpr float kRingThickness = 44.0f;
    const auto near = static_cast<float>(util::smoothstep01((frac - 0.75f) / 0.25f));
    const render::Color ring{kTargetRing.r + (kOk.r - kTargetRing.r) * near,
                             kTargetRing.g + (kOk.g - kTargetRing.g) * near,
                             kTargetRing.b + (kOk.b - kTargetRing.b) * near,
                             kTargetRing.a + (0.85f - kTargetRing.a) * near};

    // drawArc CENTRA la fascia sul raggio dato: passando outerR, una fascia
    // spessa 44px si estenderebbe da outerR-22 a outerR+22, e lo sciame a
    // pieno riempimento finirebbe a meta' dentro la fascia invece di
    // toccarla. Spostando il raggio di meta' spessore il bordo INTERNO
    // coincide con outerR: il momento in cui i puntini arrivano al bersaglio
    // e' allora un contatto netto, che e' tutto cio' che la fase deve
    // comunicare.
    r.drawArc(c, outerR + kRingThickness * 0.5f, -90.0f, 270.0f, kRingThickness, ring);

    if (!contactOk) {
        r.drawArc(c, innerR, -90.0f, 270.0f, 3.0f, kMuted);
    } else {
        gFocusSwarm.draw(r, c, innerR, animT, std::max(0.15f, frac), 0x4D17C3u);
    }
}

/**
 * Pagina d'ingresso: il titolo dell'esperienza e un tasto per cominciare.
 *
 * Non spiega la calibrazione - quella ha gia' la sua schermata subito dopo -
 * e non elenca istruzioni: dice cos'e' Mind Zoom in una riga e lascia
 * decidere quando partire. Serve soprattutto a dare un momento di attesa
 * prima che l'esperienza cominci: senza, il programma si apriva gia' dentro
 * la calibrazione, con la fascia magari ancora in mano.
 */
void drawLandingPage(render::Renderer& r, const app::ControlState& st, double animT) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;
    const float cy  = win.height * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), kBg);

    // Lo stesso sciame delle fasi di calibrazione, ma qui non sta dietro il
    // titolo: riempie lo SCHERMO. E' la pagina d'ingresso a raccontare cosa
    // succede quando la fascia comincia a leggere - i pallini sparsi ovunque
    // sono il segnale che ancora non c'e', la sfera al centro e' il segnale
    // trovato. Il passaggio fra i due stati non e' un taglio: lo fa la fisica
    // dello sciame, vedi CausticSwarm::draw.
    const render::Point swarmC{cx, cy + 40.0f};
    const float         swarmR = 210.0f;
    gLandingSwarm.configure(kLandingDots, kLandingDotScale);

    // Semiampiezze del riquadro da coprire, in unita' di swarmR, misurate dal
    // centro dello sciame al bordo piu' lontano. Il margine del 6% manda i
    // pallini appena oltre il bordo, cosi' il campo non ha una cornice vuota.
    const float boxW = std::max(cx, win.width - cx) * 1.06f / swarmR;
    const float boxH = std::max(swarmC.y, win.height - swarmC.y) * 1.06f / swarmR;

    // Non basta che la fascia sia collegata: una fascia accesa sul tavolo e'
    // collegata e non dice niente. Serve che stiano arrivando dati recenti e
    // che superino il vaglio di plausibilita' fisica - le stesse due condizioni
    // che il pannello diagnostico riassume in "Segnale: OK".
    const bool segnale = st.signalFresh && st.signalFault == 0 &&
                        (st.bleState == 3 || st.replaying);

    static float                 gather = 0.0f;
    static std::optional<double> lastLandingT;
    const float dt = lastLandingT
                        ? static_cast<float>(std::clamp(animT - *lastLandingT, 0.0, 0.1))
                        : 0.0f;
    lastLandingT = animT;
    // Costante di tempo asimmetrica: si raduna con calma, perche' e' il momento
    // da guardare; si disperde piu' in fretta se il segnale cade, perche' li'
    // conta capire subito che qualcosa non va.
    const float bersaglio = segnale ? 1.0f : 0.0f;
    const float tau       = segnale ? 2.6f : 1.2f;
    gather += (bersaglio - gather) * std::clamp(dt / tau, 0.0f, 1.0f);

    gLandingSwarm.draw(r, swarmC, swarmR, animT, 1.0f, 0x1A9F3Du, gather, boxW, boxH);

    // Il riquadro deve stare COMODO attorno al corpo del testo: se l'altezza
    // non basta per l'interlinea, la riga non viene disegnata affatto invece
    // di sbordare - a 72pt servono circa 86px, e un riquadro da 82 faceva
    // sparire il titolo senza dire niente.
    r.drawText(L"Mind Zoom", render::rect(cx - 460.0f, cy - 280.0f, cx + 460.0f, cy - 160.0f),
              72.0f, kInk, render::TextAlign::Center, true);
    // Le ultime due righe spiegano il meccanismo (cosa succede concentrandosi
    // o rilassandosi) e danno un compito concreto ("un dettaglio della foto"),
    // non solo l'esistenza di un legame fra attenzione e zoom: e' la lacuna
    // segnalata dai test con altre persone, che non sapevano su cosa
    // concentrarsi ne' come.
    r.drawTextBody(L"Un viaggio dentro una fotografia al microscopio,\n"
                   L"guidato dalla tua attenzione.\n"
                   L"Concentrati su un dettaglio della foto e lo zoom cresce;\n"
                   L"lascia andare lo sguardo, rilassati, e torna indietro.",
                  render::rect(cx - 420.0f, cy - 148.0f, cx + 420.0f, cy + 20.0f), 19.0f, kMuted);

    const render::Rect cta = render::rect(cx - 170.0f, cy + 240.0f, cx + 170.0f, cy + 300.0f);
    drawPillButton(r, cta, L"Premi INVIO per iniziare", kAccent);

    // Stato della fascia: qui e' un'informazione utile prima di cominciare, non
    // un dettaglio diagnostico - se non e' collegata conviene saperlo adesso,
    // non a calibrazione avviata.
    const wchar_t* stato =
        st.replaying                      ? L"riproduzione da file in corso"
        : st.bleState == 3                ? L"fascia collegata"
        : st.bleState == 0                ? L"fascia non collegata - B per il pannello Bluetooth"
                                          : L"ricerca della fascia in corso...";
    const render::Color colore = st.replaying ? kAccent
                               : st.bleState == 3 ? kOk
                               : st.bleState == 0 ? kBad : kWarn;
    drawStatusHint(r, render::rect(cx - 400.0f, cy + 318.0f, cx + 400.0f, cy + 346.0f), stato,
                  colore);
}

/**
 * Riscaldamento della banda adattiva: quello che si vede al posto della
 * calibrazione.
 *
 * NON e' una calibrazione mascherata: non si chiede niente alla persona, non
 * si puo' fallire e non c'e' un traguardo da raggiungere. E' solo il tempo che
 * serve perche' i percentili dell'indice significhino qualcosa - e infatti il
 * testo dice di guardare la foto, non di fare un esercizio.
 *
 * La foto sta gia' dietro (in modalita' adattiva l'esperienza e' partita dal
 * primo istante): qui sopra ci va solo una velatura e una riga, cosi' il
 * passaggio a "adesso comandi tu" non e' un cambio di schermata ma lo
 * svanire di un velo.
 */
void drawWarmupOverlay(render::Renderer& r, const app::ControlState& st) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;
    const float cy  = win.height * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), {0.02f, 0.02f, 0.03f, 0.82f});

    r.drawText(L"Un momento", render::rect(cx - 400.0f, cy - 130.0f, cx + 400.0f, cy - 50.0f),
              46.0f, kInk, render::TextAlign::Center, true);
    // Restava un promemoria passivo ("non devi fare niente: guarda la
    // fotografia") senza dire cosa sarebbe successo dopo: chi arrivava qui
    // senza aver letto la landing page si ritrovava a comandare lo zoom senza
    // preavviso. Resta un'attesa passiva - nessun esercizio, vedi il commento
    // sopra la funzione - ma ora anticipa il meccanismo che sta per diventare
    // attivo.
    r.drawTextBody(L"Sto imparando com'e' fatto il tuo segnale: non c'e' nulla da fare,\n"
                   L"guarda pure la fotografia. Quando la barra si riempie, concentrarti\n"
                   L"la ingrandira' e rilassarti la fara' tornare indietro.",
                  render::rect(cx - 420.0f, cy - 34.0f, cx + 420.0f, cy + 56.0f), 18.0f, kMuted);

    const float barW = 320.0f, barH = 6.0f, barY = cy + 92.0f;
    r.fillRect(render::rect(cx - barW / 2, barY, cx + barW / 2, barY + barH),
              {1, 1, 1, 0.10f}, barH * 0.5f);
    const auto avanz = static_cast<float>(std::clamp(st.adaptiveWarmup, 0.0, 1.0));
    if (avanz > 0.0f) {
        r.fillRect(render::rect(cx - barW / 2, barY, cx - barW / 2 + barW * avanz, barY + barH),
                  kAccent2, barH * 0.5f);
    }

    // Se il segnale non arriva la barra non avanza, e va detto: altrimenti
    // sembra che il programma sia bloccato.
    if (!st.signalFresh) {
        drawStatusHint(r, render::rect(cx - 400.0f, barY + 26.0f, cx + 400.0f, barY + 54.0f),
                      L"In attesa del segnale dalla fascia.", kWarn);
    } else if (st.signalFault != 0 || !st.contactOk) {
        drawStatusHint(r, render::rect(cx - 400.0f, barY + 26.0f, cx + 400.0f, barY + 54.0f),
                      L"Il segnale non e' utilizzabile: sistema la fascia.", kBad);
    }
}

void drawCalibrationCard(render::Renderer& r, const app::ControlState& st, double focusFrac,
                         double animT) {
    const auto win = r.size();
    const float cx = win.width * 0.5f;

    // Schermo intero e opaco: niente foto dietro a fare concorrenza
    // all'attenzione durante il respiro guidato o la concentrazione.
    r.fillRect(render::rect(0, 0, win.width, win.height), kBg);

    const auto stage = static_cast<control::CalibStage>(st.calibStage);
    const bool inRelax       = (stage == control::CalibStage::Relax);
    const bool inPrepare     = (stage == control::CalibStage::Prepare);
    const bool inConcentrate = (stage == control::CalibStage::Concentrate);
    // Concentrazione e' la prima fase ora: passata quando si e' in Prepare o
    // in Relax.
    const bool pastFirst = inPrepare || inRelax;

    // --- fasi ATTIVE: solo l'elemento centrale ---
    //
    // Mentre si sta facendo l'esercizio non c'e' niente da leggere: le
    // istruzioni sono gia' state date nella schermata che precede la fase
    // (Intro per la concentrazione, Preparati per il rilassamento), e
    // lasciarle a schermo insieme a pannello, breadcrumb e contatori mette in
    // concorrenza il testo con l'unica cosa da guardare. Peggio: i contatori
    // trasformano un esercizio in una barra di caricamento da fissare, che e'
    // il modo piu' sicuro per non concentrarsi ne' rilassarsi.
    //
    // Restano solo il conto alla rovescia e la parola del respiro dentro
    // l'elemento del rilassamento: non sono didascalia, sono il metronomo
    // dell'esercizio - senza, non si sa quando inspirare.
    if (inConcentrate || inRelax) {
        const render::Point vizC{cx, win.height * 0.5f};
        if (inConcentrate) {
            drawFocusTarget(r, vizC, 92.0f, static_cast<float>(focusFrac), st.contactOk, animT);
        } else {
            drawBreathingCircles(r, vizC, st.calibBreathPhase, animT);
        }

        // Un'etichetta e una riga sola, non le istruzioni per intero: senza
        // niente attorno l'elemento non dice cosa farne, ma un paragrafo
        // rimetterebbe in concorrenza il testo con l'unica cosa da guardare.
        // Il titolo sta sopra, il compito sotto, entrambi lontani
        // dall'elemento - si leggono una volta e poi si dimenticano.
        const render::Color stageColor = inConcentrate ? kAccent : kAccent2;
        r.drawText(inConcentrate ? L"Concentrazione" : L"Relax",
                  render::rect(cx - 320.0f, win.height * 0.5f - 232.0f,
                              cx + 320.0f, win.height * 0.5f - 190.0f),
                  30.0f, stageColor, render::TextAlign::Center, true);
        r.drawTextBody(inConcentrate
                           ? L"Fai crescere la sfera fino al bordo: conta all'indietro\n"
                             L"da 300, di 7 in 7."
                           : L"Respira insieme alla sfera. Non c'e' piu' niente da fare.",
                      render::rect(cx - 340.0f, win.height * 0.5f + 178.0f,
                                  cx + 340.0f, win.height * 0.5f + 238.0f),
                      16.0f, kMuted);
        // Unica eccezione: se il segnale non e' utilizzabile la persona deve
        // saperlo, altrimenti resta li' a sforzarsi mentre nulla viene
        // contato. Sta in basso, lontano dall'elemento.
        const render::Rect avviso =
            render::rect(cx - 320.0f, win.height - 78.0f, cx + 320.0f, win.height - 46.0f);
        if (!st.signalFresh) {
            drawStatusHint(r, avviso, L"In attesa del segnale dalla fascia: il conteggio non avanza.",
                          kWarn);
        } else if (st.signalFault != 0) {
            drawStatusHint(r, avviso, L"Il segnale non e' utilizzabile: questi momenti non contano.",
                          kBad);
        } else if (!st.contactOk) {
            drawStatusHint(r, avviso, L"Contatto assente: sistema la fascia.", kWarn);
        }
        return;
    }

    const float panelW = 580.0f;
    const float panelH = 600.0f;
    const render::Rect panel = render::rect(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    r.fillRectShadow(panel, kPanel, 24.0f, kShadow, 46.0f, {0.0f, 20.0f});
    r.fillRectGradient(panel, kPanel, darken(kPanel, 0.10f), 24.0f);
    r.drawRectOutline(panel, {1, 1, 1, 0.09f}, 1.0f, 24.0f);

    const float top = panel.top + 40.0f;

    // Indicatore di passaggio: due pallini numerati, condiviso da tutte le fasi
    // tranne l'esito finale, dove non c'e' piu' un "prossimo passo" da indicare.
    // 1 = Concentrazione (ciano, bersaglio che cresce), 2 = Relax (viola,
    // respiro guidato) - nell'ordine in cui si incontrano davvero adesso.
    if (stage != control::CalibStage::Done && stage != control::CalibStage::Failed) {
        const float by = top;
        const float gap = 64.0f;
        const render::Point b1{cx - gap / 2, by};
        const render::Point b2{cx + gap / 2, by};
        r.drawLine({b1.x + 16.0f, b1.y}, {b2.x - 16.0f, b2.y}, {1, 1, 1, 0.14f}, 2.0f);
        drawStepBadge(r, b1, 16.0f, 1, stage == control::CalibStage::Intro || inConcentrate,
                     pastFirst, kAccent);
        drawStepBadge(r, b2, 16.0f, 2, pastFirst, false, kAccent2);
    }

    if (stage == control::CalibStage::Intro) {
        r.drawText(L"Calibrazione", render::rect(panel.left, top + 42, panel.right, top + 92),
                  34.0f, kInk, render::TextAlign::Center, true);
        r.drawTextBody(L"Due passaggi brevi, la durata si adatta al tuo segnale.",
                      render::rect(panel.left + 40, top + 100, panel.right - 40, top + 128), 16.0f,
                      kMuted);

        const float rowY = top + 170.0f;
        const render::Point p1{cx - 150.0f, rowY};
        const render::Point p2{cx + 150.0f, rowY};
        drawStepBadge(r, p1, 26.0f, 1, true, false, kAccent);
        drawStepBadge(r, p2, 26.0f, 2, true, false, kAccent2);
        r.drawTextBody(L"Concentrazione\nfai crescere il cerchio",
                      render::rect(p1.x - 110.0f, p1.y + 38.0f, p1.x + 110.0f, p1.y + 90.0f), 15.0f,
                      kInk, render::TextAlign::Center);
        r.drawTextBody(L"Relax\nrespira col cerchio",
                      render::rect(p2.x - 110.0f, p2.y + 38.0f, p2.x + 110.0f, p2.y + 90.0f), 15.0f,
                      kInk, render::TextAlign::Center);
        r.drawLine({p1.x + 34.0f, p1.y}, {p2.x - 34.0f, p2.y}, {1, 1, 1, 0.14f}, 2.0f);

        r.drawTextBody(L"Finisce da sola quando ha raccolto abbastanza misure:\n"
                       L"se il segnale peggiora si ferma, quei momenti non contano.",
                      render::rect(panel.left + 48, rowY + 110.0f, panel.right - 48, rowY + 160.0f),
                      14.5f, kMuted);

        const render::Rect cta =
            render::rect(cx - 140.0f, panel.bottom - 118.0f, cx + 140.0f, panel.bottom - 62.0f);
        drawPillButton(r, cta, L"Premi INVIO per iniziare", kAccent);

        drawStatusHint(r, render::rect(panel.left, panel.bottom - 46, panel.right, panel.bottom - 22),
                      st.replaying ? L"riproduzione da file in corso"
                                  : L"la fascia deve essere collegata",
                      kMuted);
        if (!st.signalFresh && !st.replaying) {
            r.drawTextBody(L"Nessun dato dalla fascia: puoi iniziare lo stesso, il conteggio\n"
                           L"partira' quando arriva il segnale.",
                          render::rect(panel.left + 32, rowY + 160.0f, panel.right - 32,
                                      rowY + 205.0f),
                          13.5f, kWarn);
            r.drawTextBody(L"Q per uscire   ·   D per procedere con dati simulati",
                      render::rect(panel.left + 32, rowY + 208.0f, panel.right - 32, rowY + 232.0f),
                      13.5f, kMuted, render::TextAlign::Center);
        }
        return;
    }

    if (stage == control::CalibStage::Prepare) {
        // Stessa forma dell'introduzione - titolo, passaggio numerato,
        // istruzione - cosi' la seconda fase si presenta come si e'
        // presentata la prima, invece che con una riga di testo buttata li'.
        // Al posto del pulsante c'e' una barra che si riempie: dice che si
        // andra' avanti da soli, e quanto manca, senza chiedere niente.
        r.drawText(L"Secondo passaggio",
                  render::rect(panel.left, top + 42, panel.right, top + 92),
                  32.0f, kInk, render::TextAlign::Center, true);
        r.drawTextBody(L"Hai finito con la concentrazione. Ora si misura l'opposto.",
                      render::rect(panel.left + 40, top + 100, panel.right - 40, top + 128), 16.0f,
                      kMuted);

        const float rowY = top + 190.0f;
        drawStepBadge(r, {cx, rowY}, 34.0f, 2, true, false, kAccent2);
        r.drawText(L"Relax",
                  render::rect(panel.left, rowY + 54.0f, panel.right, rowY + 96.0f), 26.0f,
                  kAccent2, render::TextAlign::Center, true);
        r.drawTextBody(L"Segui il cerchio che respira: inspira, trattieni, espira,\n"
                       L"trattieni - quattro tempi da 4 secondi ciascuno.\n"
                       L"Lascia andare lo sforzo di prima, non c'e' piu' niente da fare.",
                      render::rect(panel.left + 40, rowY + 104.0f, panel.right - 40, rowY + 180.0f),
                      15.5f, kMuted);

        // Barra di avanzamento: e' l'unica cosa che sostituisce il pulsante.
        const float barW = 280.0f, barH = 6.0f;
        const float barY = panel.bottom - 108.0f;
        r.fillRect(render::rect(cx - barW / 2, barY, cx + barW / 2, barY + barH),
                  {1, 1, 1, 0.10f}, barH * 0.5f);
        const auto avanz = static_cast<float>(std::clamp(st.calibProgress, 0.0, 1.0));
        if (avanz > 0.0f) {
            r.fillRect(render::rect(cx - barW / 2, barY, cx - barW / 2 + barW * avanz, barY + barH),
                      kAccent2, barH * 0.5f);
        }
        drawStatusHint(r, render::rect(panel.left, barY + 22.0f, panel.right, barY + 46.0f),
                      L"si parte da solo, non devi premere niente", kMuted);
        return;
    }

    if (stage == control::CalibStage::Done) {
        const render::Point ic{cx, win.height * 0.5f - 96.0f};
        const float pulse = 3.0f + 2.0f * static_cast<float>(std::sin(animT * 2.0));
        r.fillCircle(ic, 42.0f + pulse, {kOk.r, kOk.g, kOk.b, 0.12f});
        r.fillCircle(ic, 34.0f, kOk);
        r.drawLine({ic.x - 15.0f, ic.y + 1.0f}, {ic.x - 4.0f, ic.y + 13.0f},
                   {0.03f, 0.05f, 0.04f, 1.0f}, 4.0f);
        r.drawLine({ic.x - 4.0f, ic.y + 13.0f}, {ic.x + 17.0f, ic.y - 12.0f},
                   {0.03f, 0.05f, 0.04f, 1.0f}, 4.0f);

        r.drawText(st.calibUsingFallback ? L"Pronto (banda generica)" : L"Calibrazione completata",
                  render::rect(panel.left, win.height * 0.5f - 30, panel.right,
                              win.height * 0.5f + 12),
                  28.0f, kInk, render::TextAlign::Center, true);
        r.drawTextBody(st.calibUsingFallback
                           ? L"Concentrandoti aumenti lo zoom, rilassandoti torni indietro -\n"
                             L"ma la banda non e' la tua: e' un valore generico, cosi' puoi\n"
                             L"comunque provare l'esperienza."
                           : L"Concentrandoti aumenti lo zoom, rilassandoti torni indietro.\n"
                             L"Buona esplorazione.",
                      render::rect(panel.left + 48, win.height * 0.5f + 26, panel.right - 48,
                                  win.height * 0.5f + 100),
                      16.0f, kMuted);
        return;
    }

    if (stage == control::CalibStage::Failed) {
        const auto reason = static_cast<app::FailReason>(st.failReason);
        const wchar_t* detail =
            reason == app::FailReason::NoSignal
                ? L"Segnale assente durante la calibrazione.\n"
                  L"Controlla che la fascia sia ben posizionata."
            : reason == app::FailReason::ImplausibleSignal
                ? L"Quello che arriva dalla fascia non e' un segnale cerebrale.\n"
                  L"Spegni e riaccendi la fascia, poi riprova.\n"
                  L"Se continua, e' un problema del programma, non tuo."
                : L"Modulazione troppo debole: marca di piu' la differenza\n"
                  L"fra concentrazione e rilassamento.";

        const render::Point ic{cx, win.height * 0.5f - 130.0f};
        r.fillCircle(ic, 34.0f, {kBad.r, kBad.g, kBad.b, 0.16f});
        r.drawLine({ic.x - 12.0f, ic.y - 12.0f}, {ic.x + 12.0f, ic.y + 12.0f}, kBad, 4.0f);
        r.drawLine({ic.x + 12.0f, ic.y - 12.0f}, {ic.x - 12.0f, ic.y + 12.0f}, kBad, 4.0f);

        r.drawText(L"Calibrazione non riuscita",
                   render::rect(panel.left, win.height * 0.5f - 76, panel.right,
                               win.height * 0.5f - 32),
                   26.0f, kInk, render::TextAlign::Center, true);
        r.drawTextBody(detail,
                      render::rect(panel.left + 48, win.height * 0.5f - 12, panel.right - 48,
                                  win.height * 0.5f + 78),
                      16.0f, kMuted);

        const render::Rect cta =
            render::rect(cx - 140.0f, panel.bottom - 96.0f, cx + 140.0f, panel.bottom - 40.0f);
        drawPillButton(r, cta, L"Premi INVIO per riprovare", kBad);

        if (reason == app::FailReason::NoSignal) {
            drawStatusHint(
                r, render::rect(panel.left + 32, panel.bottom - 30, panel.right - 32, panel.bottom - 6),
                L"M per continuare comunque, con una banda generica non personale",
                kMuted);
        }
        return;
    }

    // Concentrate e Relax sono gia' stati gestiti sopra, con la loro schermata
    // spoglia: qui non arriva piu' nessuno stato che debba disegnare qualcosa.
}

double fieldWidthMeters(double magnification) {
    if (magnification <= 0.0) return 0.0;
    return (config::kReferenceWidthMm / 1000.0) / magnification;
}

double niceLength(double v) {
    if (v <= 0.0) return 0.0;
    const double exp10 = std::pow(10.0, std::floor(std::log10(v)));
    const double m = v / exp10;
    const double mant = (m >= 5.0) ? 5.0 : (m >= 2.0 ? 2.0 : 1.0);
    return mant * exp10;
}

std::wstring formatLength(double meters) {
    struct Unita { double fattore; const wchar_t* nome; };
    static const Unita scale[] = {
        {1e-9, L"nm"}, {1e-6, L"µm"}, {1e-3, L"mm"}, {1e-2, L"cm"}, {1.0, L"m"},
    };
    for (const auto& u : scale) {
        const double v = meters / u.fattore;
        if (v < 1000.0) {
            const int dec = (v < 10.0) ? 1 : 0;
            std::wstring s = fixed(v, dec);
            if (dec == 1 && s.size() > 2 && s.substr(s.size() - 2) == L".0") {
                s = s.substr(0, s.size() - 2);
            }
            return s + L" " + u.nome;
        }
    }
    return fixed(meters, 1) + L" m";
}

void drawProjectionScale(render::Renderer& r, const control::CrossfadeState& cf) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;

    const float testo = std::max(24.0f, win.height * 0.055f);
    const float padX  = testo * 0.9f;
    const float padY  = testo * 0.34f;

    const std::wstring etichetta = std::to_wstring(cf.magnification) + L"×";
    const float boxW = testo * (0.62f * etichetta.size()) + padX * 2.0f;
    const float boxH = testo + padY * 2.0f;
    const float boxY = win.height * 0.035f;

    const render::Rect box =
        render::rect(cx - boxW / 2, boxY, cx + boxW / 2, boxY + boxH);
    r.fillRectShadow(box, {0.05f, 0.06f, 0.08f, 0.85f}, boxH * 0.5f, {0.0f, 0.0f, 0.0f, 0.4f},
                    16.0f, {0.0f, 4.0f});
    r.drawRectOutline(box, {kAccent.r, kAccent.g, kAccent.b, 0.45f}, 1.5f, boxH * 0.5f);
    r.drawTextCentered(etichetta, {cx, (box.top + box.bottom) * 0.5f}, testo, kInk, true);

    const double campo = fieldWidthMeters(cf.magnification);
    if (campo <= 0.0) return;

    const double perPixel = campo / win.width;
    const double target   = perPixel * (win.width * 0.20);
    const double lunghezza = niceLength(target);
    if (lunghezza <= 0.0) return;

    const auto  barW = static_cast<float>(lunghezza / perPixel);
    const float barY = win.height * 0.93f;
    const float tick = std::max(8.0f, win.height * 0.014f);
    const float spess = std::max(2.0f, win.height * 0.004f);

    const float x0 = cx - barW / 2.0f;
    const float x1 = cx + barW / 2.0f;

    const float etichettaH = std::max(15.0f, win.height * 0.026f);
    const render::Rect sfondo =
        render::rect(x0 - 28.0f, barY - etichettaH - 18.0f, x1 + 28.0f, barY + tick + 12.0f);
    r.fillRectShadow(sfondo, {0.05f, 0.06f, 0.08f, 0.6f}, 12.0f, {0.0f, 0.0f, 0.0f, 0.35f}, 14.0f,
                    {0.0f, 3.0f});

    const render::Color bianco{1.0f, 1.0f, 1.0f, 0.92f};
    r.fillRect(render::rect(x0, barY, x1, barY + spess), bianco);
    r.fillRect(render::rect(x0, barY - tick / 2, x0 + spess, barY + tick), bianco);
    r.fillRect(render::rect(x1 - spess, barY - tick / 2, x1, barY + tick), bianco);

    r.drawTextCentered(formatLength(lunghezza), {(x0 + x1) * 0.5f, barY - etichettaH - 10.0f},
                       etichettaH, bianco, true);
}

void drawHud(render::Renderer& r, const app::ControlState& st, const control::ZoomController& zoom,
             const control::CrossfadeState& cf, const control::Tunables& t) {
    const float x = 24.0f;
    float y = 20.0f;
    // Il pannello diagnostico e' un elenco di letture, non un'intestazione:
    // resta sul sans di corpo (Iowan Old Style e' per titoli e accenti).
    const auto line = [&](const std::wstring& s, render::Color c, float size = 14.0f) {
        r.drawTextBody(s, render::rect(x, y, x + 460, y + size + 8), size, c,
                      render::TextAlign::Left);
        y += size + 7.0f;
    };

    if (st.replaying) {
        line(L"RIPRODUZIONE (fascia non in uso)", kAccent, 15.0f);
        if (!g.replayName.empty()) line(L"  " + g.replayName, kMuted, 12.0f);
    } else {
        const wchar_t* bleNames[] = {L"DISCONNESSO", L"RICERCA", L"CONNESSIONE", L"STREAMING"};
        const int bs = std::clamp(st.bleState, 0, 3);
        line(std::wstring(L"Muse: ") + bleNames[bs],
             bs == 3 ? kOk : (bs == 0 ? kBad : kWarn), 15.0f);
    }

    const wchar_t* phaseNames[] = {L"IDLE",   L"ONBOARDING", L"HOOK", L"HANDOVER",
                                   L"INTERACTIVE", L"OUTRO", L"DONE"};
    std::wstring phaseLine = std::wstring(L"Fase: ") +
                             phaseNames[std::clamp(st.phase, 0, 6)];
    if (zoom.locked()) phaseLine += L" (HOLD)";
    line(phaseLine, kInk);

    if (st.signalFault != 0) {
        const wchar_t* faults[] = {L"OK", L"RETE 50 Hz", L"SATURO", L"PIATTO",
                                   L"NON E' UN SEGNALE"};
        const int fi = std::clamp(st.signalFault, 0, 4);
        line(std::wstring(L"Segnale: ") + faults[fi], kBad, 15.0f);

        if (fi == static_cast<int>(dsp::SignalFault::Mains)) {
            line(L"  " + fixed(st.mainsFraction * 100.0, 0) +
                     L"% a 50 Hz DOPO la derivazione bipolare: non e' modo comune",
                 kMuted, 12.0f);
            line(L"  i due frontali leggono cose diverse: uno dei due non e' accoppiato",
                 kMuted, 12.0f);
        } else {
            line(L"  correlazione " + fixed(st.autocorr1, 2) + L" (serve > " +
                     fixed(config::kMinAutocorr1, 2) + L")   saturi " +
                     fixed(st.railFraction * 100.0, 1) + L"%   rete " +
                     fixed(st.mainsFraction * 100.0, 0) + L"%",
                 kMuted, 12.0f);
        }
    } else {
        const wchar_t* gate = !st.contactOk ? L"CONTATTO" : (st.artifact ? L"ARTEFATTO" : L"OK");
        line(std::wstring(L"Segnale: ") + gate,
             !st.contactOk ? kBad : (st.artifact ? kWarn : kOk));
    }

    if (st.stalled) line(L"Flusso fermo: ripresa in corso", kBad);

    line(L"Indice: " + fixed(st.rawIndex, 3) + L"   c: " + fixed(st.smoothedIndex, 3), kMuted);

    if (st.calibValid) {
        line(L"Banda: [" + fixed(st.absMin, 2) + L" .. " + fixed(st.absMax, 2) + L"]   M: " +
                 fixed(st.neutral, 2), kMuted);
        line(L"Locale: [" + fixed(st.localMin, 2) + L" .. " + fixed(st.localMax, 2) + L"]", kMuted);
    } else {
        line(L"Banda: calibrazione in corso", kMuted);
    }

    line(L"Velocita': " + fixed(st.velocity, 3) + L"  (cruda " + fixed(st.velocityRaw, 3) + L")",
         st.velocity > 0 ? kOk : (st.velocity < 0 ? kWarn : kMuted));

    if (st.calibValid && st.absMax > st.absMin) {
        const float bw = 460.0f, bh = 22.0f, by = y;
        const auto toX = [&](double v) {
            const double f = (v - st.absMin) / (st.absMax - st.absMin);
            return x + static_cast<float>(std::clamp(f, 0.0, 1.0)) * bw;
        };

        r.fillRect(render::rect(x, by, x + bw, by + bh), {1, 1, 1, 0.06f}, 4.0f);
        r.fillRect(render::rect(toX(st.localMin), by, toX(st.localMax), by + bh),
                   {1, 1, 1, 0.07f}, 4.0f);

        const double tolUp = t.localTolerance * (st.absMax - st.neutral);
        const double tolDn = t.localTolerance * (st.neutral - st.absMin);
        r.fillRect(render::rect(toX(st.localMax - tolUp), by, toX(st.localMax), by + bh),
                   {kOk.r, kOk.g, kOk.b, 0.24f}, 4.0f);
        r.fillRect(render::rect(toX(st.localMin), by, toX(st.localMin + tolDn), by + bh),
                   {kWarn.r, kWarn.g, kWarn.b, 0.24f}, 4.0f);

        const float nx = toX(st.neutral);
        r.fillRect(render::rect(nx - 1.0f, by, nx + 1.0f, by + bh), kMuted);

        const float px = toX(st.smoothedIndex);
        r.fillRect(render::rect(px - 2.0f, by - 3.0f, px + 2.0f, by + bh + 3.0f), kAccent, 2.0f);

        y += bh + 7.0f;
        line(L"Gate: " + fixed(st.gate, 2) + L"   Ampiezza: " + fixed(st.magnitude, 2),
             st.gate > 0.05 ? kOk : kMuted, 13.0f);
    }

    line(L"Focus: " + fixed(zoom.targetFocus(), 3) + L" -> " + fixed(zoom.currentFocus(), 3), kMuted);
    line(L"Bande  t:" + fixed(st.theta, 1) + L"  a:" + fixed(st.alpha, 1) + L"  b:" +
             fixed(st.beta, 1), kMuted);
    line(L"Ampiezza: " + fixed(st.maxAbsRaw, 0) + L" uV   pkt: " +
             std::to_wstring(st.packetLen) + L"B/" + std::to_wstring(st.packetSamples) + L"smp",
         kMuted);

    {
        const std::wstring counts = L"Pacchetti: " + std::to_wstring(st.rawPackets) +
                                    L" grezzi / " + std::to_wstring(st.validPackets) + L" validi";
        render::Color c = kMuted;
        if (st.bleState == 3 && st.rawPackets == 0)                 c = kBad;
        else if (st.rawPackets > 0 && st.validPackets == 0)         c = kWarn;
        line(counts, c);
    }

    line(L"Ingrandimento: " + std::to_wstring(cf.magnification) + L"x", kAccent, 15.0f);

    const auto logLines = g.bleLogSnapshot();
    if (!logLines.empty()) {
        y += 8.0f;
        line(L"Bluetooth:", {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
        for (const auto& l : logLines) line(L"  " + l, kMuted, 12.0f);
    }

    if (st.recording) {
        const double secs = static_cast<double>(st.recordedSamples) / config::kSampleRate;
        line(L"Registrazione: " + fileNameOf(g.recorder.path()) + L"  (" +
                 fixed(secs, 0) + L"s)", kOk, 12.0f);
    }

    y += 8.0f;
    line(L"Sensibilita': " + fixed(t.sensitivity, 1) + L"x    Tolleranza: " +
             fixed(t.localTolerance, 2) + L"    Smoothing: " + fixed(t.velTauS, 2) + L"s" +
             L"    Elastico: " + fixed(t.elasticTauS, 2) + L"s",
         kAccent, 12.0f);
    if (!t.holdEnabled) line(L"Hold/detent SPENTO (diagnostica)", kWarn, 12.0f);

    y += 6.0f;
    line(L"su/giu sensibilita'   sin/des tolleranza   S smoothing   E elastico   L hold   "
         L"R reset (tienilo premuto: riavvia tutto)",
         {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
    line(L"H pannello   B bluetooth   K ricalibra   V debug calibrazione   ESC/Q esce",
         {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
}

/**
 * Modale di gestione Bluetooth (tasto B): stato della connessione e due
 * azioni a comando (C riconnetti, X disconnetti), invece del solo testo
 * passivo del pannello diagnostico. Sopra a tutto il resto, sfondo scurito.
 */
/**
 * Overlay di debug (tasto V): due grafici sulla finestra recente per capire a
 * colpo d'occhio come procede la calibrazione, invece di dover leggere il log
 * su terminale - campioni effettivi rispetto al traguardo, e quanto e'
 * correlato il segnale che li produce (piu' e' vicino a 1, meno ogni nuovo
 * campione conta come informazione indipendente).
 */
void drawCalibDebug(render::Renderer& r, const app::TelemetryHistory& history,
                    const app::ControlState& st) {
    const auto  win = r.size();
    const float w   = std::min(620.0f, win.width * 0.75f);
    const float h   = 300.0f;
    const float x0  = 24.0f;
    const float y0  = win.height - h - 24.0f;

    const render::Rect box = render::rect(x0, y0, x0 + w, y0 + h);
    r.fillRectShadow(box, kPanel, 16.0f, kShadow, 24.0f, {0.0f, 8.0f});
    r.fillRectGradient(box, kPanel, darken(kPanel, 0.10f), 16.0f);
    r.drawRectOutline(box, {kAccent.r, kAccent.g, kAccent.b, 0.35f}, 1.5f, 16.0f);

    r.drawTextBody(L"Debug calibrazione (V per chiudere)",
                   render::rect(box.left + 14, box.top + 10, box.right - 14, box.top + 28),
                   13.0f, kMuted, render::TextAlign::Left);

    constexpr double kWindowS = 60.0;
    const auto visible = static_cast<std::size_t>(kWindowS * config::kControlHz);
    const std::size_t count = std::min(history.size(), visible);
    const std::size_t first = history.size() - count;

    const float graphX0 = box.left + 14.0f, graphX1 = box.right - 14.0f;
    const float gapY = 10.0f;
    const float rowH = (box.bottom - (box.top + 36.0f) - gapY - 30.0f) / 2.0f;

    const auto plotSeries = [&](render::Rect g, double lo, double hi,
                                double (*get)(const app::TelemetrySample&),
                                render::Color color) {
        r.fillRect(g, {1.0f, 1.0f, 1.0f, 0.04f}, 6.0f);
        r.drawRectOutline(g, {1.0f, 1.0f, 1.0f, 0.10f}, 1.0f, 6.0f);
        if (count < 2 || hi <= lo) return;
        std::vector<render::Point> pts;
        pts.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double v = get(history.at(first + i));
            const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
            const float x = g.left + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                                         (g.right - g.left);
            const float y = static_cast<float>(g.bottom - t * (g.bottom - g.top));
            pts.push_back({x, y});
        }
        r.drawPolyline(pts.data(), pts.size(), color, 1.8f);
    };

    // --- 1. Campioni effettivi vs traguardo ---
    {
        const render::Rect g = render::rect(graphX0, box.top + 36.0f, graphX1,
                                            box.top + 36.0f + rowH);
        const double target = config::kCalibTargetEffSamples;
        const double hi = std::max(target * 1.3, st.calibEffN * 1.15);
        r.drawTextBody(L"Campioni effettivi (n_eff)",
                       render::rect(g.left, g.top - 16.0f, g.right, g.top), 11.0f,
                       render::Color{kMuted.r, kMuted.g, kMuted.b, kMuted.a * 0.8f}, render::TextAlign::Left);
        const float targetY = g.bottom - static_cast<float>(target / hi) * (g.bottom - g.top);
        r.drawLine({g.left, targetY}, {g.right, targetY},
                  render::Color{kWarn.r, kWarn.g, kWarn.b, kWarn.a * 0.6f}, 1.0f);
        plotSeries(g, 0.0, hi,
                  [](const app::TelemetrySample& s) { return s.calibEffN; }, kAccent);
        r.drawTextBody(L"n_eff " + std::to_wstring(static_cast<int>(st.calibEffN)) + L" / " +
                          std::to_wstring(static_cast<int>(target)),
                       render::rect(g.right - 140, g.top + 4, g.right - 4, g.top + 20), 11.0f,
                       kAccent, render::TextAlign::Center);
    }

    // --- 2. Autocorrelazione a ritardo 1 ---
    {
        const render::Rect g = render::rect(graphX0, box.top + 36.0f + rowH + gapY, graphX1,
                                            box.top + 36.0f + rowH + gapY + rowH);
        r.drawTextBody(L"Autocorrelazione (0 = indipendente, 1 = ripetuto)",
                       render::rect(g.left, g.top - 16.0f, g.right, g.top), 11.0f,
                       render::Color{kMuted.r, kMuted.g, kMuted.b, kMuted.a * 0.8f}, render::TextAlign::Left);
        plotSeries(g, 0.0, 1.0,
                  [](const app::TelemetrySample& s) { return s.autocorr1; }, kWarn);
        r.drawTextBody(L"rho " + fixed(st.autocorr1, 2),
                       render::rect(g.right - 90, g.top + 4, g.right - 4, g.top + 20), 11.0f,
                       kWarn, render::TextAlign::Center);
    }
}

void drawBleModal(render::Renderer& r, const app::ControlState& st) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;
    const float cy  = win.height * 0.5f;
    const float w   = std::min(520.0f, win.width * 0.7f);
    const float h   = 260.0f;

    r.fillRect(render::rect(0, 0, win.width, win.height), {0.0f, 0.0f, 0.0f, 0.6f});

    const render::Rect box = render::rect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    r.fillRectShadow(box, kPanel, 20.0f, kShadow, 30.0f, {0.0f, 10.0f});
    r.fillRectGradient(box, kPanel, darken(kPanel, 0.10f), 20.0f);
    r.drawRectOutline(box, {kAccent.r, kAccent.g, kAccent.b, 0.35f}, 1.5f, 20.0f);

    float y = box.top + 40.0f;
    r.drawTextCentered(L"Bluetooth", {cx, y}, 26.0f, kInk, true);
    y += 46.0f;

    if (st.replaying) {
        r.drawTextBodyCentered(L"Sorgente: dati simulati/riproduzione", {cx, y}, 16.0f, kAccent);
        y += 26.0f;
        r.drawTextBodyCentered(L"C torna alla fascia reale", {cx, y}, 13.0f, kMuted);
    } else {
        const wchar_t* bleNames[] = {L"Disconnesso", L"Ricerca in corso", L"Connessione in corso",
                                     L"Streaming"};
        const int bs = std::clamp(st.bleState, 0, 3);
        const render::Color stColor = (bs == 3) ? kOk : (bs == 0 ? kBad : kWarn);
        r.drawTextBodyCentered(std::wstring(L"Stato: ") + bleNames[bs], {cx, y}, 17.0f, stColor,
                              true);
        y += 30.0f;

        const std::string name = g_muse.deviceName();
        if (!name.empty()) {
            const std::wstring wname(name.begin(), name.end());
            r.drawTextBodyCentered(L"Dispositivo: " + wname, {cx, y}, 14.0f, kMuted);
            y += 24.0f;
        }
    }

    y = box.bottom - 70.0f;
    r.drawTextBodyCentered(L"C connetti/riconnetti      X disconnetti", {cx, y}, 15.0f, kInk);
    y += 26.0f;
    r.drawTextBodyCentered(L"ESC chiude", {cx, y}, 12.0f, kMuted);
}

// ---------------------------------------------------------------------------
// Doppio schermo: la scelta dello schermo di proiezione, porto di main.cpp
// (Windows). fieldWidthMeters/niceLength/formatLength/drawProjectionScale
// esistono gia' piu' sopra (gia' usate in schermo singolo). Lo stato di
// scelta (displays/choosing/candidate) NON vive qui: e' gestione di finestre,
// di proprieta' dello shell (vedi ProjectionState in experience.hpp) - qui
// arriva gia' risolto, solo per disegnare.
// ---------------------------------------------------------------------------

/**
 * Sullo schermo candidato durante la scelta: un numero gigante col nome e la
 * risoluzione. La finestra salta fisicamente da uno schermo all'altro mentre
 * si scorre, cosi' la scelta si conferma guardando, non leggendo un elenco e
 * sperando che "Display 2" sia quello giusto.
 */
void drawScreenPicker(render::Renderer& r, int candidate, const std::vector<Display>& displays) {
    const auto win = r.size();
    r.fillRect(render::rect(0, 0, win.width, win.height), {0.03f, 0.05f, 0.08f, 1.0f});

    const std::wstring number = std::to_wstring(candidate + 1);
    r.drawText(number,
               render::rect(0, win.height * 0.5f - 190.0f, win.width, win.height * 0.5f + 60.0f),
               260.0f, kAccent, render::TextAlign::Center, true);

    std::wstring caption = L"Questo schermo";
    if (candidate >= 0 && candidate < static_cast<int>(displays.size())) {
        caption += L"  -  " + displays[static_cast<std::size_t>(candidate)].describe();
    }
    r.drawText(caption,
               render::rect(0, win.height * 0.5f + 70.0f, win.width, win.height * 0.5f + 110.0f),
               22.0f, kInk, render::TextAlign::Center);

    r.drawText(L"Frecce per cambiare schermo   -   INVIO per confermare",
               render::rect(0, win.height * 0.5f + 130.0f, win.width, win.height * 0.5f + 170.0f),
               18.0f, kMuted, render::TextAlign::Center);
}

/** Sullo schermo dell'operatore durante la scelta: l'elenco, col candidato evidenziato. */
void drawScreenPickerPanel(render::Renderer& r, int candidate, const std::vector<Display>& displays) {
    const auto win = r.size();
    const float cx = win.width * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), {0.0f, 0.0f, 0.0f, 0.78f});

    const float panelW = 620.0f;
    const float rowH   = 52.0f;
    const float panelH = 250.0f + rowH * static_cast<float>(displays.size());
    const render::Rect panel = render::rect(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    r.fillRectShadow(panel, kPanel, 18.0f, kShadow, 30.0f, {0.0f, 10.0f});
    r.fillRectGradient(panel, kPanel, darken(kPanel, 0.10f), 18.0f);
    r.drawRectOutline(panel, {1, 1, 1, 0.10f}, 1.0f, 18.0f);

    float y = panel.top + 34.0f;
    r.drawText(L"Su quale schermo proiettare?",
               render::rect(panel.left, y, panel.right, y + 40.0f), 28.0f, kInk,
               render::TextAlign::Center, true);
    y += 56.0f;

    r.drawText(L"Il partecipante vedra' solo l'immagine, a schermo intero.\n"
               L"Qui restano la telemetria e i comandi.",
               render::rect(panel.left + 36, y, panel.right - 36, y + 60.0f), 16.0f, kMuted);
    y += 76.0f;

    for (std::size_t i = 0; i < displays.size(); ++i) {
        const bool sel = (static_cast<int>(i) == candidate);
        const render::Rect row = render::rect(panel.left + 30, y, panel.right - 30, y + rowH - 8.0f);
        if (sel) {
            r.fillRect(row, {kAccent.r, kAccent.g, kAccent.b, 0.20f}, 8.0f);
            r.drawRectOutline(row, {kAccent.r, kAccent.g, kAccent.b, 0.65f}, 1.5f, 8.0f);
        }
        r.drawText(std::to_wstring(i + 1) + L".   " + displays[i].describe(),
                   render::rect(row.left + 18, row.top + 10, row.right - 18, row.bottom),
                   18.0f, sel ? kInk : kMuted, render::TextAlign::Left, sel);
        y += rowH;
    }

    y += 14.0f;
    r.drawText(L"Frecce per cambiare   -   INVIO per confermare",
               render::rect(panel.left, y, panel.right, y + 30.0f), 17.0f, kAccent,
               render::TextAlign::Center, true);
    y += 34.0f;
    r.drawText(L"Il numero compare a schermo intero sullo schermo evidenziato.",
               render::rect(panel.left + 24, y, panel.right - 24, y + 26.0f), 13.0f, kMuted,
               render::TextAlign::Center);
}

// --- stato di rendering, vive nel thread che chiama frame() (il thread principale) ---
control::ZoomController zoom;
double        focusFrac = 0.5;
double        animT     = 0.0;   // orologio decorativo: pulsazioni della scheda di calibrazione
util::Ema     velRender;
app::TelemetryHistory history;
std::chrono::steady_clock::time_point lastFrameTime;
bool          initialized = false;

} // namespace

std::string start(const StartOptions& opt) {
    g.running.store(true, std::memory_order_release);
    g.quitRequested.store(false, std::memory_order_release);
    // Ogni avvio riparte dalla pagina d'ingresso, anche se il processo era
    // gia' stato usato: start() e' il punto in cui l'esperienza ricomincia.
    g.landingVisible.store(true, std::memory_order_relaxed);
    // Va scritto PRIMA che parta il thread DSP, che lo legge una volta sola.
    g.adaptiveBand.store(opt.adaptiveBand, std::memory_order_relaxed);
    g.hudVisible.store(!opt.hudHidden, std::memory_order_relaxed);

    g.openDebugLog(opt.debugDir);

    if (opt.record) {
        g.recorder.arm(newRecordingPath(opt.recordingsDir));
    }

    g_muse.onSample([](const ble::Sample& s) {
        if (!g.ring.push(s)) g.dropped.fetch_add(1, std::memory_order_relaxed);
    });
    g_muse.onRawPacket([](const std::uint8_t* d, std::size_t n) {
        RawPacket p;
        p.len = static_cast<std::uint16_t>(std::min<std::size_t>(n, sizeof(p.data)));
        std::memcpy(p.data, d, p.len);
        g.rawRing.push(p);
    });
    g_muse.onLog([](const std::string& msg) { g.pushBleLog(msg); });

    if (!opt.replayPath.empty()) {
        const std::string err = startReplayInternal(opt.replayPath);
        if (!err.empty()) return err;
    } else {
        g_muse.start();
    }

    g_dspThread = std::thread(dspThread);

    zoom = control::ZoomController{};
    focusFrac = 0.5;
    velRender = util::Ema{};
    history.clear();
    lastFrameTime = std::chrono::steady_clock::now();

    g.publishTunables();
    initialized = true;
    return {};
}

void stop() {
    if (!initialized) return;
    g.running.store(false, std::memory_order_release);
    if (g_dspThread.joinable()) g_dspThread.join();
    stopReplayInternal();

    const auto st = g.state.read();
    g.closeDebugLog("fine sessione: bleState=" + std::to_string(st.bleState) +
                    " rawPackets=" + std::to_string(st.rawPackets) +
                    " validPackets=" + std::to_string(st.validPackets) +
                    " packetLen=" + std::to_string(st.packetLen) +
                    " packetSamples=" + std::to_string(st.packetSamples));

    g_muse.stop();
    g.recorder.close();
    initialized = false;
}

void handleKey(Key key) {
    switch (key) {
        case Key::Escape:
            // La modale si chiude con lo stesso tasto che chiude tutto il
            // resto: se e' aperta lo intercetta lei, altrimenti si esce.
            if (g.bleModalVisible.load(std::memory_order_relaxed)) {
                g.bleModalVisible.store(false, std::memory_order_relaxed);
                return;
            }
            g.quitRequested.store(true, std::memory_order_release);
            return;
        case Key::Q:
            g.quitRequested.store(true, std::memory_order_release);
            return;
        case Key::B:
            g.bleModalVisible.store(!g.bleModalVisible.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
            return;
        case Key::C:
            if (g.bleModalVisible.load(std::memory_order_relaxed)) reconnectInternal();
            return;
        case Key::X:
            if (g.bleModalVisible.load(std::memory_order_relaxed)) disconnectInternal();
            return;
        case Key::V:
            g.calibDebugVisible.store(!g.calibDebugVisible.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            return;
        case Key::D: {
            // Solo quando ha senso: niente fascia viva e non gia' in
            // riproduzione, altrimenti scavalcherebbe un segnale vero o una
            // registrazione gia' in corso senza che l'utente l'abbia chiesto.
            const auto st = g.state.read();
            if (!st.replaying && !st.signalFresh) startSyntheticInternal();
            return;
        }
        case Key::Enter: {
            // La pagina d'ingresso intercetta il primo INVIO: da li' si passa
            // alla schermata di calibrazione, che chiedera' il suo.
            if (g.landingVisible.load(std::memory_order_relaxed)) {
                g.landingVisible.store(false, std::memory_order_relaxed);
                // Lo zoom deve SEMPRE partire dal minimo: senza questo, il
                // segnale gia' arrivato mentre si sistemava la fascia (in
                // banda adattiva l'esperienza e' gia' "viva" da prima
                // dell'INVIO, vedi frame()) poteva aver spinto currentFocus_
                // avanti prima ancora che l'utente avesse scelto di iniziare -
                // osservato sul campo, si partiva gia' a 200x.
                g.resetHistory.store(true, std::memory_order_release);
                return;
            }
            const auto st = g.state.read();
            const auto stage = static_cast<control::CalibStage>(st.calibStage);
            if (stage == control::CalibStage::Intro) {
                g.command.store(static_cast<int>(app::Command::StartCalibration),
                                std::memory_order_release);
            } else if (stage == control::CalibStage::Failed) {
                g.command.store(static_cast<int>(app::Command::RetryCalibration),
                                std::memory_order_release);
            }
            return;
        }
        case Key::M: {
            // Solo dalla schermata di calibrazione non riuscita: un ripiego
            // esplicito, mai automatico - vedi Calibration::useFallbackProfile.
            const auto st = g.state.read();
            if (static_cast<control::CalibStage>(st.calibStage) == control::CalibStage::Failed) {
                g.command.store(static_cast<int>(app::Command::UseFallbackProfile),
                                std::memory_order_release);
                g.pushBleLog("fallback: banda generica al posto della calibrazione personale");
            }
            return;
        }
        case Key::K:
            // Torna alla calibrazione da QUALUNQUE punto, esperienza gia' in
            // corso compresa: e' la via d'uscita per chi si accorge a meta'
            // percorso che la banda misurata non lo rappresenta (misurata di
            // fretta, fascia sistemata male, un'altra persona che prova dopo).
            // Senza, l'unico modo era riavviare il programma. StartCalibration
            // riporta anche la fase a Onboarding, vedi dspThread.
            g.command.store(static_cast<int>(app::Command::StartCalibration),
                            std::memory_order_release);
            g.pushBleLog("ricalibrazione richiesta dall'utente");
            return;
        case Key::Up:    g.tune.adjustSensitivity(+1); g.publishTunables(); return;
        case Key::Down:  g.tune.adjustSensitivity(-1); g.publishTunables(); return;
        case Key::Right: g.tune.adjustTolerance(+1);   g.publishTunables(); return;
        case Key::Left:  g.tune.adjustTolerance(-1);   g.publishTunables(); return;
        case Key::S:     g.tune.adjustSmoothing(+1);   g.publishTunables(); return;
        case Key::ShiftS:g.tune.adjustSmoothing(-1);   g.publishTunables(); return;
        case Key::E:     g.tune.adjustElastic(+1);     g.publishTunables(); return;
        case Key::ShiftE:g.tune.adjustElastic(-1);     g.publishTunables(); return;
        case Key::L:     g.tune.holdEnabled = !g.tune.holdEnabled; g.publishTunables(); return;
        case Key::R:     g.tune.reset();               g.publishTunables(); return;
        case Key::RHold:
            // Riavvio completo, distinto dal semplice tap su R (che riporta
            // solo le manopole ai default): tenuto premuto apposta - vedi la
            // soglia in shell_macos.mm - perche' butta via la banda adattiva
            // e i progressi della sessione corrente, non solo la taratura.
            // RestartSession (non StartCalibration) azzera anche STFT e
            // gating, non solo calibrazione/smoothing: e' il reset piu'
            // completo che dspThread sa fare senza toccare il BLE.
            g.landingVisible.store(true, std::memory_order_relaxed);
            g.command.store(static_cast<int>(app::Command::RestartSession),
                            std::memory_order_release);
            g.resetHistory.store(true, std::memory_order_release);
            g.pushBleLog("riavvio completo richiesto (R tenuto premuto)");
            return;
        case Key::H:
            g.hudVisible.store(!g.hudVisible.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            return;
    }
}

bool wantsQuit() { return g.quitRequested.load(std::memory_order_acquire); }

void frame(render::Renderer& r, double dt, const ProjectionState& proj) {
    if (!initialized) return;

    const auto st = g.state.read();
    const auto phase = static_cast<control::Phase>(st.phase);
    const bool showLanding = g.landingVisible.load(std::memory_order_relaxed);

    const double velocity = velRender.push(st.velocity, dt, config::kVelRenderTauS);
    // Prima dell'INVIO che chiude la pagina d'ingresso l'esperienza non e'
    // ancora "iniziata" per l'utente, anche se in banda adattiva il thread DSP
    // e' gia' in Interactive fin dal primo istante (vedi dspThread). Onboarding
    // e' l'unica fase per cui authorityVelocity torna sempre 0: usarla qui
    // impedisce che lo zoom derivi mentre ci si sistema la fascia, cosi' non
    // c'e' niente da annullare quando poi si preme INVIO.
    const auto zoomPhase = showLanding ? control::Phase::Onboarding : phase;
    zoom.update(velocity, dt, zoomPhase, st.phaseElapsed, g.tune);

    focusFrac += (st.calibDisplayTarget - focusFrac) * config::kCalibDisplayEma;
    animT += dt;

    if (g.resetHistory.exchange(false, std::memory_order_acq_rel)) {
        history.clear();
        zoom.reset();
        velRender.reset();
    }

    history.append(st, zoom.targetFocus(), zoom.currentFocus(), zoom.locked());

    const auto cf = zoom.crossfade();
    const bool showCard = !showLanding && (phase == control::Phase::Onboarding);

    // --- schermo di proiezione: il partecipante, solo se lo shell ne ha creato uno ---
    // A differenza dello schermo dell'operatore (sotto, invariato: qui il
    // pannello diagnostico e' gia' un riquadro compatto sopra l'immagine, non
    // ha bisogno di una miniatura come in main.cpp/Windows), qui NON deve
    // comparire altro che l'immagine, la scala, e - durante l'onboarding - la
    // scheda di calibrazione, cosi' chi guida vede cio' che vede il
    // partecipante mentre lo guida.
    if (proj.renderer) {
        render::Renderer& pr = *proj.renderer;
        pr.begin(kBg);
        if (proj.choosing) {
            drawScreenPicker(pr, proj.candidate, *proj.displays);
        } else if (!showLanding) {
            pr.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                         static_cast<float>(cf.activeAlpha));
            pr.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                         static_cast<float>(cf.nextAlpha));
            if (showCard) {
                drawCalibrationCard(pr, st, focusFrac, animT);
            } else {
                drawProjectionScale(pr, cf);
                if (st.adaptiveActive && !st.adaptiveReady) drawWarmupOverlay(pr, st);
            }
        }
        // showLanding senza choosing: sfondo e basta, la pagina d'ingresso e'
        // testo per l'operatore, il partecipante non ha ancora niente da vedere.
        pr.end();
    }

    r.begin(kBg);
    // La calibrazione e' a schermo intero apposta: la foto al microscopio
    // dietro distrarrebbe proprio nella fase che chiede piu' attenzione, quindi
    // durante l'onboarding non si disegna ne' lei ne' la scala di proiezione.
    // Lo stesso vale, a maggior ragione, per la pagina d'ingresso.
    if (showLanding) {
        drawLandingPage(r, st, animT);
    } else if (!showCard) {
        r.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                    static_cast<float>(cf.activeAlpha));
        r.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                    static_cast<float>(cf.nextAlpha));
        drawProjectionScale(r, cf);
        // Con la banda adattiva la foto e' gia' viva mentre la banda si forma:
        // il riscaldamento e' un velo sopra, non una schermata al posto.
        if (st.adaptiveActive && !st.adaptiveReady) drawWarmupOverlay(r, st);
    } else {
        drawCalibrationCard(r, st, focusFrac, animT);
    }

    if (g.hudVisible.load(std::memory_order_relaxed)) {
        drawHud(r, st, zoom, cf, g.tune);
    }

    if (g.bleModalVisible.load(std::memory_order_relaxed)) {
        drawBleModal(r, st);
    }

    if (g.calibDebugVisible.load(std::memory_order_relaxed)) {
        drawCalibDebug(r, history, st);
    }

    if (proj.choosing) {
        drawScreenPickerPanel(r, proj.candidate, *proj.displays);
    }

    r.end();
}

} // namespace mz::app::experience
