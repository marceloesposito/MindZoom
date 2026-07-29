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
#include "config.hpp"
#include "control/calibration.hpp"
#include "control/zoom.hpp"
#include "dsp/gating.hpp"
#include "dsp/stft.hpp"
#include "render/renderer.hpp"
#include "util/double_buffer.hpp"
#include "util/spsc_ring.hpp"

#include <windows.h>
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

    std::atomic<int>           command{static_cast<int>(app::Command::None)};
    std::atomic<bool>          running{true};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool>          hudVisible{true};

    // Ultimi messaggi del BLE, mostrati nel pannello diagnostico: senza, quando
    // la fascia cade non si capisce il motivo. Frequenza bassissima, quindi un
    // mutex va benissimo e non tocca il percorso caldo.
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
    double velocity     = 0.0;
    int    gatedWindows = 0;
    bool   contactOk    = false;   // finché non arriva un frame non si sa

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
            velocity = 0.0;
        }

        ble::Sample s;
        bool consumed = false;

        // --- consumo dei campioni: tutto ciò che dipende dal SEGNALE ---
        while (g.ring.pop(s)) {
            consumed = true;
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
                velocity = 0.0;
                ++gatedWindows;
            } else if (quality.artifact) {
                // La finestra sporca non alimenta l'indice. Si tiene la velocità
                // precedente per non introdurre uno scatto ad ogni blink, ma un
                // gating prolungato non deve incollare lo zoom.
                ++gatedWindows;
                if (gatedWindows > config::kArtifactHoldMaxS * config::kControlHz) {
                    velocity *= 0.85;
                }
            } else if (index) {
                gatedWindows = 0;
                const double c = smoother.push(*index);
                st.smoothedIndex = c;

                if (phase == control::Phase::Onboarding) {
                    calib.sample(c);
                } else {
                    velocity = calib.velocity(c, dt);
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
        const auto silent = g_muse.millisSinceLastPacket();
        const bool stalled = g_muse.streaming() && silent > 0 &&
                             silent > static_cast<std::int64_t>(config::kEegWatchdogS * 1000);
        if (stalled) {
            velocity = 0.0;
            g_muse.resumeStreaming();
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
        st.velocity = velocity;
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
        st.droppedSamples = g.dropped.load(std::memory_order_relaxed);
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
             const control::CrossfadeState* cf) {
    const float x = 24.0f;
    float y = 20.0f;
    const auto line = [&](const std::wstring& s, render::Color c, float size = 14.0f) {
        r.drawText(s, D2D1::RectF(x, y, x + 460, y + size + 8), size, c, render::TextAlign::Left);
        y += size + 7.0f;
    };

    const wchar_t* bleNames[] = {L"DISCONNESSO", L"RICERCA", L"CONNESSIONE", L"STREAMING"};
    const int bs = std::clamp(st.bleState, 0, 3);
    line(std::wstring(L"Muse: ") + bleNames[bs],
         bs == 3 ? kOk : (bs == 0 ? kBad : kWarn), 15.0f);

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

    line(L"Velocita': " + fixed(st.velocity, 3), st.velocity > 0 ? kOk
                                               : (st.velocity < 0 ? kWarn : kMuted));
    line(L"Focus: " + fixed(zoom.targetFocus(), 3) + L" -> " + fixed(zoom.currentFocus(), 3), kMuted);
    line(L"Bande  t:" + fixed(st.theta, 1) + L"  a:" + fixed(st.alpha, 1) + L"  b:" +
             fixed(st.beta, 1), kMuted);
    line(L"Ampiezza: " + fixed(st.maxAbsRaw, 0) + L" uV   pkt: " +
             std::to_wstring(st.packetLen) + L"B/" + std::to_wstring(st.packetSamples) + L"smp",
         kMuted);

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

    y += 6.0f;
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
                default:
                    break;
            }
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    // --selftest: verifica l'intera catena grafica (Direct2D, WIC, DirectWrite)
    // su una finestra mai mostrata, e riporta l'esito col codice d'uscita.
    // Serve a validare il rendering senza aprire nulla sullo schermo.
    const bool selfTest = (cmdLine && wcsstr(cmdLine, L"--selftest") != nullptr);

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
        drawHud(renderer, st, probe, &cf);
        const bool drawn = renderer.end();

        renderer.shutdown();
        DestroyWindow(hwnd);
        CoUninitialize();
        return drawn ? 0 : 4;
    }

    ShowWindow(hwnd, showCmd);

    // Thread 1: BLE.
    g_muse.onSample([](const ble::Sample& s) {
        if (!g.ring.push(s)) g.dropped.fetch_add(1, std::memory_order_relaxed);
    });
    g_muse.onLog([](const std::string& msg) { g.pushBleLog(msg); });
    g_muse.start();

    // Thread 2: DSP.
    std::thread dsp(dspThread);

    control::ZoomController zoom;
    double squarePos = 0.5;   // posizione filtrata del quadratino (EMA a frame rate)

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

        // Rate control a frame rate: è qui che i 5.3 Hz del controllo diventano
        // movimento fluido, esattamente come nel ticker della versione web.
        zoom.update(st.velocity, dt, phase, st.phaseElapsed);

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
            drawHud(renderer, st, zoom, &cf);
        }

        renderer.end();
    }

    g.running.store(false, std::memory_order_release);
    if (dsp.joinable()) dsp.join();
    g_muse.stop();
    renderer.shutdown();
    CoUninitialize();
    return 0;
}
