// Mind Zoom - applicazione nativa Windows.
//
// Tre thread, come da architettura del brief:
//   1. BLE   - dentro MuseClient: GATT WinRT, decodifica, push su ring lock-free
//   2. DSP   - consuma il ring: STFT, Pope, gating, calibrazione, velocità
//   3. Render (questo, main) - vsync, rate control, crossfade, HUD
//
// La comunicazione è senza lock: ring SPSC verso il DSP, double buffer atomico
// verso il render. Il thread di render non si blocca mai in attesa del segnale.

#include "app/displays.hpp"
#include "app/shared_state.hpp"
#include "app/telemetry.hpp"
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
#include <commdlg.h>    // GetOpenFileNameW: scelta della registrazione
#include <shellapi.h>   // CommandLineToArgvW
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
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
constexpr render::Color kGrid      {1.0f, 1.0f, 1.0f, 0.10f};
constexpr render::Color kPlotBg    {0.04f, 0.05f, 0.07f, 0.88f};

const app::PlotTheme kPlotTheme{kInk, kMuted, kAccent, kOk, kWarn, kBad, kGrid, kPlotBg};

/**
 * Una notifica Bluetooth così com'è arrivata. Dimensione fissa per non allocare
 * nel callback GATT, che non deve mai bloccarsi: i pacchetti del Muse sono di
 * una ventina di byte, questo margine li copre tutti.
 */
struct RawPacket {
    std::uint16_t len = 0;
    std::uint8_t  data[256]{};
};

struct Shared {
    util::SpscRing<ble::Sample, 16384> ring;
    // I grezzi passano da un secondo ring: così a scrivere sul file resta un
    // thread solo, quello del DSP, e non serve nessun lock.
    util::SpscRing<RawPacket, 2048> rawRing;
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

    // --- riproduzione da file invece che dalla fascia ---
    // Si può accendere a programma aperto (tasto P), quindi lo stato è atomico e
    // il thread ha un interruttore di uscita proprio: il precedente va fermato
    // prima di far partire il successivo, o due sorgenti scriverebbero nello
    // stesso ring e il segnale risulterebbe un miscuglio delle due.
    std::atomic<bool> replaying{false};
    std::atomic<bool> replayCancel{false};
    std::thread       replayWorker;
    std::wstring      replayName;      // solo il nome del file, per il pannello
    std::atomic<bool> resetHistory{false};

    // --- schermi ---
    // Con un monitor solo si resta a finestra unica, immagini con pannello
    // sovrapposto: e' il comportamento di sempre e non deve mai rompersi.
    std::vector<app::Display> displays;
    HWND                 opHwnd   = nullptr;   // operatore: telemetria
    HWND                 projHwnd = nullptr;   // proiezione: solo l'immagine
    int                  projIndex = -1;       // indice in displays
    std::atomic<bool>    choosing{false};      // schermata di scelta in corso
    std::atomic<int>     candidate{-1};        // schermo evidenziato durante la scelta

    bool dualScreen() const noexcept { return projHwnd != nullptr; }

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

/** Sposta la finestra di proiezione sullo schermo indicato, a tutto schermo. */
void placeProjection(int index) {
    if (!g.projHwnd || index < 0 || index >= static_cast<int>(g.displays.size())) return;

    const app::ScreenRect& b = g.displays[static_cast<std::size_t>(index)].bounds;
    SetWindowPos(g.projHwnd, HWND_TOPMOST, b.left, b.top,
                 b.right - b.left, b.bottom - b.top, SWP_SHOWWINDOW);
    g.candidate.store(index, std::memory_order_relaxed);
}

/** Conferma lo schermo di proiezione e ricorda la scelta per la prossima volta. */
void confirmProjection(int index) {
    if (index < 0 || index >= static_cast<int>(g.displays.size())) return;

    g.projIndex = index;
    g.choosing.store(false, std::memory_order_release);
    placeProjection(index);
    app::saveDisplayChoice(g.displays[static_cast<std::size_t>(index)].deviceName,
                      app::layoutSignature(g.displays));

    // La finestra dell'operatore deve tornare davanti: durante la scelta era la
    // proiezione ad avere il fuoco, e i tasti servono qui.
    if (g.opHwnd) SetForegroundWindow(g.opHwnd);
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

    while (g.running.load(std::memory_order_acquire) &&
           !g.replayCancel.load(std::memory_order_acquire) &&
           sent < samples.size()) {
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

/** Ferma la riproduzione in corso, se c'è, e aspetta che il thread esca. */
void stopReplay() {
    g.replayCancel.store(true, std::memory_order_release);
    if (g.replayWorker.joinable()) g.replayWorker.join();
    g.replayCancel.store(false, std::memory_order_release);
    g.replaying.store(false, std::memory_order_release);
}

/**
 * Passa a riprodurre `path`. Ferma la fascia e l'eventuale riproduzione
 * precedente, poi riavvia l'esperienza da capo: gli estremi calibrati su
 * un'altra sessione non descrivono questa.
 * @return messaggio d'errore, vuoto se è andata.
 */
std::string startReplay(const std::wstring& path) {
    auto loaded = ble::loadRecording(path);
    if (!loaded.ok) return loaded.error;

    stopReplay();

    // La fascia non deve piu' scrivere nel ring: due sorgenti insieme darebbero
    // un segnale che non e' ne' l'uno ne' l'altro.
    g_muse.stop();

    // Via i campioni ancora in coda dalla sorgente precedente.
    g.ring.clear();

    g.replayName = fileNameOf(path);
    g.replaying.store(true, std::memory_order_release);
    g.resetHistory.store(true, std::memory_order_release);
    g.command.store(static_cast<int>(app::Command::RestartSession), std::memory_order_release);

    g.pushBleLog("riproduzione: " + std::to_string(static_cast<int>(loaded.seconds())) + "s");

    g.replayWorker = std::thread(replayThread, std::move(loaded.samples));
    return {};
}

/**
 * Chiede quale registrazione riprodurre.
 *
 * La finestra di proiezione è topmost: va abbassata mentre il dialogo è aperto,
 * o il dialogo può finirci sotto e sembrare che il programma si sia piantato.
 */
void chooseReplayFile() {
    const bool wasTopmost = (g.projHwnd != nullptr);
    if (wasTopmost) {
        SetWindowPos(g.projHwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    wchar_t file[MAX_PATH]{};
    const std::wstring initialDir = exeDirectory() + L"\\registrazioni";

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = g.opHwnd;
    ofn.lpstrFilter     = L"Registrazioni Mind Zoom (*.mzr)\0*.mzr\0Tutti i file\0*.*\0";
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.lpstrTitle      = L"Quale sessione registrata vuoi riprodurre?";
    ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    const bool picked = GetOpenFileNameW(&ofn) != FALSE;

    if (wasTopmost && g.projHwnd) {
        SetWindowPos(g.projHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (!picked) return;

    const std::string err = startReplay(file);
    if (!err.empty()) {
        MessageBoxW(g.opHwnd,
                    (L"Impossibile riprodurre:\n" + std::wstring(file) + L"\n\n" +
                     std::wstring(err.begin(), err.end())).c_str(),
                    L"Mind Zoom", MB_ICONERROR);
    }
    if (g.opHwnd) SetForegroundWindow(g.opHwnd);
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
    bool   signalPlausible = false;
    double flushTimer      = 0.0;

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
        } else if (cmd == app::Command::RestartSession) {
            // Cambiata la sorgente del segnale: si riparte dall'inizio, perché
            // gli estremi misurati su un'altra sessione non descrivono questa.
            calib = control::Calibration{};
            stft  = dsp::SlidingStft{};
            gating.reset();
            smoother.reset();
            velocityRaw = 0.0;
            velocitySmooth.reset();
            phase        = control::Phase::Onboarding;
            phaseElapsed = 0.0;
            doneHold     = 0.0;
            gatedWindows = 0;
            contactOk    = false;
        }

        const control::Tunables tune = g.tunables.read();

        // I pacchetti grezzi si travasano sul file qui, dove c'è già l'unico
        // scrittore. Vanno registrati sempre: se un giorno il decodificatore
        // risultasse sbagliato, questi sono l'unica cosa che permette di
        // ricostruire la sessione invece di buttarla.
        RawPacket raw;
        while (g.rawRing.pop(raw)) {
            g.recorder.writeRaw(raw.data, raw.len);
        }

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
                    // Il campione conta solo se il segnale vale: la calibrazione
                    // ora si misura in campioni raccolti, non in secondi passati,
                    // quindi ammettere un campione cattivo significherebbe
                    // avvicinare la fine con dell'informazione falsa.
                    calib.sample(c, quality.contactOk &&
                                    quality.fault == dsp::SignalFault::None);
                } else {
                    velocityRaw = calib.velocity(c, dt, tune);
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
            // E deve essere un segnale PLAUSIBILE: su rumore la calibrazione
            // riesce lo stesso, perché le sue escursioni casuali superano
            // abbondantemente la soglia di modulazione minima. È successo.
            calib.tick(elapsed, signalFresh && contactOk && signalPlausible);

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
        const bool replaying = g.replaying.load(std::memory_order_acquire);
        const auto silent = g_muse.millisSinceLastPacket();
        const bool stalled = !replaying && g_muse.streaming() && silent > 0 &&
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
        st.calibShowTarget    = calib.showTarget();
        st.calibProgress      = calib.progress();
        st.calibEffN          = calib.effectiveSamples();
        st.calibSeparation    = calib.separation();
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
        } else if (st.frames > 0 && !signalPlausible) {
            // Non è un fallimento della calibrazione: è che non si può nemmeno
            // cominciare. Va detto subito, non dopo trenta secondi.
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
        // Il file va portato su disco spesso: la chiusura può richiedere
        // secondi e chi si stanca termina il processo. Due secondi di margine
        // invece dell'intera sessione.
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

void drawCalibrationCard(render::Renderer& r, const app::ControlState& st, double squarePos) {
    const auto win = r.size();
    const float cx = win.width * 0.5f;

    // Velo scuro: la scheda deve staccare dallo sfondo.
    r.fillRect(render::rect(0, 0, win.width, win.height), {0.0f, 0.0f, 0.0f, 0.72f});

    const auto stage = static_cast<control::CalibStage>(st.calibStage);

    const float panelW = 520.0f;
    const float panelH = 560.0f;
    const render::Rect panel = render::rect(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    r.fillRect(panel, kPanel, 18.0f);
    r.drawRectOutline(panel, {1, 1, 1, 0.10f}, 1.0f, 18.0f);

    const float top = panel.top + 36.0f;

    if (stage == control::CalibStage::Intro) {
        r.drawText(L"Calibrazione", render::rect(panel.left, top, panel.right, top + 50), 32.0f,
                   kInk, render::TextAlign::Center, true);
        r.drawText(L"Due fasi: prima ti concentri per spingere il quadratino verso\n"
                   L"l'obiettivo in alto, poi lasci andare e ti rilassi.\n\n"
                   L"Non hanno una durata fissa: finiscono quando il programma ha\n"
                   L"raccolto abbastanza misure per distinguere i tuoi due stati.\n"
                   L"La barra dice quanto manca. Se il segnale peggiora si ferma,\n"
                   L"perche' quei momenti non contano.",
                   render::rect(panel.left + 40, top + 70, panel.right - 40, panel.bottom - 120),
                   17.0f, kMuted);
        r.drawText(L"INVIO per iniziare",
                   render::rect(panel.left, panel.bottom - 100, panel.right, panel.bottom - 62),
                   19.0f, kAccent, render::TextAlign::Center, true);
        r.drawText(st.replaying ? L"P per cambiare registrazione"
                                : L"P per usare una sessione registrata, senza fascia",
                   render::rect(panel.left, panel.bottom - 58, panel.right, panel.bottom - 34),
                   14.0f, kMuted, render::TextAlign::Center);
        if (!st.signalFresh && !st.replaying) {
            r.drawText(L"Nessun dato dalla fascia: puoi iniziare lo stesso,\n"
                       L"il conteggio partira' quando arriva il segnale.",
                       render::rect(panel.left + 24, panel.bottom - 150, panel.right - 24,
                                   panel.bottom - 105),
                       14.0f, kWarn);
        }
        return;
    }

    if (stage == control::CalibStage::Done) {
        r.drawText(L"Calibrazione completata",
                   render::rect(panel.left, win.height * 0.5f - 60, panel.right,
                               win.height * 0.5f - 10),
                   30.0f, kOk, render::TextAlign::Center, true);
        r.drawText(L"Concentrandoti aumenterai lo zoom, rilassandoti tornerai indietro.\n"
                   L"Buona esplorazione.",
                   render::rect(panel.left + 40, win.height * 0.5f + 10, panel.right - 40,
                               win.height * 0.5f + 100),
                   17.0f, kMuted);
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

        r.drawText(L"Calibrazione non riuscita",
                   render::rect(panel.left, win.height * 0.5f - 80, panel.right,
                               win.height * 0.5f - 30),
                   28.0f, kBad, render::TextAlign::Center, true);
        r.drawText(detail,
                   render::rect(panel.left + 40, win.height * 0.5f - 10, panel.right - 40,
                               win.height * 0.5f + 90),
                   17.0f, kMuted);
        r.drawText(L"INVIO per riprovare",
                   render::rect(panel.left, panel.bottom - 80, panel.right, panel.bottom - 40),
                   19.0f, kAccent, render::TextAlign::Center, true);
        return;
    }

    // --- fasi attive ---
    const bool concentrate = (stage == control::CalibStage::Concentrate);

    r.drawText(concentrate ? L"Concentrazione" : L"Rilassamento",
               render::rect(panel.left, top, panel.right, top + 44), 30.0f, kInk,
               render::TextAlign::Center, true);
    r.drawText(concentrate
                   ? L"Concentrati per spingere il quadratino verso l'obiettivo in alto.\n"
                     L"Funziona meglio con un compito mentale vero: contare all'indietro\n"
                     L"da 300 saltando di 7."
                   : L"Ora lascia andare. Respira lentamente, sguardo morbido,\n"
                     L"mascella rilassata. Non c'e' niente da guardare e niente da\n"
                     L"raggiungere: la barra si riempie da sola.",
               render::rect(panel.left + 32, top + 52, panel.right - 32, top + 140), 17.0f, kMuted);

    const float railTop = panel.top + 175.0f;

    // Il quadratino esiste solo in concentrazione. Nel rilassamento un
    // indicatore che si muove darebbe un compito, e sorvegliare il proprio
    // rilassamento e' esso stesso attivita' attenzionale: si misurerebbe peggio
    // proprio cio' che si vuole misurare.
    if (st.calibShowTarget) {
        const render::Rect rail = render::rect(cx - kRailW / 2, railTop, cx + kRailW / 2,
                                             railTop + kRailH);
        r.fillRect(rail, kRailBg, 12.0f);

        const render::Rect target =
            render::rect(rail.left, rail.top, rail.right, rail.top + kTargetH);
        r.fillRect(target, kTargetZone, 10.0f);
        r.drawRectOutline(target, {kOk.r, kOk.g, kOk.b, 0.55f}, 1.5f, 10.0f);

        const float travel = kRailH - kSquareH;
        const float sy = rail.bottom - kSquareH - static_cast<float>(squarePos) * travel;
        const render::Rect square =
            render::rect(cx - kSquareH / 2, sy, cx + kSquareH / 2, sy + kSquareH);

        render::Color squareColor = kAccent;
        if (!st.contactOk)        squareColor = kMuted;
        else if (squarePos > 0.8) squareColor = kOk;
        r.fillRect(square, squareColor, 8.0f);
    }

    // --- barra di avanzamento ---
    // Sostituisce il countdown, che mentiva: i secondi scorrevano anche quando
    // non stava entrando nessuna informazione utile. Questa si riempie con i
    // campioni indipendenti raccolti, quindi si ferma davvero quando il segnale
    // non vale ed e' onesta su quanto manca.
    const float barY = st.calibShowTarget ? (railTop + kRailH + 34.0f)
                                          : (panel.top + 210.0f);
    const float barW = panelW - 96.0f;
    const render::Rect barBg =
        render::rect(cx - barW / 2, barY, cx + barW / 2, barY + 22.0f);
    r.fillRect(barBg, kRailBg, 11.0f);

    const auto p = static_cast<float>(std::clamp(st.calibProgress, 0.0, 1.0));
    if (p > 0.001f) {
        r.fillRect(render::rect(barBg.left, barBg.top, barBg.left + barW * p, barBg.bottom),
                   p >= 0.999f ? kOk : kAccent, 11.0f);
    }
    r.drawRectOutline(barBg, {1, 1, 1, 0.12f}, 1.0f, 11.0f);

    r.drawText(std::to_wstring(static_cast<int>(p * 100.0f + 0.5f)) + L"%",
               render::rect(panel.left, barBg.bottom + 10, panel.right, barBg.bottom + 44),
               22.0f, kInk, render::TextAlign::Center, true);

    // Il conteggio si ferma sia senza fascia sia con contatto scarso: sono due
    // situazioni diverse e vanno dette in modo diverso, altrimenti sembra che il
    // programma sia bloccato.
    if (!st.signalFresh) {
        r.drawText(L"In attesa del segnale dalla fascia. La barra non avanza.",
                   render::rect(panel.left + 24, panel.bottom - 60, panel.right - 24,
                               panel.bottom - 20),
                   15.0f, kWarn);
    } else if (st.signalFault != 0) {
        r.drawText(L"Il segnale non e' utilizzabile: questi campioni non contano.",
                   render::rect(panel.left + 24, panel.bottom - 60, panel.right - 24,
                               panel.bottom - 20),
                   15.0f, kBad);
    } else if (!st.contactOk) {
        r.drawText(L"Contatto assente: sistema la fascia. Questi campioni non contano.",
                   render::rect(panel.left + 24, panel.bottom - 60, panel.right - 24,
                               panel.bottom - 20),
                   15.0f, kWarn);
    } else {
        // Con segnale buono si dice cosa si sta accumulando: senza, una barra
        // che avanza a scatti sembra rotta invece che onesta.
        std::wstring nota = L"campioni indipendenti: " +
                            std::to_wstring(static_cast<int>(st.calibEffN)) + L" / " +
                            std::to_wstring(static_cast<int>(config::kCalibTargetEffSamples));
        if (!concentrate) {
            nota += L"     separazione: " + fixed(st.calibSeparation, 1) + L" / " +
                    fixed(config::kCalibMinSeparationT, 1);
        }
        r.drawText(nota,
                   render::rect(panel.left + 24, panel.bottom - 56, panel.right - 24,
                               panel.bottom - 24),
                   14.0f, kMuted, render::TextAlign::Center);
    }
}

/**
 * Larghezza del campo inquadrato, in metri, all'ingrandimento dato.
 *
 * L'ingrandimento e' un rapporto, non una lunghezza: diventa una misura solo
 * fissando la larghezza di riferimento (vedi config::kReferenceWidthMm).
 */
double fieldWidthMeters(double magnification) {
    if (magnification <= 0.0) return 0.0;
    return (config::kReferenceWidthMm / 1000.0) / magnification;
}

/** Numero "tondo" (1, 2, 5 x 10^n) non superiore a v. */
double niceLength(double v) {
    if (v <= 0.0) return 0.0;
    const double exp10 = std::pow(10.0, std::floor(std::log10(v)));
    const double m = v / exp10;
    const double mant = (m >= 5.0) ? 5.0 : (m >= 2.0 ? 2.0 : 1.0);
    return mant * exp10;
}

/** Formatta una lunghezza in metri con l'unita' piu' leggibile. */
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
            // "1.0 um" e' rumore: se e' intero si scrive intero.
            if (dec == 1 && s.size() > 2 && s.substr(s.size() - 2) == L".0") {
                s = s.substr(0, s.size() - 2);
            }
            return s + L" " + u.nome;
        }
    }
    return fixed(meters, 1) + L" m";
}

/**
 * Sovrimpressione dello schermo di proiezione: ingrandimento in alto al centro,
 * barra di scala in basso al centro.
 *
 * E' l'unica interfaccia ammessa sul proiettore, e ci sta perche' non e' un
 * comando ne' una diagnostica: e' parte di cio' che si sta guardando. Senza un
 * riferimento di scala un'immagine al microscopio elettronico e' una texture
 * astratta - e' la scala che la rende una cosa.
 */
void drawProjectionScale(render::Renderer& r, const control::CrossfadeState& cf) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;

    // --- ingrandimento, in alto al centro ---
    // Richiesto almeno il 5% dell'altezza dello schermo: qui e' il corpo del
    // testo, non il riquadro, cosi' il vincolo vale sulla cosa che si legge.
    const float testo = std::max(24.0f, win.height * 0.055f);
    const float padX  = testo * 0.9f;
    const float padY  = testo * 0.34f;

    const std::wstring etichetta = std::to_wstring(cf.magnification) + L"×";
    const float boxW = testo * (0.62f * etichetta.size()) + padX * 2.0f;
    const float boxH = testo + padY * 2.0f;
    const float boxY = win.height * 0.035f;

    const render::Rect box =
        render::rect(cx - boxW / 2, boxY, cx + boxW / 2, boxY + boxH);
    r.fillRect(box, {0.0f, 0.0f, 0.0f, 0.55f}, boxH * 0.22f);
    r.drawRectOutline(box, {1.0f, 1.0f, 1.0f, 0.28f}, 1.5f, boxH * 0.22f);
    r.drawText(etichetta, render::rect(box.left, box.top + padY * 0.6f, box.right, box.bottom),
               testo, kInk, render::TextAlign::Center, true);

    // --- barra di scala, in basso al centro ---
    // Si sceglie una lunghezza tonda che occupi circa un quinto della larghezza:
    // una barra di "3,7 um" sarebbe esatta e illeggibile, il senso e' dare un
    // metro di paragone a colpo d'occhio.
    const double campo = fieldWidthMeters(cf.magnification);
    if (campo <= 0.0) return;

    const double perPixel = campo / win.width;          // metri per pixel a schermo
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
    r.fillRect(sfondo, {0.0f, 0.0f, 0.0f, 0.45f}, 10.0f);

    const render::Color bianco{1.0f, 1.0f, 1.0f, 0.92f};
    r.fillRect(render::rect(x0, barY, x1, barY + spess), bianco);
    r.fillRect(render::rect(x0, barY - tick / 2, x0 + spess, barY + tick), bianco);
    r.fillRect(render::rect(x1 - spess, barY - tick / 2, x1, barY + tick), bianco);

    r.drawText(formatLength(lunghezza),
               render::rect(x0 - 28.0f, barY - etichettaH - 14.0f, x1 + 28.0f, barY - 6.0f),
               etichettaH, bianco, render::TextAlign::Center, true);
}

/**
 * Sullo schermo candidato: un numero gigante col nome e la risoluzione.
 *
 * La finestra salta fisicamente da uno schermo all'altro mentre si scorre, così
 * la scelta si conferma guardando, non leggendo un elenco e sperando che
 * "DISPLAY2" sia quello giusto.
 */
void drawScreenPicker(render::Renderer& r, int candidate) {
    const auto win = r.size();
    r.fillRect(render::rect(0, 0, win.width, win.height), {0.03f, 0.05f, 0.08f, 1.0f});

    const std::wstring number =
        std::to_wstring(candidate + 1);
    r.drawText(number,
               render::rect(0, win.height * 0.5f - 190.0f, win.width, win.height * 0.5f + 60.0f),
               260.0f, kAccent, render::TextAlign::Center, true);

    std::wstring caption = L"Questo schermo";
    if (candidate >= 0 && candidate < static_cast<int>(g.displays.size())) {
        caption += L"  -  " + g.displays[static_cast<std::size_t>(candidate)].describe();
    }
    r.drawText(caption,
               render::rect(0, win.height * 0.5f + 70.0f, win.width, win.height * 0.5f + 110.0f),
               22.0f, kInk, render::TextAlign::Center);

    r.drawText(L"Frecce per cambiare schermo   -   INVIO per confermare",
               render::rect(0, win.height * 0.5f + 130.0f, win.width, win.height * 0.5f + 170.0f),
               18.0f, kMuted, render::TextAlign::Center);
}

/** Sullo schermo dell'operatore: l'elenco, con evidenziato il candidato. */
void drawScreenPickerPanel(render::Renderer& r, int candidate) {
    const auto win = r.size();
    const float cx = win.width * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), {0.0f, 0.0f, 0.0f, 0.78f});

    const float panelW = 620.0f;
    const float rowH   = 52.0f;
    const float panelH = 250.0f + rowH * static_cast<float>(g.displays.size());
    const render::Rect panel = render::rect(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    r.fillRect(panel, kPanel, 18.0f);
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

    for (std::size_t i = 0; i < g.displays.size(); ++i) {
        const bool sel = (static_cast<int>(i) == candidate);
        const render::Rect row = render::rect(panel.left + 30, y, panel.right - 30, y + rowH - 8.0f);
        if (sel) {
            r.fillRect(row, {kAccent.r, kAccent.g, kAccent.b, 0.20f}, 8.0f);
            r.drawRectOutline(row, {kAccent.r, kAccent.g, kAccent.b, 0.65f}, 1.5f, 8.0f);
        }
        r.drawText(std::to_wstring(i + 1) + L".   " + g.displays[i].describe(),
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
               render::rect(panel.left + 30, y, panel.right - 30, y + 26.0f), 13.0f,
               {0.45f, 0.48f, 0.55f, 1.0f}, render::TextAlign::Center);
}

void drawHud(render::Renderer& r, const app::ControlState& st, const control::ZoomController& zoom,
             const control::CrossfadeState* cf, const control::Tunables& t) {
    const float x = 24.0f;
    float y = 20.0f;
    const auto line = [&](const std::wstring& s, render::Color c, float size = 14.0f) {
        r.drawText(s, render::rect(x, y, x + 460, y + size + 8), size, c, render::TextAlign::Left);
        y += size + 7.0f;
    };

    if (st.replaying) {
        // Va detto forte: guardando i numeri senza questa riga si crederebbe di
        // stare leggendo la fascia.
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

    // La plausibilità viene PRIMA del gating: se quello che arriva non è un
    // segnale biologico, dire "artefatto" o "contatto" manda a cercare il
    // problema sulla testa dell'utente invece che nel programma.
    if (st.signalFault != 0) {
        // L'ordine segue dsp::SignalFault.
        const wchar_t* faults[] = {L"OK", L"RETE 50 Hz", L"SATURO", L"PIATTO",
                                   L"NON E' UN SEGNALE"};
        const int fi = std::clamp(st.signalFault, 0, 4);
        line(std::wstring(L"Segnale: ") + faults[fi], kBad, 15.0f);

        // Il rimedio e' diverso per ciascuno, quindi lo si scrive: "RETE"
        // manda a sistemare la fascia, "NON E' UN SEGNALE" manda a guardare il
        // programma, e confonderli fa perdere una sessione intera.
        if (fi == static_cast<int>(dsp::SignalFault::Mains)) {
            // Non dire "non tocca": manda a cercare un problema che non c'e'.
            // Un elettrodo secco puo' appoggiare benissimo e avere comunque
            // un'impedenza altissima - pelle asciutta, sebo, un velo di capelli -
            // e in quel caso capta la rete per accoppiamento capacitivo PUR
            // toccando. Il rimedio e' inumidire, non premere.
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

        r.fillRect(render::rect(x, by, x + bw, by + bh), {1, 1, 1, 0.06f}, 4.0f);

        // Banda locale: dentro questa il controllo era fermo del tutto, prima.
        r.fillRect(render::rect(toX(st.localMin), by, toX(st.localMax), by + bh),
                   {1, 1, 1, 0.07f}, 4.0f);

        // Rampe di tolleranza: il bordo morbido su cui si guadagna autorita'.
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
    line(L"P sessione registrata   D schermo   H pannello   ESC esce",
         {0.45f, 0.48f, 0.55f, 1.0f}, 12.0f);
}

// ---------------------------------------------------------------------------
// Finestra
// ---------------------------------------------------------------------------

/**
 * Tasti della fase di scelta dello schermo. È una modalità a sé, prima
 * dell'esperienza: qui le frecce scelgono il monitor, dopo la conferma tornano
 * a significare tolleranza e sensibilità.
 */
bool handlePickerKey(WPARAM wp) {
    const int n = static_cast<int>(g.displays.size());
    if (n <= 0) return false;

    int cur = g.candidate.load(std::memory_order_relaxed);
    if (cur < 0) cur = 0;

    switch (wp) {
        case VK_LEFT:
        case VK_UP:
            placeProjection((cur - 1 + n) % n);
            return true;
        case VK_RIGHT:
        case VK_DOWN:
            placeProjection((cur + 1) % n);
            return true;
        case VK_RETURN:
            confirmProjection(cur);
            return true;
        default:
            // Anche i tasti numerici: con più di due schermi è più rapido.
            if (wp >= '1' && wp < static_cast<WPARAM>('1' + n)) {
                confirmProjection(static_cast<int>(wp - '1'));
                return true;
            }
            return false;
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            // Solo la finestra dell'operatore chiude l'applicazione: la
            // proiezione viene distrutta insieme, non è lei a comandare.
            if (hwnd == g.opHwnd) {
                g.running.store(false, std::memory_order_release);
                PostQuitMessage(0);
            }
            return 0;

        case WM_DISPLAYCHANGE:
            // Uno schermo scollegato o aggiunto: la finestra di proiezione
            // potrebbe essere finita fuori dal desktop visibile, cioè invisibile
            // e irraggiungibile. Si riparte dalla scelta.
            if (hwnd == g.opHwnd) {
                g.displays = app::enumerateDisplays();
                if (g.projHwnd && g.displays.size() >= 2) {
                    const int match = app::matchStoredChoice(g.displays);
                    if (match >= 0) {
                        confirmProjection(match);
                    } else {
                        g.choosing.store(true, std::memory_order_release);
                        placeProjection(g.displays.size() > 1 ? 1 : 0);
                    }
                }
            }
            return 0;

        case WM_SETCURSOR:
            // Sul proiettore il puntatore non deve comparire: è nell'inquadratura
            // del partecipante.
            if (hwnd == g.projHwnd && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            // Durante la scelta i tasti hanno un altro significato. La
            // proiezione inoltra qui: da qualunque finestra si prema, funziona.
            if (g.choosing.load(std::memory_order_acquire) && handlePickerKey(wp)) return 0;

            switch (wp) {
                case VK_ESCAPE:
                    PostMessageW(g.opHwnd ? g.opHwnd : hwnd, WM_CLOSE, 0, 0);
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
                case 'P':
                    // Passa a una sessione registrata. Utile per provare la
                    // taratura, o l'installazione a due schermi, senza fascia.
                    chooseReplayFile();
                    return 0;
                case 'D':
                    // Riapre la scelta dello schermo senza riavviare.
                    if (g.displays.size() >= 2 && g.projHwnd) {
                        g.choosing.store(true, std::memory_order_release);
                        placeProjection(g.projIndex >= 0 ? g.projIndex : 1);
                    }
                    return 0;
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
            break;
    }
    // Ci si arriva anche da WM_SETCURSOR sulla finestra dell'operatore, dove il
    // puntatore deve restare quello di sistema.
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/** Opzioni da riga di comando. */
struct Options {
    bool         selfTest = false;
    bool         listScreens = false;
    bool         record   = true;    // la registrazione è il default: serve dopo,
                                     // e chiederla ogni volta significa non averla
                                     // proprio la volta in cui servirebbe
    bool         singleScreen = false;   // forza il comportamento a finestra unica
    std::wstring replayPath;
    std::wstring inspectPath;            // --controlla: verdetto su una registrazione
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
        else if (a == L"--schermi")        o.listScreens = true;
        else if (a == L"--senza-log")      o.record = false;
        else if (a == L"--schermo-singolo") o.singleScreen = true;
        else if (a == L"--riproduci" && i + 1 < argc) o.replayPath = argv[++i];
        else if (a == L"--controlla" && i + 1 < argc) o.inspectPath = argv[++i];
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

/**
 * Scrive sulla console che ha lanciato il programma, se c'è.
 *
 * L'applicazione è del sottosistema grafico e non ha una console propria: senza
 * questo, una diagnostica da riga di comando dovrebbe uscire in una finestra
 * modale, che blocca chi la sta usando da uno script.
 * @return false se non c'era nessuna console: allora serve la finestra.
 */
bool writeToParentConsole(const std::wstring& text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) {
        FreeConsole();
        return false;
    }
    DWORD written = 0;
    // WriteConsoleW vale solo su una console vera: con l'output rediretto su file
    // o pipe l'handle non lo è, e va scritto a byte in UTF-8.
    if (!WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr)) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string utf8(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                utf8.data(), n, nullptr, nullptr);
            WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
    FreeConsole();
    return true;
}

/** --schermi: elenco dei monitor rilevati, senza aprire nulla. */
int reportScreens() {
    const auto displays = app::enumerateDisplays();

    std::wstring text = L"Schermi rilevati: " + std::to_wstring(displays.size()) + L"\n\n";
    for (std::size_t i = 0; i < displays.size(); ++i) {
        text += std::to_wstring(i + 1) + L".  " + displays[i].describe() + L"\n     " +
                displays[i].deviceName + L"\n";
    }

    if (displays.size() < 2) {
        text += L"\nCon un solo schermo il programma resta a finestra unica:\n"
                L"immagini e pannello diagnostico insieme.";
    } else {
        const int stored = app::matchStoredChoice(displays);
        text += L"\nProiezione: ";
        text += (stored >= 0) ? (L"schermo " + std::to_wstring(stored + 1) + L" (memorizzato)")
                              : std::wstring(L"da scegliere al prossimo avvio");
    }
    text += L"\n";

    if (!writeToParentConsole(L"\n" + text + L"\n")) {
        MessageBoxW(nullptr, text.c_str(), L"Mind Zoom - schermi", MB_ICONINFORMATION);
    }
    return 0;
}

/**
 * --controlla: verdetto su una registrazione, senza aprire nulla.
 *
 * Fa passare la sessione per la STESSA catena dell'esperienza - decodifica,
 * STFT, gating - e riporta cosa ne esce. Serve a rispondere in dieci secondi
 * alla domanda che conta dopo ogni sessione: quello che è stato registrato è
 * segnale, oppure no? Guardarlo dal pannello richiede di rifare la sessione.
 */
int reportRecording(const std::wstring& path) {
    const auto rec = ble::loadRecording(path);

    std::wstring text = L"File: " + path + L"\n";
    if (!rec.ok) {
        text += L"\nNON UTILIZZABILE\n  " +
                std::wstring(rec.error.begin(), rec.error.end()) + L"\n";
        if (!writeToParentConsole(L"\n" + text + L"\n")) {
            MessageBoxW(nullptr, text.c_str(), L"Mind Zoom - controllo", MB_ICONWARNING);
        }
        return 2;
    }

    auto num = [](double v, int dec) {
        std::wstringstream ss;
        ss.imbue(std::locale::classic());
        ss << std::fixed << std::setprecision(dec) << v;
        return ss.str();
    };

    text += L"Formato v" + std::to_wstring(rec.version) +
            L"   notifiche grezze: " + std::to_wstring(rec.packets.size()) +
            L"   campioni: " + std::to_wstring(rec.samples.size()) +
            L"   durata: " + num(rec.seconds(), 1) + L" s\n";
    if (rec.redecoded) {
        text += L"I campioni sono stati RICOSTRUITI dai pacchetti grezzi con il\n"
                L"decodificatore attuale, non letti da quelli salvati allora.\n";
    }

    // Stessa catena dell'esperienza: se qui il verdetto e' buono, lo e' anche li'.
    dsp::SlidingStft stft;
    dsp::Gating      gating;
    int  frames = 0;
    std::array<int, 5> faults{};
    double sumAutocorr = 0.0, sumMains = 0.0, sumAmp = 0.0;

    // Il verdetto va dato anche sulla CODA, non solo sulla media: una sessione
    // in cui si sistema la fascia nei primi minuti ha una media pessima e una
    // fine ottima, e mediarle nasconde proprio il fatto che il problema e'
    // stato risolto.
    std::vector<int> storia;
    storia.reserve(rec.samples.size() / config::kStftHop + 1);

    for (const auto& s : rec.samples) {
        std::array<double, config::kChannels>        uv{};
        std::array<std::uint16_t, config::kChannels> adc{};
        for (std::size_t c = 0; c < config::kChannels; ++c) {
            uv[c]  = s.uv[c];
            adc[c] = s.adc[c];
        }
        if (!stft.pushSample(uv, adc)) continue;

        const auto q = gating.assess(stft);
        const int  f = std::clamp(static_cast<int>(q.fault), 0, 4);
        ++faults[static_cast<std::size_t>(f)];
        storia.push_back(f);
        sumAutocorr += q.autocorr1;
        sumMains    += q.mainsFraction;
        sumAmp      += q.maxAbsRaw;
        ++frames;
    }

    if (frames == 0) {
        text += L"\nTroppo corta per un verdetto: servono almeno 256 campioni.\n";
    } else {
        const wchar_t* names[] = {L"OK", L"RETE 50 Hz", L"SATURO", L"PIATTO",
                                  L"NON E' UN SEGNALE"};
        text += L"\nFinestre analizzate: " + std::to_wstring(frames) + L"\n";
        for (std::size_t i = 0; i < faults.size(); ++i) {
            if (faults[i] == 0) continue;
            const double pct = 100.0 * faults[i] / frames;
            text += L"  " + std::wstring(names[i]) + L": " + num(pct, 1) + L"%\n";
        }
        text += L"\nCorrelazione fra campioni: " + num(sumAutocorr / frames, 3) +
                L"   (serve > " + num(config::kMinAutocorr1, 2) + L")\n";
        text += L"Potenza a 50 Hz:           " + num(100.0 * sumMains / frames, 1) +
                L"%   (serve < " + num(100.0 * config::kMaxMainsFraction, 0) + L"%)\n";
        text += L"Ampiezza media:            " + num(sumAmp / frames, 1) + L" uV\n";

        // Coda: gli ultimi 30 secondi di frame spettrali.
        const int codaFrames =
            std::min<int>(frames, 30 * config::kSampleRate / config::kStftHop);
        int codaOk = 0;
        for (std::size_t i = storia.size() - static_cast<std::size_t>(codaFrames);
             i < storia.size(); ++i) {
            if (storia[i] == 0) ++codaOk;
        }
        const double codaPct = 100.0 * codaOk / codaFrames;
        const double okPct   = 100.0 * faults[0] / frames;

        const int codaSecondi = codaFrames * config::kStftHop / config::kSampleRate;
        if (codaFrames < frames) {
            text += L"\nUltimi " + std::to_wstring(codaSecondi) + L" s: " +
                    num(codaPct, 1) + L"% utilizzabile";
            text += (codaPct > okPct + 10.0)
                        ? L"   (in miglioramento: la fascia si e' assestata)\n"
                        : L"\n";
        }

        text += L"\n";
        if (codaPct > 70.0 && okPct <= 70.0) {
            text += L"VERDETTO: partenza difficile, ma alla fine il segnale era buono.\n"
                    L"Quello che conta e' la coda: da li' in poi la fascia leggeva te.\n";
        } else if (okPct > 70.0) {
            text += L"VERDETTO: segnale utilizzabile.\n";
        } else if (faults[1] > frames / 2) {
            // Il ronzio residuo e' quello che la derivazione bipolare NON ha
            // cancellato, quindi non e' modo comune: i due frontali stanno
            // captando cose diverse, il che accade quando uno dei due non e'
            // accoppiato. Niente consigli di inumidire: in un contesto pubblico
            // non e' praticabile.
            text += L"VERDETTO: quello che resta dopo la derivazione bipolare e'\n"
                    L"ancora rete elettrica, quindi NON e' di modo comune: i due\n"
                    L"frontali stanno leggendo cose diverse, e di solito vuol dire\n"
                    L"che uno dei due non e' accoppiato alla pelle.\n"
                    L"Da provare, in quest'ordine: pulire gli elettrodi con una\n"
                    L"salvietta all'alcol isopropilico (sgrassa e disinfetta),\n"
                    L"scostare i capelli sotto la fascia, spostare la fascia di\n"
                    L"qualche millimetro, allontanarsi dagli alimentatori.\n";
        } else if (faults[4] > frames / 2) {
            text += L"VERDETTO: i campioni non formano una forma d'onda.\n"
                    L"Questo si' che indica un difetto di decodifica nel programma.\n"
                    L"I pacchetti grezzi sono nel file: si puo' correggere a posteriori.\n";
        } else {
            text += L"VERDETTO: segnale disturbato a tratti, utilizzabile solo in parte.\n";
        }
    }

    if (!writeToParentConsole(L"\n" + text + L"\n")) {
        MessageBoxW(nullptr, text.c_str(), L"Mind Zoom - controllo", MB_ICONINFORMATION);
    }
    return 0;
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
                     L"  --controlla FILE.mzr   dice se quella sessione contiene\n"
                     L"                         segnale vero, ed esce\n"
                     L"  --senza-log            non registrare questa sessione\n"
                     L"  --schermi              elenca i monitor rilevati ed esce\n"
                     L"  --schermo-singolo      non usare il secondo schermo\n\n"
                     L"Senza opzioni il programma registra da solo in registrazioni\\.")
                        .c_str(),
                    L"Mind Zoom", MB_ICONINFORMATION);
        return 1;
    }

    if (opt.listScreens) return reportScreens();
    if (!opt.inspectPath.empty()) return reportRecording(opt.inspectPath);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    // --- schermi ---
    g.displays = app::enumerateDisplays();
    const bool wantDual = !selfTest && !opt.singleScreen && g.displays.size() >= 2;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MindZoomWindow";
    RegisterClassExW(&wc);

    WNDCLASSEXW pc = wc;
    pc.lpszClassName = L"MindZoomProjection";
    pc.hCursor       = nullptr;   // sul proiettore il puntatore non deve comparire
    RegisterClassExW(&pc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Mind Zoom",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    g.opHwnd = hwnd;

    // Finestra di proiezione: senza bordo, sopra tutto, dimensionata sul monitor.
    if (wantDual) {
        g.projHwnd = CreateWindowExW(WS_EX_TOPMOST, pc.lpszClassName, L"Mind Zoom",
                                     WS_POPUP, 0, 0, 640, 480,
                                     nullptr, nullptr, instance, nullptr);
    }

    render::GraphicsCore graphics;
    if (!graphics.init()) {
        if (!selfTest) {
            MessageBoxW(hwnd, L"Inizializzazione di Direct2D fallita.", L"Mind Zoom", MB_ICONERROR);
        }
        return 2;
    }

    // Le immagini vanno decodificate PRIMA di creare i render target: è
    // createTarget() a caricarne le bitmap di device, e se le sorgenti non ci
    // sono ancora ne carica zero. Il risultato non è un errore, è un programma
    // che gira senza mostrare nulla.
    const std::wstring assets = exeDirectory() + L"\\assets";
    if (!graphics.loadSprites(assets, config::kTotalImages)) {
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

    render::Renderer renderer;
    render::Renderer projRenderer;
    if (!renderer.init(graphics, hwnd)) {
        if (!selfTest) {
            MessageBoxW(hwnd, L"Inizializzazione di Direct2D fallita.", L"Mind Zoom", MB_ICONERROR);
        }
        return 2;
    }
    if (g.projHwnd && !projRenderer.init(graphics, g.projHwnd)) {
        // La proiezione è un di più: se non si inizializza si continua a schermo
        // singolo invece di negare l'esperienza.
        DestroyWindow(g.projHwnd);
        g.projHwnd = nullptr;
    }

    // Le bitmap devono essere davvero arrivate sul target. Senza questo
    // controllo un programma che non disegna nulla esce comunque con successo:
    // è esattamente il modo in cui il difetto precedente è passato inosservato.
    if (renderer.spriteCount() != config::kTotalImages) {
        if (!selfTest) {
            MessageBoxW(hwnd, L"Le immagini non sono state caricate sulla scheda grafica.",
                        L"Mind Zoom", MB_ICONERROR);
        }
        return 6;
    }

    if (selfTest) {
        // Un frame completo: sprite in crossfade, scheda di calibrazione, HUD,
        // scelta schermo e telemetria, cioè ogni percorso di disegno.
        control::ZoomController probe;
        app::ControlState st;
        st.calibStage = static_cast<int>(control::CalibStage::Concentrate);
        st.calibProgress   = 0.45;
        st.calibShowTarget = true;
        st.calibEffN       = 9.0;
        st.calibValid = true;
        st.absMin = 0.5; st.absMax = 1.5; st.neutral = 1.0;
        st.localMin = 0.9; st.localMax = 1.2;

        // Un po' di storia finta: senza, le tracce non verrebbero disegnate e il
        // percorso delle spezzate resterebbe non verificato.
        app::TelemetryHistory history;
        for (int i = 0; i < 200; ++i) {
            st.frames = static_cast<std::uint64_t>(i) + 1;
            st.smoothedIndex = 1.0 + 0.3 * std::sin(i * 0.1);
            st.rawIndex      = st.smoothedIndex + 0.05;
            st.velocity      = 0.2 * std::sin(i * 0.07);
            st.maxAbsRaw     = 80.0 + 20.0 * std::sin(i * 0.05);
            history.append(st, 0.4, 0.38, i % 40 < 5);
        }

        renderer.begin(kBg);
        const auto cf = probe.crossfade();
        renderer.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                            static_cast<float>(cf.activeAlpha));
        renderer.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                            static_cast<float>(cf.nextAlpha));
        app::drawTelemetry(renderer, render::rect(20, 20, 600, 700), history, st,
                      control::Tunables{}, kPlotTheme);
        drawProjectionScale(renderer, cf);
        drawCalibrationCard(renderer, st, 0.6);
        drawHud(renderer, st, probe, &cf, control::Tunables{});
        drawScreenPickerPanel(renderer, 0);
        drawScreenPicker(renderer, 0);
        const bool drawn = renderer.end();

        renderer.shutdown();
        graphics.shutdown();
        DestroyWindow(hwnd);
        CoUninitialize();
        return drawn ? 0 : 4;
    }

    ShowWindow(hwnd, showCmd);

    // --- scelta dello schermo di proiezione ---
    if (g.projHwnd) {
        const int stored = app::matchStoredChoice(g.displays);
        if (stored >= 0) {
            confirmProjection(stored);
        } else {
            // Candidato di partenza: il primo schermo NON primario, che è quasi
            // sempre quello giusto. Resta comunque da confermare guardando.
            g.choosing.store(true, std::memory_order_release);
            placeProjection(1);
        }
    }

    // --- sorgente del segnale: la fascia, oppure una registrazione ---
    if (!opt.replayPath.empty()) {
        const std::string err = startReplay(opt.replayPath);
        if (!err.empty()) {
            MessageBoxW(hwnd,
                        (L"Impossibile riprodurre:\n" + opt.replayPath + L"\n\n" +
                         std::wstring(err.begin(), err.end())).c_str(),
                        L"Mind Zoom", MB_ICONERROR);
            return 5;
        }
    }

    if (opt.record) {
        // Il file nasce al primo campione: una sessione senza fascia non lascia
        // un file vuoto che poi ricompare nell'elenco e viene rifiutato.
        g.recorder.arm(newRecordingPath());
    }

    // Thread 1: la fascia. Se si sta già riproducendo un file, il thread di
    // riproduzione è partito dentro startReplay() e scrive nello stesso ring,
    // quindi il resto del programma è identico.
    g_muse.onSample([](const ble::Sample& s) {
        if (!g.ring.push(s)) g.dropped.fetch_add(1, std::memory_order_relaxed);
    });
    g_muse.onRawPacket([](const std::uint8_t* d, std::size_t n) {
        RawPacket p;
        p.len = static_cast<std::uint16_t>(std::min<std::size_t>(n, sizeof(p.data)));
        std::memcpy(p.data, d, p.len);
        g.rawRing.push(p);   // se pieno si perde: la diagnostica non blocca il BLE
    });
    g_muse.onLog([](const std::string& msg) { g.pushBleLog(msg); });
    if (!g.replaying.load(std::memory_order_acquire)) g_muse.start();

    // Thread 2: DSP.
    std::thread dsp(dspThread);

    control::ZoomController zoom;
    double squarePos = 0.5;   // posizione filtrata del quadratino (EMA a frame rate)
    util::Ema velRender;      // interpola la velocità dai 5.3 Hz del DSP al vsync
    app::TelemetryHistory history;   // storia per i grafici

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
        if (g.projHwnd) {
            RECT pr{};
            GetClientRect(g.projHwnd, &pr);
            projRenderer.resize(static_cast<UINT>(pr.right - pr.left),
                                static_cast<UINT>(pr.bottom - pr.top));
        }

        const auto st = g.state.read();
        const auto phase = static_cast<control::Phase>(st.phase);
        const bool choosing = g.choosing.load(std::memory_order_acquire);

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

        // Cambiata la sorgente, la storia precedente non c'entra più nulla con
        // quello che sta scorrendo: lasciarla renderebbe i grafici bugiardi.
        if (g.resetHistory.exchange(false, std::memory_order_acq_rel)) {
            history.clear();
            zoom.reset();
            velRender.reset();
        }

        // Storia per i grafici: si appende solo sui frame nuovi del DSP, non a
        // 60 Hz, altrimenti ogni valore comparirebbe undici volte.
        history.append(st, zoom.targetFocus(), zoom.currentFocus(), zoom.locked());

        const auto cf = zoom.crossfade();
        const bool showCard = (phase == control::Phase::Onboarding);

        // --- schermo di proiezione: il partecipante ---
        // In interazione qui NON va disegnato nulla oltre all'immagine.
        if (g.projHwnd) {
            projRenderer.begin(kBg);
            projRenderer.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                                    static_cast<float>(cf.activeAlpha));
            projRenderer.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                                    static_cast<float>(cf.nextAlpha));
            if (choosing) {
                drawScreenPicker(projRenderer, g.candidate.load(std::memory_order_relaxed));
            } else {
                // La scala sta SOTTO la scheda di calibrazione: durante
                // l'onboarding l'immagine non e' ancora il soggetto.
                drawProjectionScale(projRenderer, cf);
                if (showCard) drawCalibrationCard(projRenderer, st, squarePos);
            }
            projRenderer.end();
        }

        // --- schermo dell'operatore ---
        renderer.begin(kBg);

        if (g.projHwnd) {
            // Telemetria a tutto campo. L'immagine resta come miniatura: serve a
            // vedere cosa sta guardando il partecipante senza girarsi.
            const auto win = renderer.size();
            const float thumbW = std::min(360.0f, win.width * 0.32f);
            const float thumbH = thumbW * 0.62f;
            const render::Rect thumb = render::rect(win.width - thumbW - 20.0f, 20.0f,
                                                  win.width - 20.0f, 20.0f + thumbH);

            app::drawTelemetry(renderer,
                          render::rect(20.0f, 20.0f, std::max(320.0f, win.width - thumbW - 44.0f),
                                      win.height - 20.0f),
                          history, st, g.tune, kPlotTheme);

            renderer.drawSpriteIn(cf.activeIndex, thumb, static_cast<float>(cf.activeScale),
                                  static_cast<float>(cf.activeAlpha));
            renderer.drawSpriteIn(cf.activeIndex + 1, thumb, static_cast<float>(cf.nextScale),
                                  static_cast<float>(cf.nextAlpha));
            renderer.drawRectOutline(thumb, {1, 1, 1, 0.18f}, 1.0f, 6.0f);
            renderer.drawText(L"proiezione   " + std::to_wstring(cf.magnification) + L"x",
                              render::rect(thumb.left, thumb.bottom + 4.0f, thumb.right,
                                          thumb.bottom + 22.0f),
                              12.0f, kMuted, render::TextAlign::Center);
        } else {
            // Schermo singolo: comportamento di sempre, immagine a tutto campo.
            renderer.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                                static_cast<float>(cf.activeAlpha));
            renderer.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                                static_cast<float>(cf.nextAlpha));
        }

        // La scheda di calibrazione compare su ENTRAMBI gli schermi: l'operatore
        // deve vedere esattamente ciò che vede il partecipante mentre lo guida.
        if (showCard && !choosing) {
            drawCalibrationCard(renderer, st, squarePos);
        }

        if (g.hudVisible.load(std::memory_order_relaxed) && !choosing) {
            drawHud(renderer, st, zoom, &cf, g.tune);
        }

        if (choosing) {
            drawScreenPickerPanel(renderer, g.candidate.load(std::memory_order_relaxed));
        }

        renderer.end();
    }

    g.running.store(false, std::memory_order_release);
    // Il DSP va fermato PRIMA di chiudere il file: è lui che ci scrive dentro.
    if (dsp.joinable()) dsp.join();
    if (g.replayWorker.joinable()) g.replayWorker.join();
    g_muse.stop();

    // A sessione finita si dice dove sono finiti i dati e come rigiocarli:
    // scritto in una cartella e mai nominato, il log non lo userebbe nessuno.
    const bool saved = g.recorder.hasData();
    const std::wstring path = g.recorder.path();
    const double secs = g.recorder.seconds();
    g.recorder.close();

    projRenderer.shutdown();
    renderer.shutdown();
    graphics.shutdown();
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
