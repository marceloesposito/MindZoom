// Mind Zoom - applicazione nativa Windows.
//
// Tre thread, come da architettura del brief:
//   1. BLE   - dentro MuseClient: GATT WinRT, decodifica, push su ring lock-free
//   2. DSP   - consuma il ring: STFT, Pope, gating, calibrazione, velocità
//   3. Render (questo, main) - vsync, rate control, crossfade, HUD
//
// La comunicazione è senza lock: ring SPSC verso il DSP, double buffer atomico
// verso il render. Il thread di render non si blocca mai in attesa del segnale.

#include "app/shared_state.hpp"
#include "ble/muse.hpp"
#include "ble/recording.hpp"
#include "config.hpp"
#include "control/calibration.hpp"
#include "control/tunables.hpp"
#include "control/zoom.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include "render/renderer.hpp"
#include "util/double_buffer.hpp"
#include "util/smoothing.hpp"
#include "util/spsc_ring.hpp"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mz;

namespace {

// --- geometria della scheda di calibrazione (specchio del CSS della versione web) ---
constexpr float kRailW   = 64.0f;
constexpr float kRailH   = 300.0f;
constexpr float kSquareH = 40.0f;
constexpr float kTargetH = 44.0f;

constexpr render::Color kBg        {0.02f, 0.02f, 0.03f, 1.0f};
constexpr render::Color kInk       {0.90f, 0.93f, 0.97f, 1.0f};
constexpr render::Color kMuted     {0.62f, 0.66f, 0.73f, 1.0f};
constexpr render::Color kAccent    {0.22f, 0.74f, 0.94f, 1.0f};
constexpr render::Color kOk        {0.30f, 0.85f, 0.55f, 1.0f};
constexpr render::Color kWarn      {0.96f, 0.62f, 0.26f, 1.0f};
constexpr render::Color kBad       {0.94f, 0.36f, 0.36f, 1.0f};
constexpr render::Color kPanel     {0.05f, 0.06f, 0.09f, 0.92f};
constexpr render::Color kRailBg    {1.0f, 1.0f, 1.0f, 0.08f};
constexpr render::Color kTargetZone{0.30f, 0.85f, 0.55f, 0.22f};

struct Shared {
    util::SpscRing<ble::Sample, 16384> ring;
    util::DoubleBuffer<app::ControlState> state;

    // Manopole di taratura: la UI le muove coi tasti, il DSP le rilegge ad ogni
    // giro. Stesso meccanismo dello stato, in direzione opposta.
    // `tune` è la copia di lavoro, toccata SOLO dal thread della UI (windowProc e
    // loop di render sono lo stesso thread); `tunables` è la sua pubblicazione.
    control::Tunables                     tune;
    util::DoubleBuffer<control::Tunables> tunables;

    void publishTunables() { tunables.publish(tune); }

    std::atomic<int>           command{static_cast<int>(app::Command::None)};
    std::atomic<bool>          running{true};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool>          hudVisible{true};

    // Ultimi messaggi del BLE, mostrati nel pannello diagnostico: senza, quando
    // la fascia cade non si capisce il motivo. Frequenza bassissima, quindi un
    // mutex va benissimo e non tocca il percorso caldo.
    // Registrazione della sessione. Solo il thread DSP la tocca dopo l'avvio,
    // perche' e' lui che consuma il ring: scrivere dal callback GATT
    // significherebbe fare I/O su un thread che non deve mai bloccarsi.
    ble::Recorder recorder;

    // Riproduzione da file invece che dalla fascia.
    bool         replaying = false;
    std::wstring replayPath;

    std::mutex                bleLogMutex;
    std::deque<std::wstring>  bleLog;

    void pushBleLog(const std::string& msg) {
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

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring fixed(double v, int decimals) {
    wchar_t buf[64]{};
    swprintf(buf, 64, L"%.*f", decimals, v);
    return buf;
}

/** <exe>\registrazioni\sessione-AAAAMMGG-HHMMSS.mzr, cartella creata se manca. */
std::wstring newRecordingPath() {
    const std::wstring dir = exeDirectory() + L"\\registrazioni";
    CreateDirectoryW(dir.c_str(), nullptr);   // se esiste gia', va bene cosi'

    SYSTEMTIME t{};
    GetLocalTime(&t);

    wchar_t name[64]{};
    swprintf(name, 64, L"\\sessione-%04d%02d%02d-%02d%02d%02d.mzr",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return dir + name;
}

/** Nome del file senza il percorso: e' quello che ha senso mostrare nel pannello. */
std::wstring fileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// Thread 1-bis: riproduzione da file al posto della fascia
// ---------------------------------------------------------------------------

/**
 * Rigioca una registrazione nel ring, allo stesso ritmo dell'hardware.
 *
 * Scrive esattamente dove scriverebbe il callback GATT, quindi tutto cio' che
 * sta a valle non sa che il segnale viene da un file: nessun percorso
 * alternativo da tenere in vita, e quello che si osserva e' la pipeline vera.
 */
void replayThread(std::vector<ble::Sample> samples) {
    if (samples.empty()) return;

    const auto start = std::chrono::steady_clock::now();
    std::size_t sent = 0;

    while (g.running.load(std::memory_order_acquire) && sent < samples.size()) {
        // Quanti campioni sarebbero arrivati dall'hardware a quest'ora.
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

// ---------------------------------------------------------------------------
// Thread 2: DSP
// ---------------------------------------------------------------------------

void dspThread() {
    dsp::SlidingStft       stft;
    dsp::Gating            gating;
    control::IndexSmoother smoother;
    control::Calibration   calib;

    auto phase = control::Phase::Onboarding;
    double phaseElapsed = 0.0;
    double doneHold     = 0.0;
    double velocityRaw  = 0.0;     // uscita cruda della legge di controllo
    int    gatedWindows = 0;
    bool   contactOk    = false;   // finché non arriva un frame non si sa

    // Smoothing a valle del controllo: la legge produce un valore ogni 187 ms e
    // senza filtro il render ne vede lo scalino.
    util::Ema velocitySmooth;

    auto lastTick    = std::chrono::steady_clock::now();
    auto lastFrameAt = lastTick;

    app::ControlState st;
    st.phase = static_cast<int>(phase);

    constexpr double dt = config::kControlDt;

    while (g.running.load(std::memory_order_acquire)) {
        // Comandi dalla UI.
        const auto cmd = static_cast<app::Command>(
            g.command.exchange(static_cast<int>(app::Command::None), std::memory_order_acq_rel));
        if (cmd == app::Command::StartCalibration || cmd == app::Command::RetryCalibration) {
            calib.start();
            smoother.reset();
            velocityRaw = 0.0;
            velocitySmooth.reset();
        }

        const control::Tunables tune = g.tunables.read();

        ble::Sample s;
        bool consumed = false;

        // --- consumo dei campioni: tutto ciò che dipende dal SEGNALE ---
        while (g.ring.pop(s)) {
            consumed = true;
            // Si registra il campione grezzo, prima di qualunque elaborazione:
            // una registrazione gia' filtrata non permetterebbe di riprovare
            // tarature diverse del DSP.
            g.recorder.write(s);

            if (!stft.pushSample(s.uv, s.adc)) continue;

            ++st.frames;
            lastFrameAt = std::chrono::steady_clock::now();

            const auto quality = gating.assess(stft);
            dsp::Bands bands;
            const auto index = dsp::popeIndex(stft, &bands);
            if (index) st.rawIndex = *index;

            contactOk = quality.contactOk;

            // Gating del contatto: meglio uno zoom fermo che uno impazzito.
            if (!quality.contactOk) {
                velocityRaw = 0.0;
                ++gatedWindows;
            } else if (quality.artifact) {
                // La finestra sporca non alimenta l'indice. Si tiene la velocità
                // precedente per non introdurre uno scatto ad ogni blink, ma un
                // gating prolungato non deve incollare lo zoom.
                ++gatedWindows;
                if (gatedWindows > config::kArtifactHoldMaxS * config::kControlHz) {
                    velocityRaw *= 0.85;
                }
            } else if (index) {
                gatedWindows = 0;
                const double c = smoother.push(*index, dt);
                st.smoothedIndex = c;

                if (phase == control::Phase::Onboarding) {
                    calib.sample(c);
                } else {
                    velocityRaw = calib.velocity(c, dt, tune);
                }
            }

            st.theta = bands.theta; st.alpha = bands.alpha; st.beta = bands.beta;
            st.maxAbsRaw = quality.maxAbsRaw;
            st.contactOk = quality.contactOk;
            st.artifact  = quality.artifact;
        }

        // --- avanzamento a tempo reale ---
        // Le fasi NON possono dipendere dall'arrivo dei campioni: senza fascia
        // collegata l'interfaccia resterebbe congelata sulla schermata iniziale
        // e il tasto INVIO sembrerebbe non fare nulla.
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;

        // Il segnale è "fresco" se è arrivato un frame di recente: è questo che
        // distingue una pausa per contatto scarso da una fascia mai connessa.
        const bool signalFresh =
            st.frames > 0 && std::chrono::duration<double>(now - lastFrameAt).count() < 1.0;

        if (phase == control::Phase::Onboarding) {
            // Il conteggio avanza solo con segnale valido: una fascia storta o
            // assente mette in pausa invece di consumare la calibrazione.
            calib.tick(elapsed, signalFresh && contactOk);

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

        // Watchdog: il flusso BLE può fermarsi senza emettere alcun evento.
        // In riproduzione non ha senso: non c'è nessun link da rianimare, e a
        // registrazione esaurita farebbe lampeggiare un guasto inesistente.
        const auto silent = g_muse.millisSinceLastPacket();
        const bool stalled = !g.replaying && g_muse.streaming() && silent > 0 &&
                             silent > static_cast<std::int64_t>(config::kEegWatchdogS * 1000);
        if (stalled) {
            velocityRaw = 0.0;
            g_muse.resumeStreaming();
        }

        // --- smoothing della velocità ---
        // Senza segnale fresco la velocità DECADE invece di restare congelata:
        // prima, fra la perdita del flusso e lo scatto del watchdog a 3 s, lo
        // zoom continuava a muoversi su dati morti.
        double velocity = 0.0;
        if (signalFresh) {
            velocity = velocitySmooth.push(velocityRaw, elapsed, tune.velTauS);
        } else {
            velocityRaw = 0.0;
            velocity = velocitySmooth.decay(elapsed, config::kVelStaleTauS);
        }

        // --- pubblicazione: ogni giro, non solo quando arrivano campioni ---
        st.phase        = static_cast<int>(phase);
        st.phaseElapsed = phaseElapsed;
        st.calibStage   = static_cast<int>(calib.stage());
        st.calibDisplayTarget = calib.displayTarget();
        st.calibRemaining = (calib.stageDuration() > 0.0)
            ? (calib.stageDuration() - calib.stageElapsed()) : 0.0;
        st.calibValid = calib.valid();
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
        } else {
            st.failReason = static_cast<int>(app::FailReason::None);
        }

        st.stalled        = stalled;
        st.bleState       = static_cast<int>(g_muse.state());
        st.packetLen      = g_muse.lastPacketLen();
        st.packetSamples  = g_muse.lastPacketSamples();
        st.rawPackets     = g_muse.rawPackets();
        st.validPackets   = g_muse.validPackets();
        st.droppedSamples = g.dropped.load(std::memory_order_relaxed);
        st.replaying       = g.replaying;
        st.recording       = g.recorder.active();
        st.recordedSamples = g.recorder.count();
        g.state.publish(st);

        if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---------------------------------------------------------------------------
// Disegno
// ---------------------------------------------------------------------------

void drawCalibrationCard(render::Renderer& r, const app::ControlState& st, double squarePos) {
    const auto win = r.size();
    const float cx = win.width * 0.5f;

    // Velo scuro: la scheda deve staccare dallo sfondo.
    r.fillRect(D2D1::RectF(0, 0, win.width, win.height), {0.0f, 0.0f, 0.0f, 0.72f});

    const auto stage = static_cast<control::CalibStage>(st.calibStage);

    const float panelW = 520.0f;
    const float panelH = 560.0f;
    const D2D1_RECT_F panel = D2D1::RectF(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    r.fillRect(panel, kPanel, 18.0f);
    r.drawRectOutline(panel, {1, 1, 1, 0.10f}, 1.0f, 18.0f);

    const float top = panel.top + 36.0f;

    if (stage == control::CalibStage::Intro) {
        r.drawText(L"Calibrazione", D2D1::RectF(panel.left, top, panel.right, top + 50), 32.0f,
                   kInk, render::TextAlign::Center, true);
        r.drawText(L"Due fasi da 15 secondi. Prima ti concentri per spingere il quadratino\n"
                   L"verso l'obiettivo in alto, poi ti rilassi e lo lasci scendere.\n\n"
                   L"Servono a misurare i tuoi due estremi personali: da lì si ricava\n"
                   L"la soglia con cui guiderai lo zoom.",
                   D2D1::RectF(panel.left + 40, top + 70, panel.right - 40, panel.bottom - 120),
                   17.0f, kMuted);
        r.drawText(L"INVIO per iniziare",
                   D2D1::RectF(panel.left, panel.bottom - 80, panel.right, panel.bottom - 40),
                   19.0f, kAccent, render::TextAlign::Center, true);
        if (!st.signalFresh) {
            r.drawText(L"Nessun dato dalla fascia: puoi iniziare lo stesso,\n"
                       L"il conteggio partira' quando arriva il segnale.",
                       D2D1::RectF(panel.left + 24, panel.bottom - 130, panel.right - 24,
                                   panel.bottom - 85),
                       14.0f, kWarn);
        }
        return;
    }

    if (stage == control::CalibStage::Done) {
        r.drawText(L"Calibrazione completata",
                   D2D1::RectF(panel.left, win.height * 0.5f - 60, panel.right,
                               win.height * 0.5f - 10),
                   30.0f, kOk, render::TextAlign::Center, true);
        r.drawText(L"Concentrandoti aumenterai lo zoom, rilassandoti tornerai indietro.\n"
                   L"Buona esplorazione.",
                   D2D1::RectF(panel.left + 40, win.height * 0.5f + 10, panel.right - 40,
                               win.height * 0.5f + 100),
                   17.0f, kMuted);
        return;
    }

    if (stage == control::CalibStage::Failed) {
        const auto reason = static_cast<app::FailReason>(st.failReason);
        r.drawText(L"Calibrazione non riuscita",
                   D2D1::RectF(panel.left, win.height * 0.5f - 80, panel.right,
                               win.height * 0.5f - 30),
                   28.0f, kBad, render::TextAlign::Center, true);
        r.drawText(reason == app::FailReason::NoSignal
                       ? L"Segnale assente durante la calibrazione.\n"
                         L"Controlla che la fascia sia ben posizionata."
                       : L"Modulazione troppo debole: marca di piu' la differenza\n"
                         L"fra concentrazione e rilassamento.",
                   D2D1::RectF(panel.left + 40, win.height * 0.5f - 10, panel.right - 40,
                               win.height * 0.5f + 80),
                   17.0f, kMuted);
        r.drawText(L"INVIO per riprovare",
                   D2D1::RectF(panel.left, panel.bottom - 80, panel.right, panel.bottom - 40),
                   19.0f, kAccent, render::TextAlign::Center, true);
        return;
    }

    // --- fasi attive: binario verticale col quadratino ---
    const bool concentrate = (stage == control::CalibStage::Concentrate);

    r.drawText(concentrate ? L"Concentrazione" : L"Rilassamento",
               D2D1::RectF(panel.left, top, panel.right, top + 44), 30.0f, kInk,
               render::TextAlign::Center, true);
    r.drawText(concentrate
                   ? L"Concentrati per spingere il quadratino verso l'obiettivo in alto."
                   : L"Ora rilassati e lascia scendere il quadratino verso il basso.",
               D2D1::RectF(panel.left + 32, top + 52, panel.right - 32, top + 110), 17.0f, kMuted);

    const float railTop  = panel.top + 150.0f;
    const D2D1_RECT_F rail = D2D1::RectF(cx - kRailW / 2, railTop, cx + kRailW / 2,
                                         railTop + kRailH);
    r.fillRect(rail, kRailBg, 12.0f);

    // Zona obiettivo: in alto se ci si concentra, in basso se ci si rilassa.
    const D2D1_RECT_F target = concentrate
        ? D2D1::RectF(rail.left, rail.top, rail.right, rail.top + kTargetH)
        : D2D1::RectF(rail.left, rail.bottom - kTargetH, rail.right, rail.bottom);
    r.fillRect(target, kTargetZone, 10.0f);
    r.drawRectOutline(target, {kOk.r, kOk.g, kOk.b, 0.55f}, 1.5f, 10.0f);

    // Il quadratino: posizione 0 = fondo del binario, 1 = cima.
    const float travel = kRailH - kSquareH;
    const float sy = rail.bottom - kSquareH - static_cast<float>(squarePos) * travel;
    const D2D1_RECT_F square = D2D1::RectF(cx - kSquareH / 2, sy, cx + kSquareH / 2, sy + kSquareH);

    const bool reached = concentrate ? (squarePos > 0.8) : (squarePos < 0.2);
    render::Color squareColor = kAccent;
    if (!st.contactOk)   squareColor = kMuted;
    else if (reached)    squareColor = kOk;
    r.fillRect(square, squareColor, 8.0f);

    // Countdown.
    const int remaining = static_cast<int>(std::ceil(std::max(0.0, st.calibRemaining)));
    r.drawText(std::to_wstring(remaining),
               D2D1::RectF(panel.left, rail.bottom + 24, panel.right, rail.bottom + 80), 40.0f,
               kInk, render::TextAlign::Center, true);

    // Il conteggio si ferma sia senza fascia sia con contatto scarso: sono due
    // situazioni diverse e vanno dette in modo diverso, altrimenti sembra che il
    // programma sia bloccato.
    if (!st.signalFresh) {
        r.drawText(L"In attesa del segnale dalla fascia. Il conteggio e' in pausa.",
                   D2D1::RectF(panel.left + 24, panel.bottom - 60, panel.right - 24,
                               panel.bottom - 20),
                   15.0f, kWarn);
    } else if (!st.contactOk) {
        r.drawText(L"Contatto assente: sistema la fascia. Il conteggio e' in pausa.",
                   D2D1::RectF(panel.left + 24, panel.bottom - 60, panel.right - 24,
                               panel.bottom - 20),
                   15.0f, kWarn);
    }
}

void drawHud(render::Renderer& r, const app::ControlState& st, const control::ZoomController& zoom,
             const control::CrossfadeState* cf, const control::Tunables& t) {
    const float x = 24.0f;
    float y = 20.0f;
    const auto line = [&](const std::wstring& s, render::Color c, float size = 14.0f) {
        r.drawText(s, D2D1::RectF(x, y, x + 460, y + size + 8), size, c, render::TextAlign::Left);
        y += size + 7.0f;
    };

    if (st.replaying) {
        // Va detto forte: guardando i numeri senza questa riga si crederebbe di
        // stare leggendo la fascia.
        line(L"RIPRODUZIONE da file (fascia non in uso)", kAccent, 15.0f);
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

    const wchar_t* gate = !st.contactOk ? L"CONTATTO" : (st.artifact ? L"ARTEFATTO" : L"OK");
    line(std::wstring(L"Segnale: ") + gate,
         !st.contactOk ? kBad : (st.artifact ? kWarn : kOk));

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

    // --- misuratore di attivazione ---
    // Dice PERCHE' non si attiva, che è la cosa che i numeri da soli non dicono:
    // gate basso = la banda locale non lascia passare (serve piu' tolleranza o
    // una spinta piu' decisa); ampiezza bassa = si e' vicini al neutro (serve
    // concentrarsi di piu'). Le due correzioni sono opposte.
    if (st.calibValid && st.absMax > st.absMin) {
        const float bw = 460.0f, bh = 22.0f, by = y;
        const auto toX = [&](double v) {
            const double f = (v - st.absMin) / (st.absMax - st.absMin);
            return x + static_cast<float>(std::clamp(f, 0.0, 1.0)) * bw;
        };

        r.fillRect(D2D1::RectF(x, by, x + bw, by + bh), {1, 1, 1, 0.06f}, 4.0f);

        // Banda locale: dentro questa il controllo era fermo del tutto, prima.
        r.fillRect(D2D1::RectF(toX(st.localMin), by, toX(st.localMax), by + bh),
                   {1, 1, 1, 0.07f}, 4.0f);

        // Rampe di tolleranza: il bordo morbido su cui si guadagna autorita'.
        const double tolUp = t.localTolerance * (st.absMax - st.neutral);
        const double tolDn = t.localTolerance * (st.neutral - st.absMin);
        r.fillRect(D2D1::RectF(toX(st.localMax - tolUp), by, toX(st.localMax), by + bh),
                   {kOk.r, kOk.g, kOk.b, 0.24f}, 4.0f);
        r.fillRect(D2D1::RectF(toX(st.localMin), by, toX(st.localMin + tolDn), by + bh),
                   {kWarn.r, kWarn.g, kWarn.b, 0.24f}, 4.0f);

        const float nx = toX(st.neutral);
        r.fillRect(D2D1::RectF(nx - 1.0f, by, nx + 1.0f, by + bh), kMuted);

        const float px = toX(st.smoothedIndex);
        r.fillRect(D2D1::RectF(px - 2.0f, by - 3.0f, px + 2.0f, by + bh + 3.0f), kAccent, 2.0f);

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

    // Distingue "nessuna notifica" da "notifiche con formato inatteso".
    {
        const std::wstring counts = L"Pacchetti: " + std::to_wstring(st.rawPackets) +
                                    L" grezzi / " + std::to_wstring(st.validPackets) + L" validi";
        render::Color c = kMuted;
        if (st.bleState == 3 && st.rawPackets == 0)                 c = kBad;
        else if (st.rawPackets > 0 && st.validPackets == 0)         c = kWarn;
        line(counts, c);
    }

    if (cf) {
        line(L"Ingrandimento: " + std::to_wstring(cf->magnification) + L"x", kAccent, 15.0f);
    }

    // Cronologia del BLE: quando la fascia cade, dice il motivo.
    const auto logLines = g.bleLogSnapshot();
    if (!logLines.empty()) {
        y += 8.0f;
        line(L"Bluetooth:", {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
        for (const auto& l : logLines) line(L"  " + l, kMuted, 12.0f);
    }

    // --- registrazione in corso ---
    if (st.recording) {
        const double secs = static_cast<double>(st.recordedSamples) / config::kSampleRate;
        line(L"Registrazione: " + fileNameOf(g.recorder.path()) + L"  (" +
                 fixed(secs, 0) + L"s)", kOk, 12.0f);
    }

    // --- taratura corrente ---
    y += 8.0f;
    line(L"Sensibilita': " + fixed(t.sensitivity, 1) + L"x    Tolleranza: " +
             fixed(t.localTolerance, 2) + L"    Smoothing: " + fixed(t.velTauS, 2) + L"s",
         kAccent, 12.0f);
    if (!t.holdEnabled) line(L"Hold/detent SPENTO (diagnostica)", kWarn, 12.0f);

    y += 6.0f;
    line(L"su/giu sensibilita'   sin/des tolleranza   S smoothing   L hold   R reset",
         {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
    line(L"H nasconde questo pannello   -   ESC esce", {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
}

// ---------------------------------------------------------------------------
// Finestra
// ---------------------------------------------------------------------------

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            g.running.store(false, std::memory_order_release);
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE:
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case VK_RETURN: {
                    const auto st = g.state.read();
                    const auto stage = static_cast<control::CalibStage>(st.calibStage);
                    if (stage == control::CalibStage::Intro) {
                        g.command.store(static_cast<int>(app::Command::StartCalibration),
                                        std::memory_order_release);
                    } else if (stage == control::CalibStage::Failed) {
                        g.command.store(static_cast<int>(app::Command::RetryCalibration),
                                        std::memory_order_release);
                    }
                    return 0;
                }
                case 'H':
                    g.hudVisible.store(!g.hudVisible.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
                    return 0;

                // --- taratura dal vivo ---
                // La legge di controllo si giudica solo con la fascia in testa, e
                // ricompilare fra un tentativo e l'altro rende il confronto
                // inutile: quando riprovi non ricordi più com'era.
                case VK_UP:    g.tune.adjustSensitivity(+1); g.publishTunables(); return 0;
                case VK_DOWN:  g.tune.adjustSensitivity(-1); g.publishTunables(); return 0;
                case VK_RIGHT: g.tune.adjustTolerance(+1);   g.publishTunables(); return 0;
                case VK_LEFT:  g.tune.adjustTolerance(-1);   g.publishTunables(); return 0;
                case 'S':
                    g.tune.adjustSmoothing((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : +1);
                    g.publishTunables();
                    return 0;
                case 'L':
                    g.tune.holdEnabled = !g.tune.holdEnabled;
                    g.publishTunables();
                    return 0;
                case 'R':
                    g.tune.reset();
                    g.publishTunables();
                    return 0;

                default:
                    break;
            }
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/** Opzioni da riga di comando. */
struct Options {
    bool         selfTest = false;
    bool         record   = true;    // la registrazione è il default: serve dopo,
                                     // e chiederla ogni volta significa non averla
                                     // proprio la volta in cui servirebbe
    std::wstring replayPath;
    bool         badArg   = false;
    std::wstring badArgText;
};

Options parseOptions(PWSTR cmdLine) {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"", &argc);
    if (!argv) return o;

    for (int i = 0; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--selftest")            o.selfTest = true;
        else if (a == L"--senza-log")      o.record = false;
        else if (a == L"--riproduci" && i + 1 < argc) o.replayPath = argv[++i];
        else if (!a.empty() && a[0] == L'-') {
            o.badArg = true;
            o.badArgText = a;
        }
    }

    LocalFree(argv);

    // Riproducendo si legge un file: registrarlo di nuovo produrrebbe solo una
    // copia della stessa cosa.
    if (!o.replayPath.empty()) o.record = false;
    return o;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    // --selftest: verifica l'intera catena grafica (Direct2D, WIC, DirectWrite)
    // su una finestra mai mostrata, e riporta l'esito col codice d'uscita.
    // Serve a validare il rendering senza aprire nulla sullo schermo.
    const Options opt = parseOptions(cmdLine);
    const bool selfTest = opt.selfTest;

    if (opt.badArg && !selfTest) {
        MessageBoxW(nullptr,
                    (L"Opzione non riconosciuta: " + opt.badArgText + L"\n\n"
                     L"Opzioni disponibili:\n"
                     L"  --riproduci FILE.mzr   rigioca una sessione registrata,\n"
                     L"                         senza usare la fascia\n"
                     L"  --senza-log            non registrare questa sessione\n\n"
                     L"Senza opzioni il programma registra da solo in registrazioni\\.")
                        .c_str(),
                    L"Mind Zoom", MB_ICONINFORMATION);
        return 1;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MindZoomWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Mind Zoom",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    render::Renderer renderer;
    if (!renderer.init(hwnd)) {
        if (!selfTest) {
            MessageBoxW(hwnd, L"Inizializzazione di Direct2D fallita.", L"Mind Zoom", MB_ICONERROR);
        }
        return 2;
    }

    const std::wstring assets = exeDirectory() + L"\\assets";
    if (!renderer.loadSprites(assets, config::kTotalImages)) {
        if (!selfTest) {
            MessageBoxW(hwnd,
                        (L"Immagini non caricate da:\n" + assets +
                         L"\n\nServono 1..12 in .webp oppure .png.\n\n"
                         L"Se i file ci sono, probabilmente manca il codec WebP: "
                         L"installa \"Estensioni immagini WebP\" dal Microsoft Store "
                         L"oppure affianca gli stessi file convertiti in .png.").c_str(),
                        L"Mind Zoom", MB_ICONERROR);
        }
        return 3;
    }

    if (selfTest) {
        // Un frame completo: sprite in crossfade, scheda di calibrazione e HUD,
        // cioè ogni percorso di disegno dell'applicazione.
        control::ZoomController probe;
        app::ControlState st;
        st.calibStage = static_cast<int>(control::CalibStage::Concentrate);
        st.calibRemaining = 12.0;

        renderer.begin(kBg);
        const auto cf = probe.crossfade();
        renderer.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                            static_cast<float>(cf.activeAlpha));
        renderer.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                            static_cast<float>(cf.nextAlpha));
        drawCalibrationCard(renderer, st, 0.6);
        drawHud(renderer, st, probe, &cf, control::Tunables{});
        const bool drawn = renderer.end();

        renderer.shutdown();
        DestroyWindow(hwnd);
        CoUninitialize();
        return drawn ? 0 : 4;
    }

    ShowWindow(hwnd, showCmd);

    // --- sorgente del segnale: la fascia, oppure una registrazione ---
    std::vector<ble::Sample> replaySamples;
    if (!opt.replayPath.empty()) {
        auto loaded = ble::loadRecording(opt.replayPath);
        if (!loaded.ok) {
            MessageBoxW(hwnd,
                        (L"Impossibile riprodurre:\n" + opt.replayPath + L"\n\n" +
                         std::wstring(loaded.error.begin(), loaded.error.end()))
                            .c_str(),
                        L"Mind Zoom", MB_ICONERROR);
            return 5;
        }
        replaySamples = std::move(loaded.samples);
        g.replaying   = true;
        g.replayPath  = opt.replayPath;
        g.pushBleLog("riproduzione di " +
                     std::to_string(static_cast<int>(replaySamples.size() /
                                                     config::kSampleRate)) +
                     "s registrati");
    }

    if (opt.record) {
        const auto path = newRecordingPath();
        if (g.recorder.open(path)) {
            g.pushBleLog("registrazione avviata");
        } else {
            // Non si blocca l'esperienza per un log: si dice e si va avanti.
            g.pushBleLog("registrazione NON avviata: cartella non scrivibile");
        }
    }

    // Thread 1: la fascia, oppure il rigioco della registrazione. Entrambi
    // scrivono nello stesso ring, quindi il resto del programma è identico.
    std::thread replay;
    if (g.replaying) {
        replay = std::thread(replayThread, std::move(replaySamples));
    } else {
        g_muse.onSample([](const ble::Sample& s) {
            if (!g.ring.push(s)) g.dropped.fetch_add(1, std::memory_order_relaxed);
        });
        g_muse.onLog([](const std::string& msg) { g.pushBleLog(msg); });
        g_muse.start();
    }

    // Thread 2: DSP.
    std::thread dsp(dspThread);

    control::ZoomController zoom;
    double squarePos = 0.5;   // posizione filtrata del quadratino (EMA a frame rate)
    util::Ema velRender;      // interpola la velocità dai 5.3 Hz del DSP al vsync

    g.publishTunables();      // il DSP deve trovare i default già pubblicati

    auto last = std::chrono::steady_clock::now();
    MSG msg{};

    while (g.running.load(std::memory_order_acquire)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g.running.store(false, std::memory_order_release);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.running.load(std::memory_order_acquire)) break;

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        renderer.resize(static_cast<UINT>(rc.right - rc.left),
                        static_cast<UINT>(rc.bottom - rc.top));

        const auto st = g.state.read();
        const auto phase = static_cast<control::Phase>(st.phase);

        // Interpolazione 5.3 Hz -> vsync. Il DSP pubblica un valore nuovo ogni
        // 187 ms: applicandolo tale e quale si sente uno scalino ad ogni
        // aggiornamento. Questo è il punto che toglie la maggior parte della
        // ruvidità percepita.
        const double velocity = velRender.push(st.velocity, dt, config::kVelRenderTauS);

        // Rate control a frame rate: è qui che i 5.3 Hz del controllo diventano
        // movimento fluido, esattamente come nel ticker della versione web.
        zoom.update(velocity, dt, phase, st.phaseElapsed, g.tune);

        // EMA di posizione del quadratino: il target arriva a 5.3 Hz, il
        // movimento deve essere a 60.
        squarePos += (st.calibDisplayTarget - squarePos) * config::kCalibDisplayEma;

        renderer.begin(kBg);

        const auto cf = zoom.crossfade();
        renderer.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                            static_cast<float>(cf.activeAlpha));
        renderer.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                            static_cast<float>(cf.nextAlpha));

        if (phase == control::Phase::Onboarding) {
            drawCalibrationCard(renderer, st, squarePos);
        }

        if (g.hudVisible.load(std::memory_order_relaxed)) {
            drawHud(renderer, st, zoom, &cf, g.tune);
        }

        renderer.end();
    }

    g.running.store(false, std::memory_order_release);
    // Il DSP va fermato PRIMA di chiudere il file: è lui che ci scrive dentro.
    if (dsp.joinable()) dsp.join();
    if (replay.joinable()) replay.join();
    g_muse.stop();

    // A sessione finita si dice dove sono finiti i dati e come rigiocarli:
    // scritto in una cartella e mai nominato, il log non lo userebbe nessuno.
    const bool saved = g.recorder.active() && g.recorder.count() > 0;
    const std::wstring path = g.recorder.path();
    const double secs = g.recorder.seconds();
    g.recorder.close();

    renderer.shutdown();
    CoUninitialize();

    if (saved) {
        wchar_t report[1024]{};
        swprintf(report, 1024,
                 L"Sessione registrata: %.0f secondi.\n\n%s\n\n"
                 L"Per rivedere questa sessione senza indossare la fascia:\n"
                 L"  MindZoom.exe --riproduci \"%s\"\n\n"
                 L"Lo stesso file si analizza con:\n"
                 L"  mz_probe.exe --replay \"%s\"",
                 secs, path.c_str(), path.c_str(), path.c_str());
        MessageBoxW(nullptr, report, L"Mind Zoom", MB_ICONINFORMATION);
    }
    return 0;
}
