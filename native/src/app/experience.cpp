#include "app/experience.hpp"
#include "app/biodetails_blob.hpp"

#include "app/crash.hpp"
#include "app/diagnostics.hpp"
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

// Tavolozza Biodetails (design/biodetails/IDENTITA-VISIVA.md): i cinque
// colori piatti del poster, e basta. Fondo acqua da bordo a bordo, nero per
// tutto cio' che va letto, i tre caldi - magenta, corallo, arancio - per
// titoli, cifre ed etichette. Niente grigi: un testo secondario e' NERO con
// meno opacita', mai un tono intermedio. Niente sfumature, ombre, bagliori.
constexpr render::Color kBg        {0.671f, 0.812f, 0.835f, 1.0f};   // #ABCFD5 acqua: il fondo
constexpr render::Color kInk       {0.0f, 0.0f, 0.0f, 1.0f};         // nero: il testo
constexpr render::Color kMuted     {0.0f, 0.0f, 0.0f, 0.68f};        // nero attenuato: testo secondario
constexpr render::Color kAccent    {1.0f, 0.0f, 0.52f, 1.0f};        // #FF0085 magenta: la macchia, la concentrazione
constexpr render::Color kAccent2   {1.0f, 0.43f, 0.11f, 1.0f};       // #FF6E1C arancio: titoletti, il rilassamento
constexpr render::Color kCoral     {1.0f, 0.22f, 0.32f, 1.0f};       // #FF3852 corallo: il lettering, le cifre
// Gli stati del pannello operatore restano nella tavolozza: normale = nero
// (sul fondo acqua e' lo stato "niente da segnalare"), attenzione = arancio,
// errore = corallo.
constexpr render::Color kOk        = kInk;
constexpr render::Color kWarn      = kAccent2;
constexpr render::Color kBad       = kCoral;
constexpr render::Color kPanel     = kBg;                            // i pannelli sono campiture piatte del fondo
constexpr render::Color kHairline  {0.0f, 0.0f, 0.0f, 0.14f};        // l'unico "filetto": nero quasi trasparente

// Dietro la FOTOGRAFIA il fondo resta nero, non acqua: e' la tela su cui
// vive l'immagine al microscopio (che ha i propri neri), e una banda color
// acqua ai lati di una foto che non riempie lo schermo la spegnerebbe. E' il
// colore con cui si pulisce il fotogramma; le schermate di contorno si
// dipingono da sole il proprio fondo acqua sopra.
constexpr render::Color kPhotoBg   {0.0f, 0.0f, 0.0f, 1.0f};

// Gli strumenti dell'operatore - pannello, grafici, debug - vivono sopra la
// foto, cioe' sul nero, non sulla carta. Li' la tavolozza si rovescia:
// l'acqua diventa l'inchiostro e il nero il fondo, e i tre caldi tornano
// leggibili (su nero stanno fra 5.6 e 7.5 di contrasto, sull'acqua fra 1.7 e
// 2.3). Non e' una seconda identita': sono gli stessi cinque colori, scambiati
// di ruolo.
constexpr render::Color kHudBg       {0.0f, 0.0f, 0.0f, 0.72f};
constexpr render::Color kHudInk      = kBg;
constexpr render::Color kHudMuted    {kBg.r, kBg.g, kBg.b, 0.62f};
constexpr render::Color kHudOk       = kBg;                        // "niente da segnalare"
constexpr render::Color kHudHairline {1.0f, 1.0f, 1.0f, 0.12f};

// Tavolozza per l'effetto caustico/particellare dei cerchi animati della
// calibrazione (drawCausticSphere, usata da drawBreathingCircles e
// drawFocusTarget): i due estremi della rampa dei puntini sono i due caldi
// estremi del poster, magenta e arancio; il corallo sta in mezzo (swarmTint).
constexpr render::Color kSiriBlue  = kAccent;
constexpr render::Color kSiriGreen = kAccent2;

/** Pallino numerato (badge) per i due passaggi della calibrazione: cerchio pieno + cifra. */
void drawStepBadge(render::Renderer& r, render::Point c, float radius, int number, bool active,
                   bool done, render::Color tint) {
    // Fatto = nero pieno con cifra acqua; attivo = il caldo della fase con
    // cifra nera; in attesa = solo un filetto nero. Tre campiture piatte.
    if (done) {
        r.fillCircle(c, radius, kInk);
    } else if (active) {
        r.fillCircle(c, radius, tint);
    } else {
        r.drawArc(c, radius - 0.75f, -90.0f, 270.0f, 1.5f, kHairline);
    }
    const render::Color numColor = done ? kBg : active ? kInk : kMuted;
    r.drawTextBodyCentered(std::to_wstring(number), c, radius * 1.1f, numColor, true);
}

/**
 * Stessa tinta, luminanza piu' bassa: l'estremo scuro delle sfumature
 * diagonali. Scala uniformemente i tre canali invece di spostare la tinta,
 * cosi' il colore resta "lo stesso" percettivamente, solo piu' in ombra.
 */
constexpr render::Color darken(render::Color c, float amount) noexcept {
    const float k = 1.0f - amount;
    return {c.r * k, c.g * k, c.b * k, c.a};
}

/** Pulsante a pillola: sfumatura diagonale sottile sullo stesso tono. */
void drawPillButton(render::Renderer& r, render::Rect box, const std::wstring& label,
                    render::Color fill) {
    const float radius = box.height() * 0.5f;
    r.fillRectShadow(box, fill, radius, {fill.r, fill.g, fill.b, 0.35f}, 18.0f, {0.0f, 6.0f});
    r.fillRectGradient(box, fill, darken(fill, 0.16f), radius);
    r.drawTextCentered(label, {(box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f},
                       box.height() * 0.4f, {1.0f, 1.0f, 1.0f, 1.0f}, true);
}

/**
 * Pulsante d'ingresso: e' il gruppo Figma 5:6 (620x130) riprodotto livello
 * per livello, coi valori letti dal file; tutto e' in proporzione all'altezza
 * del box (s = 1 alla dimensione originale). Con l'identita' Biodetails
 * cambiano SOLO i colori: la costruzione - pillola, interno sfocato, ombra
 * interna, bagliore, contorno a sfumatura animata - resta quella.
 *
 *   1. pillola base magenta profondo con ombra interna arancio (blur 7.8);
 *   2. sopra, una pillola piu' piccola (rientro 8x6) piu' scura con layer blur
 *      26.4: e' lei a fare l'interno scuro che schiarisce verso il bordo;
 *   3. contorno 2px INTERNO a sfumatura lineare arancio -> corallo -> magenta,
 *      con layer blur 5.6;
 *   4. etichetta Manrope Regular 40, bianca.
 *
 * Il contorno sfocato e' l'unico pezzo approssimato: una sfumatura non passa
 * per il trucco dell'ombra (che ha un colore solo), quindi si sommano quattro
 * tratti concentrici di larghezza crescente e opacita' decrescente che
 * ricalcano il profilo gaussiano misurato sul render di Figma (sigma ~2px,
 * picco ~45%).
 */
// Riquadro del pulsante "prosegui": lo condividono pagina d'ingresso e
// accoglienza, e soprattutto lo condivide handleClick(), che non disegna
// nulla. Se il bersaglio del clic fosse scritto una seconda volta, potrebbe
// scivolare rispetto al pulsante disegnato senza che niente lo segnali.
// La pagina d'ingresso lo posiziona per scostamento dal centro; l'accoglienza
// lo ricava invece dal proprio ritmo verticale (onboardButtonRect, piu' sotto).
constexpr float kLandingButtonDy = 226.0f;
constexpr float kEntryButtonW    = 400.0f;
constexpr float kEntryButtonH    = 84.0f;

render::Rect entryButtonRect(render::Size win, float dy) {
    const float cx = win.width * 0.5f;
    const float cy = win.height * 0.5f;
    return render::rect(cx - kEntryButtonW * 0.5f, cy + dy,
                        cx + kEntryButtonW * 0.5f, cy + dy + kEntryButtonH);
}

void drawEntryButton(render::Renderer& r, render::Rect box, const std::wstring& label,
                     double animT, float opacity = 1.0f) {
    constexpr render::Color kBase  {0.77f, 0.0f, 0.40f, 1.0f};       // #C40066 magenta profondo
    constexpr render::Color kInner {0.54f, 0.0f, 0.28f, 1.0f};       // #8A0047
    constexpr render::Color kRim   {1.00f, 0.10f, 0.56f, 1.0f};      // #FF1A8F
    constexpr render::Color kGlow  = kAccent2;                       // #FF6E1C arancio

    // Un pulsante "spento" e' lo STESSO disegno con meno opacita', non un
    // secondo disegno: cosi' l'accensione e' una dissolvenza sola e non ci
    // sono due aspetti da tenere allineati quando il bottone cambia.
    const auto fade = [opacity](render::Color c) {
        c.a *= opacity;
        return c;
    };

    const float radius = box.height() * 0.5f;
    const float s      = box.height() / 130.0f;

    r.fillRect(box, fade(kBase), radius);

    const render::Rect inner = render::rect(box.left + 8.0f * s, box.top + 6.0f * s,
                                            box.right - 8.0f * s, box.bottom - 6.0f * s);
    // I "blur" di Figma sono raggi, non sigma: misurato sul render del bordo
    // (blur 5.6 -> sigma ~2), il rapporto e' circa 2.8.
    constexpr float kFigmaBlurToSigma = 1.0f / 2.8f;
    r.fillRectBlurred(inner, fade(kInner), radius - 6.0f * s, 26.4f * kFigmaBlurToSigma * s);
    // Ombra interna del livello scuro (#0D2B8E, blur 29.5, spread 8): e' lei a
    // tenere chiara la fascia vicino al bordo prima che l'interno scurisca.
    r.fillInnerGlow(inner, fade(kRim), radius - 6.0f * s, 29.5f * kFigmaBlurToSigma * s, 8.0f * s);

    r.fillInnerGlow(box, fade(kGlow), radius, 7.8f * kFigmaBlurToSigma * s);

    // Sfumatura di Figma: u = a*x/W + b*y/H + tx in coordinate normalizzate
    // del nodo, arancio a u=0, corallo a 0.53, magenta a u=1. Per animarla la si
    // rende periodica - la rampa e poi la stessa a specchio, periodo 2 - e la
    // si fa scorrere lungo il proprio asse: i colori viaggiano da sinistra a
    // destra e ogni punto del bordo passa arancio -> corallo -> magenta ->
    // corallo -> arancio, senza mai un salto. Nel fotogramma con fase 0 e' esattamente il
    // disegno statico.
    constexpr float a = 1.6368624f, b = 0.13640025f, tx = -0.38663131f;
    const float gx = a / box.width(), gy = b / box.height();
    const float g2 = gx * gx + gy * gy;
    constexpr double kGradientPeriodS = 6.0;
    const float phase = static_cast<float>(std::fmod(animT / kGradientPeriodS, 1.0)) * 2.0f;
    // Il bordo visibile copre u in [-0.39, 1.39]; con la fase fino a 2 serve
    // il motivo da u=-3 a u=2 (mappato su [0,1] con posizioni /5).
    constexpr float kU0 = -3.0f, kU1 = 2.0f;
    const auto at = [&](float u) {
        const float uu = u + phase;
        return render::Point{box.left + uu * gx / g2 - tx * gx / g2,
                             box.top + uu * gy / g2 - tx * gy / g2};
    };
    const render::Point from = at(kU0), to = at(kU1);
    constexpr render::Color kGreen   = kAccent2;                     // #FF6E1C arancio
    constexpr render::Color kBlue    = kCoral;                       // #FF3852 corallo
    constexpr render::Color kMagenta = kAccent;                      // #FF0085 magenta
    constexpr float kMid = 0.5288461f;
    constexpr int   kStopCount = 11;
    constexpr render::Color kStopBase[kStopCount] = {
        kGreen, kBlue, kMagenta, kBlue, kGreen, kBlue, kMagenta, kBlue, kGreen, kBlue, kMagenta};
    constexpr float kStopU[kStopCount] = {
        -3.0f, -3.0f + kMid, -2.0f, -1.0f - kMid, -1.0f, -1.0f + kMid, 0.0f, 1.0f - kMid,
        1.0f, 1.0f + kMid, 2.0f};
    float stopPos[kStopCount];
    for (int i = 0; i < kStopCount; ++i) stopPos[i] = (kStopU[i] - kU0) / (kU1 - kU0);

    // Tratto 2px interno: il suo centro sta 1px dentro il bordo.
    const render::Rect edge =
        render::rect(box.left + s, box.top + s, box.right - s, box.bottom - s);
    constexpr float kSigma = 1.8f;
    // Larghezze (in sigma) e opacita' composte in sequenza dal piu' largo al
    // piu' stretto: il profilo risultante e' la gaussiana a gradini, picco
    // ~55%. (Col picco al 45% misurato sul render il bordo usciva piu' fioco
    // del riferimento messo a confronto: la resa su schermo non e' la stessa.)
    constexpr float kRingHalfW[4] = {2.75f, 2.0f, 1.25f, 0.5f};
    constexpr float kRingAlpha[4] = {0.014f, 0.070f, 0.210f, 0.320f};
    for (int k = 0; k < 4; ++k) {
        render::Color stops[kStopCount];
        for (int i = 0; i < kStopCount; ++i) {
            stops[i]   = kStopBase[i];
            stops[i].a = kRingAlpha[k] * opacity;
        }
        r.drawRectOutlineGradient(edge, stops, stopPos, kStopCount, from, to,
                                  2.0f * kRingHalfW[k] * kSigma * s, radius - s);
    }

    r.drawTextBodyCentered(label, {(box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f},
                           40.0f * s, {1.0f, 1.0f, 1.0f, opacity});
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
    util::DoubleBuffer<app::WaveSnapshot> wave;

    control::Tunables                     tune;
    util::DoubleBuffer<control::Tunables> tunables;

    void publishTunables() { tunables.publish(tune); }

    std::atomic<int>           command{static_cast<int>(app::Command::None)};
    std::atomic<bool>          running{true};
    std::atomic<std::uint64_t> dropped{0};
    // 0 nascosto, 1 base, 2 esperto: vedi StartOptions::hudLevel.
    std::atomic<int>           hudLevel{0};
    std::atomic<bool>          quitRequested{false};
    // Pagina d'ingresso: la prima cosa che si vede all'avvio, prima ancora
    // della calibrazione. Non e' uno stato della calibrazione - riguarda
    // l'esperienza intera - quindi vive qui e non in CalibStage, che altrimenti
    // finirebbe per descrivere cose che con la calibrazione non c'entrano.
    std::atomic<bool>          landingVisible{true};
    // Accoglienza (dopo INVIO sulla pagina d'ingresso) e congedo (dopo il
    // riavvio tenuto premuto). Stessa ragione della pagina d'ingresso per
    // stare qui e non in CalibStage: descrivono l'esperienza intera, non la
    // calibrazione. I due orologi che le governano - da quanto sono aperte -
    // vivono invece col resto dello stato di disegno, perche' avanzano in
    // frame() e nessun altro thread li guarda.
    std::atomic<bool>          onboardingVisible{false};
    std::atomic<bool>          offboardingVisible{false};
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
    std::string   debugLogPath;

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
            debugLogPath = path;
            // Il log deve leggersi da solo, senza il programma accanto: la
            // legenda dei codici sta in testa, e ogni riga [MZ-...] porta
            // comunque il proprio testo in chiaro.
            debugLog << "=== Mind Zoom - log di diagnostica (macOS) ===\n"
                     << "build: " << __DATE__ << " " << __TIME__ << "\n"
                     << "kLocalTolerance=" << config::kLocalTolerance
                     << " kSampleRate=" << config::kSampleRate
                     << " kChannels=" << config::kChannels << "\n"
                     << "Come leggerlo: le righe [MZ-xNN] sono lo stato del sistema in linguaggio\n"
                     << "naturale (B = Bluetooth/fascia, S = segnale EEG, C = calibrazione,\n"
                     << "R = grafica/prestazioni, A = avvio, I = informativo, X = crash).\n"
                     << "Ogni 10 s una riga [stato] riassume fps, fase e codice corrente:\n"
                     << "se il file finisce senza '=== fine sessione ===' e senza '[MZ-X01]',\n"
                     << "il programma e' stato interrotto dall'esterno (chiuso a forza o spento).\n";
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
    beginReplay(L"segnale simulato", ble::synthetic::generate(kSyntheticSeconds));
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

            // Forme d'onda per il pannello operatore: i ring dell'STFT
            // srotolati in ordine di tempo. Static per non azzerare 6 KB a
            // ogni frame; il thread DSP e' uno solo.
            static app::WaveSnapshot wave;
            constexpr int kN = dsp::SlidingStft::kN;
            for (int ch = 0; ch < config::kChannels; ++ch) {
                const float* src = stft.dcFree(ch);
                const int    w   = stft.writeIndex(ch);
                for (int i = 0; i < kN; ++i) wave.raw[ch][i] = src[(w + 1 + i) % kN];
            }
            {
                const float* src = stft.filtered(config::kBipolar);
                const int    w   = stft.writeIndex(config::kBipolar);
                for (int i = 0; i < kN; ++i) wave.processed[i] = src[(w + 1 + i) % kN];
                std::memcpy(wave.spectrum, stft.mags(config::kBipolar), sizeof wave.spectrum);
            }
            wave.frame = st.frames;
            g.wave.publish(wave);
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

/**
 * Riga di stato in fondo alla scheda: la stessa striscia serve a tutte le fasi.
 *
 * Sempre NERA, qualunque cosa dica. Sul fondo acqua i tre caldi della palette
 * stanno fra 1.7 e 2.3 di contrasto (vedi la tabella nelle linee guida): a
 * 14pt sono illeggibili, e questa riga la legge il pubblico. Lo stato lo
 * dicono gia' le parole - "non collegata", "ricerca in corso" - non serve
 * ripeterlo con un colore che toglie leggibilita'.
 */
void drawStatusHint(render::Renderer& r, render::Rect box, const std::wstring& text) {
    r.drawTextBody(text, box, 14.0f, kInk, render::TextAlign::Center);
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
    // Con la tavolozza Biodetails la rampa e' quella dei tre caldi del poster
    // in ordine di temperatura, arancio -> corallo -> magenta: tre tinte
    // distinguibili, tutte gia' nel foglio, e i toni di mezzo restano caldi
    // perche' i tre estremi condividono rosso = 1.
    static constexpr render::Color kStops[] = {
        kSiriGreen,                    // arancio, estremo della rampa
        {1.0f, 0.33f, 0.20f, 1.0f},    // fra arancio e corallo
        kCoral,                        // corallo
        {1.0f, 0.12f, 0.42f, 1.0f},    // fra corallo e magenta
        kSiriBlue,                     // magenta
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
            ax += (std::sin(py_[si] * 3.1f + t * 0.42f + seed * kTau) * 0.45f +
                   std::sin(py_[si] * 9.7f - t * 1.40f + seed * kTau * 2.1f) * 0.12f) * vivacita;
            ay += (std::cos(px_[si] * 2.7f + t * 0.36f + seed * kTau * 1.3f) * 0.45f +
                   std::cos(px_[si] * 8.3f - t * 1.30f + seed * kTau * 0.7f) * 0.12f) * vivacita;

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
    // Era 1.6 (con le frequenze del campo a 0.28/0.95/0.24/0.85): alzato di
    // ~1.75x, e le frequenze di 1.5x, per un moto piu' rapido.
    static constexpr float kDynamism = 2.8f;
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

// ---------------------------------------------------------------------------
// Campo di metaball: la macchia del poster Biodetails, viva.
//
// Ha preso il posto dello sciame di puntini della pagina d'ingresso. La
// macchia del poster (design/biodetails/IDENTITA-VISIVA.md, par. 5) e' fatta
// di tre lobi tondi uniti da colli lisci: e' gia' un campo di metaball
// disegnato a mano. Qui i lobi diventano sorgenti di un campo scalare
//
//     f(p) = somma di r_i^2 / |p - c_i|^2 ,  contorno dove f = 1
//
// che si fondono da soli quando si avvicinano - il collo si forma con la
// stessa curvatura del poster - e si staccano allontanandosi. E' la "tensione
// superficiale" richiesta: non e' simulata, viene dalla forma del campo.
//
// I centri misurati sul tracciato del poster (distance transform sul contorno,
// in unita' della LARGHEZZA del riquadro della macchia):
//
//     lobo               centro x   centro y   raggio
//     grande, in basso     0.482      0.411     0.155
//     destro, in alto      0.882      0.118     0.118
//     sinistro             0.082      0.196     0.076
//
// Il riquadro e' alto 0.566 volte la propria larghezza.
// ---------------------------------------------------------------------------

/** Una sorgente del campo: centro e raggio, in pixel di finestra. */
struct MetaBall {
    float x, y, r;
};

class MetaballField {
public:
    /**
     * Ricalcola posizioni e contorni. Da chiamare una volta per fotogramma.
     *
     * @param area   zona in cui vive il campo (i lobi restano attorno al suo centro)
     * @param t      orologio dell'animazione, in secondi
     * @param gather 1 = raccolti e fusi (segnale presente), 0 = sparsi e separati
     */
    void update(render::Rect area, double t, float gather) {
        posiziona(area, t, gather);
        contorni();
    }

    void fill(render::Renderer& r, render::Color color) const {
        if (lens_.empty()) return;
        r.fillPolygons(verts_.data(), lens_.data(), static_cast<int>(lens_.size()), color);
    }

    /** Da chiudere con r.popClip(). Il dispositivo "il titolo cambia colore dentro la macchia". */
    void pushClip(render::Renderer& r) const {
        r.pushClipPolygons(verts_.data(), lens_.data(), static_cast<int>(lens_.size()));
    }

private:
    // I tre lobi del poster piu' due satelliti piccoli. I satelliti non stanno
    // nel foglio: servono qui perche' l'animazione vive dei momenti in cui due
    // masse si toccano e si fondono, e con tre lobi soli quei momenti sono
    // radi. Sono piu' piccoli di tutti e tre, quindi non cambiano la lettura
    // della forma - la macchia resta quella.
    //
    // Scostamenti dal centro del riquadro, in unita' della sua larghezza.
    struct Sorgente {
        float dx, dy, r;      // posizione a riposo e raggio
        float ax, ay;         // ampiezza della deriva
        float f1, f2, p1, p2; // due frequenze incommensurabili per asse, e le fasi
    };
    static constexpr Sorgente kSorgenti[] = {
        {-0.018f,  0.128f, 0.155f, 0.070f, 0.052f, 0.0290f, 0.0171f, 0.00f, 1.90f},
        { 0.382f, -0.165f, 0.118f, 0.085f, 0.063f, 0.0231f, 0.0139f, 2.20f, 0.40f},
        {-0.418f, -0.087f, 0.076f, 0.090f, 0.070f, 0.0187f, 0.0263f, 4.10f, 3.30f},
        { 0.200f,  0.220f, 0.052f, 0.105f, 0.082f, 0.0341f, 0.0197f, 1.30f, 5.10f},
        {-0.300f,  0.140f, 0.044f, 0.110f, 0.088f, 0.0223f, 0.0311f, 5.60f, 2.70f},
    };
    // Lato della cella della griglia, in pixel logici. Il contorno esce da un
    // marching squares: piu' fine e' la cella, piu' liscio il bordo e piu'
    // costa. Misurato in finestra 1280x800 su schermo retina, pagina
    // d'ingresso completa:
    //
    //     cella (px)    4      6      8     12
    //     fps          60     60     60     60
    //
    // Nessuna delle quattro fa scendere sotto il tetto del vsync (il campo e'
    // 5 sorgenti su ~4000 punti di reticolo: due ordini di grandezza meno del
    // vecchio sciame). Scelta 6: a 8 i tratti diritti si vedono sui bordi
    // quasi orizzontali, a 4 non si distingue da 6.
    static constexpr float kCella = 6.0f;

    void posiziona(render::Rect area, double t, float gather) {
        // L'unita' di misura e' la larghezza del riquadro della macchia, che
        // si prende una frazione della zona: con i satelliti e la deriva la
        // forma arriva a occupare circa 1.2 volte questa larghezza.
        const float u  = std::min(area.width() * 0.62f, area.height() * 1.10f);
        const float cx = (area.left + area.right) * 0.5f;
        const float cy = (area.top + area.bottom) * 0.5f;

        // Raccolti o sparsi: cambia la distanza dal centro, non il raggio. A
        // segnale presente i lobi si stringono e diventano una massa sola; a
        // segnale assente si allontanano finche' il campo non li tiene piu'
        // insieme e si staccano. La fusione e lo strappo li fa il campo.
        const float sparsi = 1.32f - 0.52f * std::clamp(gather, 0.0f, 1.0f);

        balls_.clear();
        for (const auto& g : kSorgenti) {
            // Somma di due sinusoidi incommensurabili per asse: il moto non
            // torna mai sullo stesso giro, ma resta lento e senza scatti (i
            // periodi piu' brevi sono di circa 29 secondi).
            const auto onda = [&](float f1, float f2, float p1, float p2) {
                return 0.62f * std::sin(static_cast<float>(t) * f1 * 6.2831853f + p1) +
                       0.38f * std::sin(static_cast<float>(t) * f2 * 6.2831853f + p2);
            };
            const float dx = onda(g.f1, g.f2, g.p1, g.p2);
            const float dy = onda(g.f2, g.f1, g.p2 + 1.7f, g.p1 + 0.6f);
            balls_.push_back({cx + (g.dx * sparsi + g.ax * dx) * u,
                              cy + (g.dy * sparsi + g.ay * dy) * u,
                              g.r * u});
        }
    }

    /** f(x,y) del campo. Il +1 evita la singolarita' esatta sul centro di una sorgente. */
    float campo(float x, float y) const {
        float v = 0.0f;
        for (const auto& b : balls_) {
            const float dx = x - b.x, dy = y - b.y;
            v += b.r * b.r / (dx * dx + dy * dy + 1.0f);
        }
        return v;
    }

    /**
     * Marching squares "pieno": per ogni cella si emette il POLIGONO della
     * parte interna al contorno, non il segmento di bordo. Cosi' non serve
     * inseguire i contorni per chiuderli in anelli - cosa che con due masse
     * che si fondono e si staccano vorrebbe dire gestire i cambi di topologia
     * a ogni fotogramma - e le celle tutte piene diventano rettangoli, che si
     * accorpano in strisce.
     *
     * I pezzi finiscono in UNA sola path (fillPolygons): i bordi in comune fra
     * due pezzi adiacenti si annullano invece di lasciare una cucitura chiara.
     */
    void contorni() {
        verts_.clear();
        lens_.clear();
        if (balls_.empty()) return;

        // La griglia copre solo dove il campo puo' arrivare a 1: il riquadro
        // delle sorgenti, allargato di un raggio e mezzo per i colli.
        float x0 = balls_[0].x, y0 = balls_[0].y, x1 = x0, y1 = y0;
        for (const auto& b : balls_) {
            x0 = std::min(x0, b.x - b.r * 1.5f);
            y0 = std::min(y0, b.y - b.r * 1.5f);
            x1 = std::max(x1, b.x + b.r * 1.5f);
            y1 = std::max(y1, b.y + b.r * 1.5f);
        }
        const int gw = static_cast<int>((x1 - x0) / kCella) + 2;
        const int gh = static_cast<int>((y1 - y0) / kCella) + 2;
        if (gw < 2 || gh < 2 || gw > 4000 || gh > 4000) return;

        f_.assign(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), 0.0f);
        for (int j = 0; j < gh; ++j) {
            for (int i = 0; i < gw; ++i) {
                f_[static_cast<std::size_t>(j) * gw + i] =
                    campo(x0 + i * kCella, y0 + j * kCella);
            }
        }

        const auto vx = [&](int i) { return x0 + i * kCella; };
        const auto vy = [&](int j) { return y0 + j * kCella; };
        const auto at = [&](int i, int j) { return f_[static_cast<std::size_t>(j) * gw + i]; };

        for (int j = 0; j + 1 < gh; ++j) {
            int runInizio = -1;   // striscia di celle tutte piene, da chiudere in un rettangolo
            for (int i = 0; i + 1 < gw; ++i) {
                const float fv[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
                const int mask = (fv[0] >= 1.0f ? 1 : 0) | (fv[1] >= 1.0f ? 2 : 0) |
                                 (fv[2] >= 1.0f ? 4 : 0) | (fv[3] >= 1.0f ? 8 : 0);
                if (mask == 15) {
                    if (runInizio < 0) runInizio = i;
                    continue;
                }
                if (runInizio >= 0) {
                    emettiRettangolo(vx(runInizio), vy(j), vx(i), vy(j + 1));
                    runInizio = -1;
                }
                if (mask == 0) continue;

                // Giro dei quattro spigoli in senso orario: si tiene lo spigolo
                // se e' dentro, e si aggiunge il punto sul lato ogni volta che
                // il contorno lo attraversa.
                const float cxs[4] = {vx(i), vx(i + 1), vx(i + 1), vx(i)};
                const float cys[4] = {vy(j), vy(j), vy(j + 1), vy(j + 1)};
                const int   partenza = static_cast<int>(verts_.size());
                for (int k = 0; k < 4; ++k) {
                    const int kk = (k + 1) & 3;
                    const bool a = fv[k] >= 1.0f, bnext = fv[kk] >= 1.0f;
                    if (a) verts_.push_back({cxs[k], cys[k]});
                    if (a != bnext) {
                        const float den = fv[kk] - fv[k];
                        const float tt  = den != 0.0f ? (1.0f - fv[k]) / den : 0.5f;
                        verts_.push_back({cxs[k] + (cxs[kk] - cxs[k]) * tt,
                                          cys[k] + (cys[kk] - cys[k]) * tt});
                    }
                }
                lens_.push_back(static_cast<int>(verts_.size()) - partenza);
            }
            if (runInizio >= 0) emettiRettangolo(vx(runInizio), vy(j), vx(gw - 1), vy(j + 1));
        }
    }

    void emettiRettangolo(float l, float t, float rr, float b) {
        verts_.push_back({l, t});
        verts_.push_back({rr, t});
        verts_.push_back({rr, b});
        verts_.push_back({l, b});
        lens_.push_back(4);
    }

    std::vector<MetaBall>      balls_;
    std::vector<float>         f_;
    std::vector<render::Point> verts_;
    std::vector<int>           lens_;
};

CausticSwarm  gBreathSwarm;
CausticSwarm  gFocusSwarm;
MetaballField gLandingBlobs;

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

    constexpr render::Color kBreathBackdrop{0.0f, 0.0f, 0.0f, 0.06f};
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
    constexpr render::Color kTargetRing{0.0f, 0.0f, 0.0f, 0.10f};
    constexpr float kRingThickness = 44.0f;
    const auto near = static_cast<float>(util::smoothstep01((frac - 0.75f) / 0.25f));
    // "Ci sei" = il magenta della concentrazione (kOk ora e' il nero degli
    // stati normali, non un colore di conferma).
    const render::Color ring{kTargetRing.r + (kAccent.r - kTargetRing.r) * near,
                             kTargetRing.g + (kAccent.g - kTargetRing.g) * near,
                             kTargetRing.b + (kAccent.b - kTargetRing.b) * near,
                             kTargetRing.a + (1.0f - kTargetRing.a) * near};

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

// ---------------------------------------------------------------------------
// La macchia e il lettering: i due artefatti dell'identita' Biodetails (vedi
// design/biodetails/IDENTITA-VISIVA.md, paragrafi 4 e 5). Il lettering e'
// corallo sul fondo e diventa arancio ESATTAMENTE dove attraversa la forma
// magenta: nel poster e' fatto con due copie del titolo, la seconda
// ritagliata con il tracciato della macchia, ed e' fatto cosi' anche qui.
// ---------------------------------------------------------------------------

/** I punti della macchia (biodetails_blob.hpp) mappati su `box`, nel formato di fillCubicPath. */
std::vector<render::Point> blobPoints(render::Rect box) {
    std::vector<render::Point> pts;
    pts.reserve(1 + 3 * biodetails::kBlobSegments);
    const auto map = [&](float u, float v) {
        return render::Point{box.left + u * box.width(), box.top + v * box.height()};
    };
    pts.push_back(map(biodetails::kBlobStart[0], biodetails::kBlobStart[1]));
    for (const auto& seg : biodetails::kBlob) {
        pts.push_back(map(seg[0], seg[1]));
        pts.push_back(map(seg[2], seg[3]));
        pts.push_back(map(seg[4], seg[5]));
    }
    return pts;
}

// League Gothic: maiuscole alte 0.735 del corpo, ascendente 0.9675, riga 1.2.
// Servono a piazzare il lettering per l'altezza delle MAIUSCOLE, che e' la
// misura con cui il poster e' costruito, e non per il riquadro del testo.
constexpr float kDisplayCapEm    = 0.735f;
constexpr float kDisplayAscentEm = 0.9675f;
constexpr float kDisplayLineEm   = 1.30f;   // 1.2 piu' un margine: sotto la riga naturale drawText non disegna

/** Riquadro per drawTextDisplay tale che le maiuscole partano da `capTop`, largo quanto la finestra. */
render::Rect displayBox(render::Size win, float capTop, float fontSize) {
    const float top = capTop - (kDisplayAscentEm - kDisplayCapEm) * fontSize;
    return render::rect(0.0f, top, win.width, top + fontSize * kDisplayLineEm);
}

/** Il dispositivo del poster: forma magenta, lettering corallo sopra, arancio dentro la forma. */
void drawLetteringOverShape(render::Renderer& r, const std::wstring& text, render::Rect box,
                            float fontSize, const std::vector<render::Point>& shape) {
    const int n = static_cast<int>(shape.size());
    r.fillCubicPath(shape.data(), n, kAccent);
    r.drawTextDisplay(text, box, fontSize, kCoral);
    r.pushClipCubicPath(shape.data(), n);
    r.drawTextDisplay(text, box, fontSize, kAccent2);
    r.popClip();
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

    // La macchia del poster, al centro dello schermo e viva: e' lei adesso a
    // raccontare cosa succede quando la fascia comincia a leggere. Lobi
    // separati che vagano = segnale che ancora non c'e'; lobi che si
    // avvicinano, si toccano e si fondono in una massa sola = segnale
    // trovato. Il passaggio fra i due stati non e' un taglio: lo fa la forma
    // del campo (vedi MetaballField), come due gocce che si uniscono.
    gLandingBlobs.update(render::rect(0, 0, win.width, win.height), animT, gather);
    gLandingBlobs.fill(r, kAccent);

    // --- lettering, e il dispositivo del poster ---
    // Il titolo e' corallo, e diventa arancio dove la macchia gli passa
    // sotto: e' la firma dell'identita' (par. 4.2 delle linee guida). Qui la
    // macchia si muove, quindi il cambio di colore va e viene da solo quando
    // un lobo sale fin sotto le lettere.
    //
    // C e' l'altezza delle maiuscole: nel poster vale il 17.8% dell'altezza
    // del foglio, qui il 16% della finestra, perche' lo schermo e' largo e
    // sotto deve starci il resto della pagina.
    const float C        = win.height * 0.16f;
    const float titoloPt = C / kDisplayCapEm;
    const render::Rect titoloBox = displayBox(win, win.height * 0.07f, titoloPt);
    r.drawTextDisplay(L"MIND ZOOM", titoloBox, titoloPt, kCoral);
    gLandingBlobs.pushClip(r);
    r.drawTextDisplay(L"MIND ZOOM", titoloBox, titoloPt, kAccent2);
    r.popClip();
    // Le ultime due righe spiegano il meccanismo (cosa succede concentrandosi
    // o rilassandosi) e danno un compito concreto ("un dettaglio della foto"),
    // non solo l'esistenza di un legame fra attenzione e zoom: e' la lacuna
    // segnalata dai test con altre persone, che non sapevano su cosa
    // concentrarsi ne' come.
    r.drawTextBody(L"Una fotografia al microscopio elettronico.\n"
                   L"Una fascia legge la tua attività cerebrale e guida lo zoom:\n"
                   L"concentrati per avvicinarti, rilassati per allontanarti.",
                  render::rect(cx - 420.0f, cy - 120.0f, cx + 420.0f, cy + 40.0f), 19.0f, kInk);

    // Il tasto va detto a parte, sotto, invece che dentro l'etichetta: il
    // pulsante si puo' anche cliccare (vedi handleClick), ma in mostra la
    // tastiera resta la via principale e va nominata.
    drawEntryButton(r, entryButtonRect(win, kLandingButtonDy), L"Inizia l’esperienza", animT);
    r.drawTextBody(L"Premi INVIO o tocca il pulsante",
                   render::rect(cx - 240.0f, cy + 318.0f, cx + 240.0f, cy + 338.0f),
                   13.0f, {kMuted.r, kMuted.g, kMuted.b, 0.75f});

    // Stato della fascia: qui e' un'informazione utile prima di cominciare, non
    // un dettaglio diagnostico - se non e' collegata conviene saperlo adesso,
    // non a calibrazione avviata. Il pubblico la legge: niente tasti, niente
    // gergo; i tasti stanno nel pannello operatore (H).
    const wchar_t* stato =
        st.replaying                      ? L"Sessione dimostrativa · segnale registrato"
        : st.bleState == 3                ? L"Fascia EEG collegata · pronta"
        : st.bleState == 0                ? L"Fascia EEG non collegata"
                                          : L"Ricerca della fascia EEG in corso…";
    drawStatusHint(r, render::rect(cx - 400.0f, cy + 346.0f, cx + 400.0f, cy + 374.0f), stato);
}

// ---------------------------------------------------------------------------
// Accoglienza e congedo: il giro completo di un visitatore, senza nessuno
// accanto a spiegare. Le due schermate stanno agli estremi dell'esperienza -
// una prepara (indossa, accendi, respira), l'altra lascia la postazione
// pronta per chi viene dopo - e sono l'unico punto in cui l'installazione
// parla a chi non ha letto niente.
// ---------------------------------------------------------------------------

// --- scala tipografica e ritmo verticale dell'accoglienza -------------------
// Corpi e distanze su una scala di 4, dichiarati qui una volta sola: l'altezza
// totale si SOMMA da queste e il blocco viene centrato in verticale, cosi'
// l'impaginato resta bilanciato su qualunque finestra invece di dipendere da
// scostamenti dal centro aggiustati a mano uno per uno. Stanno fuori dalla
// funzione di disegno perche' serve anche a onboardButtonRect(), che deve
// sapere dove e' finito il pulsante senza disegnare niente.
constexpr float kOnbTitoloPt = 68.0f, kOnbPassoPt = 19.0f, kOnbEtichettaPt = 14.0f;
constexpr float kOnbParagrafoPt = 17.0f, kOnbNotaPt = 13.0f;
constexpr float kOnbInterlinea = 1.5f;
constexpr float kOnbRigaPasso  = 48.0f;    // passo fra un punto e il successivo
constexpr float kOnbBadgeR = 15.0f, kOnbBadgeGap = 20.0f;

// 1.6 e non 1.3: il titolo e' nel serif d'accento (Iowan Old Style), che ha
// ascendenti e discendenti piu' generose del sans, e drawText NON disegna
// affatto una riga che non ci sta nel riquadro - a 1.3 il titolo spariva senza
// dire niente, che e' il modo peggiore di sbagliare una misura.
constexpr float kOnbTitoloH        = kOnbTitoloPt * 1.30f;   // lettering: riga 1.2 em piu' margine
constexpr float kOnbDopoTitolo     = 44.0f;
constexpr float kOnbPassiH         = kOnbRigaPasso * 3.0f;
constexpr float kOnbDopoPassi      = 40.0f;
constexpr float kOnbEtichettaH     = kOnbEtichettaPt * 1.60f;
constexpr float kOnbDopoEtichetta  = 10.0f;
constexpr float kOnbParagrafoH     = kOnbParagrafoPt * kOnbInterlinea * 3.0f;   // tre righe
constexpr float kOnbDopoParagrafo  = 48.0f;
constexpr float kOnbDopoPulsante   = 14.0f;
constexpr float kOnbNotaH          = kOnbNotaPt * 1.60f;
constexpr float kOnbDopoSuggerimento = 12.0f;

constexpr float kOnbTotale = kOnbTitoloH + kOnbDopoTitolo + kOnbPassiH + kOnbDopoPassi +
                             kOnbEtichettaH + kOnbDopoEtichetta + kOnbParagrafoH +
                             kOnbDopoParagrafo + kEntryButtonH + kOnbDopoPulsante + kOnbNotaH +
                             kOnbDopoSuggerimento + kOnbNotaH;

/** Dove finisce il pulsante dell'accoglienza, dato il ritmo qui sopra. */
render::Rect onboardButtonRect(render::Size win) {
    const float cx  = win.width * 0.5f;
    const float top = win.height * 0.5f - kOnbTotale * 0.5f + kOnbTitoloH + kOnbDopoTitolo +
                      kOnbPassiH + kOnbDopoPassi + kOnbEtichettaH + kOnbDopoEtichetta +
                      kOnbParagrafoH + kOnbDopoParagrafo;
    return render::rect(cx - kEntryButtonW * 0.5f, top, cx + kEntryButtonW * 0.5f,
                        top + kEntryButtonH);
}

/** Il pulsante dell'accoglienza accetta INVIO/clic solo da qui in poi. */
bool onboardReady(double elapsed) noexcept { return elapsed >= config::kOnboardReadyS; }

/**
 * Opacita' del pulsante dell'accoglienza: spento finche' non e' il momento,
 * poi dissolvenza con smoothstep - una rampa lineare "parte" e "arriva" con
 * uno scatto percettibile, questa no.
 */
float onboardButtonOpacity(double elapsed) noexcept {
    constexpr float kSpento = 0.24f;
    const double dopo = elapsed - config::kOnboardReadyS;
    if (dopo <= 0.0) return kSpento;
    const auto t = static_cast<float>(std::clamp(dopo / config::kOnboardFadeS, 0.0, 1.0));
    return kSpento + (1.0f - kSpento) * (t * t * (3.0f - 2.0f * t));
}

/**
 * Accoglienza: cosa fare prima di cominciare, e perche' conviene arrivare
 * all'esperienza con la testa sgombra.
 *
 * Il respiro e' SPIEGATO, non guidato: niente cerchio che pulsa ne' conto
 * alla rovescia. Un esercizio a tempo diventa un compito da eseguire bene -
 * si guarda l'animazione invece di respirare - ed e' l'opposto di quello che
 * serve qui. Detto a parole, ognuno lo fa col proprio ritmo o non lo fa
 * affatto, e in nessuno dei due casi resta bloccato a guardare uno schermo.
 *
 * Il pulsante resta spento per i primi secondi APPOSTA (config::kOnboardReadyS):
 * e' l'unica cosa che impedisce di attraversare la schermata senza leggerla, e
 * intanto la banda adattiva si scalda - questa schermata ha preso il posto del
 * vecchio velo di "ascolto del segnale", quindi quel tempo va speso qui.
 *
 * Tipografia: il serif d'accento e' solo del titolo, come nella pagina
 * d'ingresso; tutto il resto e' il sans del corpo, e la gerarchia la fanno
 * corpo e opacita', non un terzo carattere.
 */
void drawOnboarding(render::Renderer& r, double elapsed, double animT) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), kBg);

    static const wchar_t* const kPassi[] = {
        L"Indossa la fascia sulla fronte, sopra le sopracciglia",
        L"Accendila con il tasto sul lato: la luce si accende",
        L"Prenditi un momento per respirare, prima di entrare",
    };
    static const wchar_t* const kParagrafo =
        L"Quattro tempi uguali: inspira, trattieni, espira, trattieni.\n"
        L"Bastano due o tre giri perché il respiro si distenda\n"
        L"e il segnale che guida l’immagine diventi più nitido.";

    float y = win.height * 0.5f - kOnbTotale * 0.5f;

    // --- titolo ---
    r.drawTextDisplay(L"PRIMA DI COMINCIARE",
                      render::rect(cx - 460.0f, y, cx + 460.0f, y + kOnbTitoloH),
                      kOnbTitoloPt, kCoral);
    y += kOnbTitoloH + kOnbDopoTitolo;

    // --- i tre passaggi ---
    // Restano accesi tutti insieme: non sono una procedura da spuntare uno
    // alla volta, sono le tre cose da fare adesso.
    //
    // Il blocco (pallino + riga piu' lunga) viene CENTRATO sull'asse: un
    // elenco allineato a sinistra piazzato "a occhio" resta sempre spostato di
    // qualche decina di pixel, perche' le righe non hanno tutte la stessa
    // lunghezza. Misurando, il centro ottico e' quello vero.
    float larghezzaTesto = 0.0f;
    for (const wchar_t* passo : kPassi) {
        larghezzaTesto = std::max(larghezzaTesto, r.measureTextBody(passo, kOnbPassoPt).width);
    }
    const float altezzaRiga = r.measureTextBody(L"Hg", kOnbPassoPt).height;
    const float bloccoW     = kOnbBadgeR * 2.0f + kOnbBadgeGap + larghezzaTesto;
    const float bloccoX     = cx - bloccoW * 0.5f;

    constexpr render::Color kPasso = kInk;
    for (int i = 0; i < 3; ++i) {
        const float centro = y + kOnbRigaPasso * 0.5f + kOnbRigaPasso * static_cast<float>(i);
        drawStepBadge(r, {bloccoX + kOnbBadgeR, centro}, kOnbBadgeR, i + 1, true, false, kAccent);
        r.drawTextBody(kPassi[static_cast<std::size_t>(i)],
                      render::rect(bloccoX + kOnbBadgeR * 2.0f + kOnbBadgeGap,
                                  centro - altezzaRiga * 0.5f,
                                  bloccoX + bloccoW, centro + altezzaRiga * 0.5f + 2.0f),
                      kOnbPassoPt, kPasso, render::TextAlign::Left);
    }
    y += kOnbPassiH + kOnbDopoPassi;

    // --- il respiro quadrato, spiegato e basta ---
    // La riga d'etichetta e' l'unico grassetto della sezione: dice "qui
    // comincia un'altra cosa" senza bisogno di una cornice attorno.
    r.drawTextBody(L"IL RESPIRO QUADRATO",
                  render::rect(cx - 400.0f, y, cx + 400.0f, y + kOnbEtichettaH),
                  kOnbEtichettaPt, kAccent2, render::TextAlign::Center, true);
    y += kOnbEtichettaH + kOnbDopoEtichetta;

    r.drawParagraph(kParagrafo,
                    render::rect(cx - 440.0f, y, cx + 440.0f, y + kOnbParagrafoH + 8.0f),
                    kOnbParagrafoPt, kInk, render::TextAlign::Center, kOnbInterlinea);
    y += kOnbParagrafoH + kOnbDopoParagrafo;

    // --- pulsante e chiuse ---
    const bool  pronto  = onboardReady(elapsed);
    const float opacita = onboardButtonOpacity(elapsed);
    drawEntryButton(r, onboardButtonRect(win), L"Entra nell’esperienza", animT, opacita);
    y += kEntryButtonH + kOnbDopoPulsante;

    r.drawTextBody(pronto ? L"Premi INVIO o tocca il pulsante" : L"Fra poco…",
                  render::rect(cx - 260.0f, y, cx + 260.0f, y + kOnbNotaH), kOnbNotaPt,
                  {kMuted.r, kMuted.g, kMuted.b, 0.30f + 0.55f * opacita});
    y += kOnbNotaH + kOnbDopoSuggerimento;

    // Detto qui e non solo alla fine: quando l'esperienza sara' finita la
    // persona avra' la fascia in testa e nessuna voglia di leggere - se il
    // gesto di chiusura non lo ha gia' sentito una volta, non lo fa.
    r.drawTextBody(L"A esperienza finita, tieni premuto R per lasciarla pronta al prossimo",
                  render::rect(cx - 420.0f, y, cx + 420.0f, y + kOnbNotaH), kOnbNotaPt,
                  {kMuted.r, kMuted.g, kMuted.b, 0.55f});
}

/**
 * Promemoria del riavvio: piccolo, in basso a destra, stile HUD.
 *
 * Compare solo a esperienza avviata da un po' (config::kRestartHintAfterS) -
 * prima sarebbe un invito a interrompere qualcosa appena cominciato - oppure
 * subito, se qualcuno sta gia' premendo R: in quel caso l'anello e' l'unico
 * modo per sapere che il gesto sta funzionando e quanto manca.
 *
 * `hold` in [0,1] e' l'avanzamento della pressione. A 0 si vede solo il tasto
 * disegnato; l'anello - traccia compresa - esiste solo mentre si preme, cosi'
 * a riposo il promemoria e' una riga sola e non un widget che gira a vuoto.
 */
void drawRestartHint(render::Renderer& r, double hold, double animT) {
    const auto win = r.size();

    const bool  attivo = hold > 0.0;
    const float w = 230.0f, h = 46.0f;
    const render::Rect box = render::rect(win.width - 24.0f - w, win.height - 24.0f - h,
                                          win.width - 24.0f, win.height - 24.0f);

    // Un respiro lentissimo sull'opacita' a riposo: abbastanza da farsi notare
    // con la coda dell'occhio senza mai chiedere attenzione, che e' quello che
    // deve fare un promemoria mentre qualcuno sta guardando una fotografia.
    const auto  pulse = static_cast<float>(0.5 + 0.5 * std::sin(animT * 1.1));
    // Pillola acqua piatta sopra la foto: il respiro sta sull'opacita' della
    // campitura, che a riposo lascia intravedere la foto sotto.
    const float fondo = attivo ? 1.0f : 0.80f + 0.08f * pulse;
    r.fillRect(box, {kBg.r, kBg.g, kBg.b, fondo}, h * 0.5f);

    const render::Point tasto{box.left + 28.0f, (box.top + box.bottom) * 0.5f};
    constexpr float kTastoR = 12.5f;
    r.fillCircle(tasto, kTastoR, {0.0f, 0.0f, 0.0f, attivo ? 0.14f : 0.08f});
    // Sans come le cifre dei badge: e' un tasto disegnato, cioe' chrome di
    // servizio, e il serif d'accento qui resta al titolo delle schermate.
    r.drawTextBodyCentered(L"R", tasto, 13.5f,
                           attivo ? kInk : render::Color{kInk.r, kInk.g, kInk.b, 0.80f}, true);

    if (attivo) {
        // Traccia + avanzamento, entrambi solo mentre si preme: a riposo il
        // promemoria e' una riga sola, non un widget che gira a vuoto. Parte
        // dalle 12 e gira in senso orario - -90 gradi e' l'alto anche qui,
        // dove la y cresce verso il basso (vedi Renderer::drawArc).
        //
        // Nero e non un caldo: i caldi sono i colori delle fasi nella legge di
        // controllo, e qui non si sta misurando niente - e' un conto alla
        // rovescia meccanico, che non deve sembrare un ritorno del segnale.
        constexpr float kAnelloR = kTastoR + 4.5f;
        r.drawArc(tasto, kAnelloR, -90.0f, 270.0f, 2.5f, kHairline);
        const auto frazione = static_cast<float>(std::clamp(hold, 0.0, 1.0));
        if (frazione > 0.0f) {
            r.drawArc(tasto, kAnelloR, -90.0f, -90.0f + 360.0f * frazione, 2.5f, kInk);
        }
    }

    // Allineato a sinistra ma centrato in verticale sulla pillola: drawTextBody
    // ancora al TOP del riquadro, quindi il riquadro va posizionato sull'altezza
    // misurata della riga invece che su un mezzo-corpo stimato - a 11.5pt uno
    // scarto di due pixel dentro una pillola alta 46 si vede.
    const std::wstring etichetta = attivo ? L"Continua a tenere premuto"
                                          : L"Tieni premuto per ricominciare";
    constexpr float kEtichettaPt = 11.5f;
    const float rigaH = r.measureTextBody(etichetta, kEtichettaPt).height;
    const float meta  = (box.top + box.bottom) * 0.5f;
    r.drawTextBody(etichetta,
                  render::rect(box.left + 50.0f, meta - rigaH * 0.5f,
                              box.right - 12.0f, meta + rigaH * 0.5f + 2.0f),
                  kEtichettaPt, attivo ? kInk : kMuted, render::TextAlign::Left);
}

/**
 * Congedo: la postazione va lasciata pronta, e questa e' l'unica occasione
 * per dirlo. Passa da sola dopo config::kOffboardS - a fine esperienza le
 * mani sono occupate dalla fascia e chiedere un altro tasto vorrebbe dire
 * che qualcuno resta a guardare uno schermo fermo.
 */
void drawOffboarding(render::Renderer& r, double elapsed, double animT) {
    const auto  win = r.size();
    const float cx  = win.width * 0.5f;
    const float cy  = win.height * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), kBg);

    // Stesso ritmo dell'accoglienza: altezze e distanze dichiarate, totale
    // sommato, blocco centrato. Qui il contenuto e' poco, e proprio per questo
    // un centraggio approssimato si vedrebbe di piu'.
    // Il lettering si misura sull'altezza delle MAIUSCOLE, come nel poster: qui
    // il 13% dell'altezza della finestra (la pagina d'ingresso sta al 16%,
    // questa e' una schermata piu' quieta e con meno da dire).
    const float     kTitoloPt   = win.height * 0.13f / kDisplayCapEm;
    constexpr float kCorpoPt    = 20.0f;
    constexpr float kInterlinea = 1.5f;
    const float     kTitoloH    = kTitoloPt * kDisplayLineEm;
    constexpr float kDopoTitolo = 34.0f;
    constexpr float kCorpoH = kCorpoPt * kInterlinea * 2.0f;   // due righe
    constexpr float kDopoCorpo = 52.0f;
    constexpr float kBarH = 4.0f;
    const float kTotale = kTitoloH + kDopoTitolo + kCorpoH + kDopoCorpo + kBarH;

    float y = cy - kTotale * 0.5f;

    // La macchia FERMA dietro al lettering, nelle proporzioni esatte del
    // poster, al posto del campo animato della pagina d'ingresso: qui non
    // c'e' piu' niente da leggere nel segnale, e una forma che si agita
    // mentre si dice "abbiamo finito" direbbe il contrario. Respira appena,
    // ed e' agganciata al titolo (non allo schermo) cosi' non scivola
    // rispetto a lui.
    const auto  respiro = static_cast<float>(0.5 + 0.5 * std::sin(animT * 0.7));
    const float cap     = kTitoloPt * kDisplayCapEm;
    const float capTop  = y + (kDisplayAscentEm - kDisplayCapEm) * kTitoloPt;
    // Nel poster la macchia si misura sull'altezza delle MAIUSCOLE, non sulla
    // larghezza della parola: e' alta 1.97 C, larga di conseguenza, e le
    // lettere la attraversano a meta'. Misurandola sulla parola, "GRAZIE" -
    // che e' corta - la faceva diventare un ornamento con due lobi che
    // spuntavano sopra le lettere come orecchie.
    const float blobH   = cap * (1.97f + 0.04f * respiro);
    const float blobW   = blobH * biodetails::kBlobAspect;
    const float blobCx  = cx - 0.06f * cap;
    const float blobTop = capTop - 0.455f * cap;
    drawLetteringOverShape(r, L"GRAZIE", render::rect(cx - 460.0f, y, cx + 460.0f, y + kTitoloH),
                           kTitoloPt,
                           blobPoints(render::rect(blobCx - blobW * 0.5f, blobTop,
                                                   blobCx + blobW * 0.5f, blobTop + blobH)));
    y += kTitoloH + kDopoTitolo;

    r.drawParagraph(L"Togli la fascia e igienizzala con una salvietta.\n"
                    L"Il prossimo visitatore la troverà pronta.",
                    render::rect(cx - 420.0f, y, cx + 420.0f, y + kCorpoH + 8.0f),
                    kCorpoPt, kInk, render::TextAlign::Center, kInterlinea);
    y += kCorpoH + kDopoCorpo;

    // Barra che si svuota: dice che lo schermo tornera' da solo, cosi' nessuno
    // resta li' a chiedersi se deve fare qualcosa.
    const auto  rimasto = static_cast<float>(
        std::clamp(1.0 - elapsed / config::kOffboardS, 0.0, 1.0));
    constexpr float kBarW = 260.0f;
    r.fillRect(render::rect(cx - kBarW / 2, y, cx + kBarW / 2, y + kBarH), kHairline,
              kBarH * 0.5f);
    if (rimasto > 0.0f) {
        r.fillRect(render::rect(cx - kBarW / 2, y, cx - kBarW / 2 + kBarW * rimasto, y + kBarH),
                  kAccent2, kBarH * 0.5f);
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
        r.drawTextDisplay(inConcentrate ? L"CONCENTRAZIONE" : L"RILASSAMENTO",
                          render::rect(cx - 320.0f, win.height * 0.5f - 250.0f,
                                      cx + 320.0f, win.height * 0.5f - 186.0f),
                          48.0f, stageColor);
        r.drawTextBody(inConcentrate
                           ? L"Fai crescere la sfera fino al bordo:\n"
                             L"conta all’indietro da 300, sottraendo 7 ogni volta."
                           : L"Respira seguendo la sfera.",
                      render::rect(cx - 340.0f, win.height * 0.5f + 178.0f,
                                  cx + 340.0f, win.height * 0.5f + 238.0f),
                      16.0f, kInk);
        // Unica eccezione: se il segnale non e' utilizzabile la persona deve
        // saperlo, altrimenti resta li' a sforzarsi mentre nulla viene
        // contato. Sta in basso, lontano dall'elemento.
        const render::Rect avviso =
            render::rect(cx - 320.0f, win.height - 78.0f, cx + 320.0f, win.height - 46.0f);
        if (!st.signalFresh) {
            drawStatusHint(r, avviso,
                          L"In attesa del segnale dalla fascia EEG: il conteggio è in pausa.");
        } else if (st.signalFault != 0) {
            drawStatusHint(r, avviso,
                          L"Segnale non utilizzabile: questi istanti non vengono conteggiati.");
        } else if (!st.contactOk) {
            drawStatusHint(r, avviso, L"Contatto assente: sistema la fascia sulla fronte.");
        }
        return;
    }

    const float panelW = 580.0f;
    const float panelH = 600.0f;
    const render::Rect panel = render::rect(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    // Nessun riquadro: il poster non ha cornici, la scheda e' un blocco di
    // testo sul fondo. `panel` resta come gabbia di impaginazione.
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
        r.drawLine({b1.x + 16.0f, b1.y}, {b2.x - 16.0f, b2.y}, kHairline, 2.0f);
        drawStepBadge(r, b1, 16.0f, 1, stage == control::CalibStage::Intro || inConcentrate,
                     pastFirst, kAccent);
        drawStepBadge(r, b2, 16.0f, 2, pastFirst, false, kAccent2);
    }

    if (stage == control::CalibStage::Intro) {
        r.drawTextDisplay(L"CALIBRAZIONE", render::rect(panel.left, top + 30, panel.right, top + 96),
                          50.0f, kCoral);
        r.drawTextBody(L"Due passaggi brevi: la durata si adatta al tuo segnale.",
                      render::rect(panel.left + 40, top + 100, panel.right - 40, top + 128), 16.0f,
                      kMuted);

        const float rowY = top + 170.0f;
        const render::Point p1{cx - 150.0f, rowY};
        const render::Point p2{cx + 150.0f, rowY};
        drawStepBadge(r, p1, 26.0f, 1, true, false, kAccent);
        drawStepBadge(r, p2, 26.0f, 2, true, false, kAccent2);
        r.drawTextBody(L"Concentrazione\nfai crescere la sfera",
                      render::rect(p1.x - 110.0f, p1.y + 38.0f, p1.x + 110.0f, p1.y + 90.0f), 15.0f,
                      kInk, render::TextAlign::Center);
        r.drawTextBody(L"Rilassamento\nrespira con la sfera",
                      render::rect(p2.x - 110.0f, p2.y + 38.0f, p2.x + 110.0f, p2.y + 90.0f), 15.0f,
                      kInk, render::TextAlign::Center);
        r.drawLine({p1.x + 34.0f, p1.y}, {p2.x - 34.0f, p2.y}, kHairline, 2.0f);

        r.drawTextBody(L"Termina automaticamente quando le misure sono sufficienti.",
                      render::rect(panel.left + 48, rowY + 110.0f, panel.right - 48, rowY + 160.0f),
                      14.5f, kMuted);

        const render::Rect cta =
            render::rect(cx - 140.0f, panel.bottom - 118.0f, cx + 140.0f, panel.bottom - 62.0f);
        drawPillButton(r, cta, L"Premi INVIO per iniziare", kAccent);

        drawStatusHint(r, render::rect(panel.left, panel.bottom - 46, panel.right, panel.bottom - 22),
                      st.replaying ? L"Sessione dimostrativa · segnale registrato"
                                  : L"La fascia EEG deve essere collegata");
        if (!st.signalFresh && !st.replaying) {
            r.drawTextBody(L"Nessun dato dalla fascia: puoi iniziare comunque,\n"
                           L"il conteggio partirà all’arrivo del segnale.",
                          render::rect(panel.left + 32, rowY + 160.0f, panel.right - 32,
                                      rowY + 205.0f),
                          13.5f, kWarn);
            r.drawTextBody(L"Q per uscire   ·   D per procedere con segnale simulato",
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
        r.drawTextDisplay(L"SECONDO PASSAGGIO",
                          render::rect(panel.left, top + 30, panel.right, top + 96),
                          50.0f, kCoral);
        r.drawTextBody(L"La fase di concentrazione è conclusa. Ora si misura l’opposto.",
                      render::rect(panel.left + 40, top + 100, panel.right - 40, top + 128), 16.0f,
                      kMuted);

        const float rowY = top + 190.0f;
        drawStepBadge(r, {cx, rowY}, 34.0f, 2, true, false, kAccent2);
        r.drawTextDisplay(L"RILASSAMENTO",
                          render::rect(panel.left, rowY + 50.0f, panel.right, rowY + 100.0f),
                          36.0f, kAccent2);
        r.drawTextBody(L"Segui la sfera che respira: inspira, trattieni, espira, trattieni —\n"
                       L"quattro tempi da 4 secondi ciascuno.\n"
                       L"Lascia andare lo sforzo di prima.",
                      render::rect(panel.left + 40, rowY + 104.0f, panel.right - 40, rowY + 180.0f),
                      15.5f, kMuted);

        // Barra di avanzamento: e' l'unica cosa che sostituisce il pulsante.
        const float barW = 280.0f, barH = 6.0f;
        const float barY = panel.bottom - 108.0f;
        r.fillRect(render::rect(cx - barW / 2, barY, cx + barW / 2, barY + barH), kHairline,
                  barH * 0.5f);
        const auto avanz = static_cast<float>(std::clamp(st.calibProgress, 0.0, 1.0));
        if (avanz > 0.0f) {
            r.fillRect(render::rect(cx - barW / 2, barY, cx - barW / 2 + barW * avanz, barY + barH),
                      kAccent2, barH * 0.5f);
        }
        drawStatusHint(r, render::rect(panel.left, barY + 22.0f, panel.right, barY + 46.0f),
                      L"Parte automaticamente.");
        return;
    }

    if (stage == control::CalibStage::Done) {
        const render::Point ic{cx, win.height * 0.5f - 96.0f};
        const float pulse = 3.0f + 2.0f * static_cast<float>(std::sin(animT * 2.0));
        r.fillCircle(ic, 42.0f + pulse, {kAccent.r, kAccent.g, kAccent.b, 0.18f});
        r.fillCircle(ic, 34.0f, kAccent);
        r.drawLine({ic.x - 15.0f, ic.y + 1.0f}, {ic.x - 4.0f, ic.y + 13.0f}, kInk, 4.0f);
        r.drawLine({ic.x - 4.0f, ic.y + 13.0f}, {ic.x + 17.0f, ic.y - 12.0f}, kInk, 4.0f);

        r.drawTextDisplay(st.calibUsingFallback ? L"PRONTO · PROFILO GENERICO"
                                                : L"CALIBRAZIONE COMPLETATA",
                          render::rect(panel.left, win.height * 0.5f - 36, panel.right,
                                      win.height * 0.5f + 20),
                          42.0f, kCoral);
        r.drawTextBody(st.calibUsingFallback
                           ? L"Concentrandoti avvicini l’immagine, rilassandoti la allontani —\n"
                             L"ma il profilo non è il tuo: è un valore generico,\n"
                             L"che permette comunque di provare l’esperienza."
                           : L"Concentrandoti avvicini l’immagine, rilassandoti la allontani.\n"
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
                  L"Controlla che la fascia sia posizionata correttamente."
            : reason == app::FailReason::ImplausibleSignal
                ? L"Il segnale ricevuto non è un’onda cerebrale.\n"
                  L"Spegni e riaccendi la fascia, poi riprova.\n"
                  L"Se il problema persiste, dipende dal sistema: non da te."
                : L"Variazione troppo debole: marca di più la differenza\n"
                  L"tra concentrazione e rilassamento.";

        const render::Point ic{cx, win.height * 0.5f - 130.0f};
        r.fillCircle(ic, 34.0f, {kBad.r, kBad.g, kBad.b, 0.16f});
        r.drawLine({ic.x - 12.0f, ic.y - 12.0f}, {ic.x + 12.0f, ic.y + 12.0f}, kBad, 4.0f);
        r.drawLine({ic.x + 12.0f, ic.y - 12.0f}, {ic.x - 12.0f, ic.y + 12.0f}, kBad, 4.0f);

        r.drawTextDisplay(L"CALIBRAZIONE NON RIUSCITA",
                          render::rect(panel.left, win.height * 0.5f - 82, panel.right,
                                      win.height * 0.5f - 28),
                          40.0f, kCoral);
        r.drawTextBody(detail,
                      render::rect(panel.left + 48, win.height * 0.5f - 12, panel.right - 48,
                                  win.height * 0.5f + 78),
                      16.0f, kMuted);

        const render::Rect cta =
            render::rect(cx - 140.0f, panel.bottom - 96.0f, cx + 140.0f, panel.bottom - 40.0f);
        drawPillButton(r, cta, L"Premi INVIO per riprovare", kBad);

        if (reason == app::FailReason::NoSignal) {
            drawStatusHint(r, render::rect(panel.left + 32, panel.bottom - 30, panel.right - 32, panel.bottom - 6),
                      L"M per continuare comunque, con un profilo generico non personale");
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

    // --- pillola dell'ingrandimento, in alto ---
    const float testo = std::max(24.0f, win.height * 0.055f);
    const float padX  = testo * 0.9f;
    const float padY  = testo * 0.34f;

    const std::wstring etichetta = std::to_wstring(cf.magnification) + L"×";
    const float boxW = testo * (0.62f * etichetta.size()) + padX * 2.0f;
    const float boxH = testo + padY * 2.0f;
    const float boxY = win.height * 0.035f;

    const render::Rect box =
        render::rect(cx - boxW / 2, boxY, cx + boxW / 2, boxY + boxH);
    r.fillRect(box, kBg, boxH * 0.5f);
    r.drawTextCentered(etichetta, {cx, (box.top + box.bottom) * 0.5f}, testo, kCoral, true);

    // --- barra della scala, in basso ---
    const double campo = fieldWidthMeters(cf.magnification);
    if (campo <= 0.0) return;

    const double perPixel = campo / win.width;
    // Un settimo della larghezza: la barra e' un riferimento, non un titolo.
    const double target   = perPixel * (win.width * 0.14);
    const double lunghezza = niceLength(target);
    if (lunghezza <= 0.0) return;

    // Ritmo verticale dichiarato invece che aggiustato a occhio: corpo
    // dell'etichetta, la sua riga, lo stacco, la barra, e lo stesso margine
    // sopra e sotto. Prima la pillola era costruita per differenze a partire
    // dalla barra, e l'etichetta finiva a filo del bordo superiore.
    const float et     = std::max(13.0f, win.height * 0.020f);
    const float rigaH  = et * 1.30f;
    const float tick   = std::max(6.0f, win.height * 0.010f);
    const float spess  = std::max(2.0f, win.height * 0.003f);
    const float margY  = et * 0.72f;
    const float stacco = et * 0.55f;

    const auto  barW  = static_cast<float>(lunghezza / perPixel);
    const float pilloW = barW + et * 2.6f;
    const float pilloH = margY * 2.0f + rigaH + stacco + tick;
    const float pilloB = win.height * 0.955f;
    const render::Rect pillo =
        render::rect(cx - pilloW * 0.5f, pilloB - pilloH, cx + pilloW * 0.5f, pilloB);
    r.fillRect(pillo, kBg, pilloH * 0.5f);

    const float etY  = pillo.top + margY + rigaH * 0.5f;
    const float barY = pillo.top + margY + rigaH + stacco;
    const float x0   = cx - barW * 0.5f;
    const float x1   = cx + barW * 0.5f;

    r.drawTextCentered(formatLength(lunghezza), {cx, etY}, et, kInk, true);
    r.fillRect(render::rect(x0, barY, x1, barY + spess), kInk);
    r.fillRect(render::rect(x0, barY - tick / 2, x0 + spess, barY + tick / 2), kInk);
    r.fillRect(render::rect(x1 - spess, barY - tick / 2, x1, barY + tick / 2), kInk);
}

std::wstring toWide(const char* s) {
    std::wstring w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

/** Fase corrente in parole, per chi presidia la postazione. */
std::wstring phaseInWords(const app::ControlState& st, bool landing, bool locked) {
    if (landing) return L"Pagina d’ingresso · in attesa del partecipante";
    // Lette da qui invece che passate: sono schermate dell'esperienza intera,
    // esattamente come la pagina d'ingresso, e aggiungere due parametri a
    // ogni chiamante per due bandierine globali non chiarirebbe niente.
    if (g.onboardingVisible.load(std::memory_order_relaxed)) {
        return L"Accoglienza · istruzioni e respiro guidato";
    }
    if (g.offboardingVisible.load(std::memory_order_relaxed)) {
        return L"Congedo · fascia da togliere e igienizzare";
    }
    const auto phase = static_cast<control::Phase>(st.phase);
    if (phase == control::Phase::Onboarding) {
        switch (static_cast<control::CalibStage>(st.calibStage)) {
            case control::CalibStage::Intro:       return L"Calibrazione · istruzioni";
            case control::CalibStage::Concentrate: return L"Calibrazione · concentrazione";
            case control::CalibStage::Prepare:     return L"Calibrazione · secondo passaggio";
            case control::CalibStage::Relax:       return L"Calibrazione · rilassamento";
            case control::CalibStage::Done:        return L"Calibrazione · completata";
            case control::CalibStage::Failed:      return L"Calibrazione · non riuscita";
        }
    }
    if (st.adaptiveActive && !st.adaptiveReady) {
        return L"Adattamento al segnale · " + fixed(st.adaptiveWarmup * 100.0, 0) + L"%";
    }
    std::wstring s;
    switch (phase) {
        case control::Phase::Interactive: s = L"Esperienza in corso"; break;
        case control::Phase::Hook:        s = L"Esperienza · avvio"; break;
        case control::Phase::Handover:    s = L"Esperienza · passaggio di controllo"; break;
        case control::Phase::Outro:       s = L"Esperienza · chiusura"; break;
        case control::Phase::Done:        s = L"Esperienza conclusa"; break;
        default:                          s = L"In attesa"; break;
    }
    if (locked) s += L" · zoom fermo (in attesa di un cambio netto)";
    return s;
}

/**
 * Pannello operatore, sopra l'immagine. Due livelli:
 *   1 base:    quello che serve a chi presidia la postazione - sorgente,
 *              diagnosi con codice e cosa fare, fase, ingrandimento, FPS, log,
 *              i tasti d'emergenza. Niente numeri da interpretare.
 *   2 esperto: in piu' tutte le letture numeriche (indice, banda, velocita',
 *              bande spettrali, pacchetti) e la taratura dal vivo.
 * Le righe si raccolgono prima e si disegnano dopo, cosi' lo sfondo scuro
 * dietro puo' essere alto quanto il contenuto.
 */
float drawHud(render::Renderer& r, const app::ControlState& st, const control::ZoomController& zoom,
              const control::CrossfadeState& cf, const control::Tunables& t, int level, double fps,
              diag::Code code, bool landing) {
    struct Row { std::wstring s; render::Color c; float size; float h; };
    std::vector<Row> rows;
    const float x = 24.0f, w = 600.0f;
    const auto line = [&](const std::wstring& s, render::Color c, float size = 14.0f) {
        rows.push_back({s, c, size, size + 7.0f});
    };
    // Riga che puo' andare a capo (le azioni consigliate sono lunghe).
    const auto para = [&](const std::wstring& s, render::Color c, float size) {
        const float lines = s.size() > 78 ? 2.0f : 1.0f;
        rows.push_back({s, c, size, (size + 4.0f) * lines + 3.0f});
    };
    // Le righe dei tasti: acqua smorzata, come il resto del pannello. Nere
    // sarebbero invisibili - il pannello sta sul nero della foto.
    const render::Color kKeys{kBg.r, kBg.g, kBg.b, 0.55f};

    // --- intestazione: livello e FPS ---
    {
        render::Color fc = fps >= 50.0 ? kHudOk : (fps >= 30.0 ? kWarn : kBad);
        line(std::wstring(L"PANNELLO OPERATORE · ") + (level >= 2 ? L"esperto" : L"base") +
                 L"     " + fixed(fps, 0) + L" fps",
             fps >= 50.0 ? kHudMuted : fc, 12.0f);
    }

    // --- sorgente ---
    if (st.replaying) {
        line(L"Sorgente: sessione dimostrativa · " + g.replayName, kAccent, 15.0f);
    } else {
        const wchar_t* bleNames[] = {L"Fascia EEG scollegata", L"Ricerca della fascia in corso…",
                                     L"Connessione alla fascia in corso…",
                                     L"Fascia EEG collegata · dati in arrivo"};
        const int bs = std::clamp(st.bleState, 0, 3);
        line(std::wstring(L"Sorgente: ") + bleNames[bs], bs == 3 ? kHudOk : (bs == 0 ? kBad : kWarn),
             15.0f);
    }

    // --- diagnosi: un codice, cosa succede, cosa fare ---
    {
        const auto& d = diag::info(code);
        const render::Color c = d.severity == diag::Severity::Error ? kBad
                              : d.severity == diag::Severity::Warn  ? kWarn
                              : d.severity == diag::Severity::Info  ? kAccent
                                                                    : kHudOk;
        line(L"[" + toWide(d.code) + L"]  " + d.title, c, 15.0f);
        if (d.action[0]) para(std::wstring(L"→ ") + d.action, kHudInk, 12.5f);
    }

    line(L"Fase: " + phaseInWords(st, landing, zoom.locked()), kHudInk);
    line(L"Ingrandimento: " + std::to_wstring(cf.magnification) + L"×", kAccent, 15.0f);

    if (st.recording) {
        const double secs = static_cast<double>(st.recordedSamples) / config::kSampleRate;
        line(L"Registrazione: " + fileNameOf(g.recorder.path()) + L"  (" + fixed(secs, 0) + L" s)",
             kHudOk, 12.0f);
    }
    if (!g.debugLogPath.empty()) {
        line(L"Log: debug/" + fileNameOf(toWide(g.debugLogPath.c_str())), kHudMuted, 12.0f);
    }

    line(L"H livello pannello   B Bluetooth   R tenuto premuto: riavvio completo   ESC/Q esci",
         kKeys, 12.0f);

    if (level < 2) {
        // --- disegno (solo base) ---
        float h = 16.0f;
        for (const auto& row : rows) h += row.h;
        r.fillRect(render::rect(x - 12.0f, 12.0f, x + w + 12.0f, 12.0f + h), kHudBg, 10.0f);
        float y = 20.0f;
        for (const auto& row : rows) {
            r.drawTextBody(row.s, render::rect(x, y, x + w, y + row.h + 2.0f), row.size, row.c,
                          render::TextAlign::Left);
            y += row.h;
        }
        return 12.0f + h;
    }

    // --- livello esperto: le letture numeriche, com'erano prima ---
    line(L"", kHudMuted, 4.0f);
    line(L"LETTURE (livello esperto)", kKeys, 11.0f);

    if (st.signalFault != 0) {
        const wchar_t* faults[] = {L"OK", L"RETE 50 Hz", L"SATURO", L"PIATTO",
                                   L"NON E' UN SEGNALE"};
        const int fi = std::clamp(st.signalFault, 0, 4);
        line(std::wstring(L"Segnale: ") + faults[fi], kBad, 14.0f);

        if (fi == static_cast<int>(dsp::SignalFault::Mains)) {
            line(L"  " + fixed(st.mainsFraction * 100.0, 0) +
                     L"% a 50 Hz DOPO la derivazione bipolare: non e' modo comune",
                 kHudMuted, 12.0f);
            line(L"  i due frontali leggono cose diverse: uno dei due non e' accoppiato",
                 kHudMuted, 12.0f);
        } else {
            line(L"  correlazione " + fixed(st.autocorr1, 2) + L" (serve > " +
                     fixed(config::kMinAutocorr1, 2) + L")   saturi " +
                     fixed(st.railFraction * 100.0, 1) + L"%   rete " +
                     fixed(st.mainsFraction * 100.0, 0) + L"%",
                 kHudMuted, 12.0f);
        }
    } else {
        const wchar_t* gate = !st.contactOk ? L"CONTATTO" : (st.artifact ? L"ARTEFATTO" : L"OK");
        line(std::wstring(L"Segnale: ") + gate,
             !st.contactOk ? kBad : (st.artifact ? kWarn : kHudOk));
    }

    if (st.stalled) line(L"Flusso fermo: ripresa in corso", kBad);

    const wchar_t* phaseNames[] = {L"IDLE",   L"ONBOARDING", L"HOOK", L"HANDOVER",
                                   L"INTERACTIVE", L"OUTRO", L"DONE"};
    std::wstring phaseLine = std::wstring(L"Fase interna: ") +
                             phaseNames[std::clamp(st.phase, 0, 6)];
    if (zoom.locked()) phaseLine += L" (HOLD)";
    line(phaseLine, kHudMuted);

    line(L"Indice: " + fixed(st.rawIndex, 3) + L"   c: " + fixed(st.smoothedIndex, 3), kHudMuted);

    if (st.calibValid) {
        line(L"Banda: [" + fixed(st.absMin, 2) + L" .. " + fixed(st.absMax, 2) + L"]   M: " +
                 fixed(st.neutral, 2), kHudMuted);
        line(L"Locale: [" + fixed(st.localMin, 2) + L" .. " + fixed(st.localMax, 2) + L"]", kHudMuted);
    } else {
        line(L"Banda: calibrazione in corso", kHudMuted);
    }

    line(L"Velocita': " + fixed(st.velocity, 3) + L"  (cruda " + fixed(st.velocityRaw, 3) + L")",
         st.velocity > 0 ? kHudOk : (st.velocity < 0 ? kWarn : kHudMuted));

    // La barra di attivazione si disegna dopo le righe: qui si riserva solo
    // lo spazio e ci si segna a che riga va.
    const bool  showBar = st.calibValid && st.absMax > st.absMin;
    const float bh      = 22.0f;
    std::size_t barRow  = 0;
    if (showBar) {
        barRow = rows.size();
        rows.push_back({L"", kHudMuted, 0.0f, bh + 7.0f});
        line(L"Gate: " + fixed(st.gate, 2) + L"   Ampiezza: " + fixed(st.magnitude, 2),
             st.gate > 0.05 ? kHudOk : kHudMuted, 13.0f);
    }

    line(L"Focus: " + fixed(zoom.targetFocus(), 3) + L" -> " + fixed(zoom.currentFocus(), 3), kHudMuted);
    line(L"Bande  t:" + fixed(st.theta, 1) + L"  a:" + fixed(st.alpha, 1) + L"  b:" +
             fixed(st.beta, 1), kHudMuted);
    line(L"Ampiezza: " + fixed(st.maxAbsRaw, 0) + L" uV   pkt: " +
             std::to_wstring(st.packetLen) + L"B/" + std::to_wstring(st.packetSamples) + L"smp",
         kHudMuted);

    {
        const std::wstring counts = L"Pacchetti: " + std::to_wstring(st.rawPackets) +
                                    L" grezzi / " + std::to_wstring(st.validPackets) + L" validi";
        render::Color c = kHudMuted;
        if (st.bleState == 3 && st.rawPackets == 0)                 c = kBad;
        else if (st.rawPackets > 0 && st.validPackets == 0)         c = kWarn;
        line(counts, c);
    }

    const auto logLines = g.bleLogSnapshot();
    if (!logLines.empty()) {
        line(L"", kHudMuted, 1.0f);
        line(L"Ultime righe del log:", kKeys, 12.0f);
        for (const auto& l : logLines) line(L"  " + l, kHudMuted, 12.0f);
    }

    line(L"", kHudMuted, 1.0f);
    line(L"Sensibilita': " + fixed(t.sensitivity, 1) + L"x    Tolleranza: " +
             fixed(t.localTolerance, 2) + L"    Smoothing: " + fixed(t.velTauS, 2) + L"s" +
             L"    Elastico: " + fixed(t.elasticTauS, 2) + L"s",
         kAccent, 12.0f);
    if (!t.holdEnabled) line(L"Hold/detent SPENTO (diagnostica)", kWarn, 12.0f);

    line(L"su/giu sensibilita'   sin/des tolleranza   S smoothing   E elastico   L hold   "
         L"R tap: reset manopole",
         kKeys, 12.0f);
    line(L"K ricalibra   V grafici calibrazione   D segnale simulato   C fascia reale", kKeys,
         12.0f);

    // --- disegno (livello esperto) ---
    float h = 16.0f;
    for (const auto& row : rows) h += row.h;
    r.fillRect(render::rect(x - 12.0f, 12.0f, x + w + 12.0f, 12.0f + h),
               {kHudBg.r, kHudBg.g, kHudBg.b, 0.72f}, 10.0f);
    float y = 20.0f;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (showBar && i == barRow) {
            const float bw = 460.0f, by = y;
            const auto toX = [&](double v) {
                const double f = (v - st.absMin) / (st.absMax - st.absMin);
                return x + static_cast<float>(std::clamp(f, 0.0, 1.0)) * bw;
            };
            r.fillRect(render::rect(x, by, x + bw, by + bh), {0, 0, 0, 0.06f}, 4.0f);
            r.fillRect(render::rect(toX(st.localMin), by, toX(st.localMax), by + bh),
                       {0, 0, 0, 0.08f}, 4.0f);
            const double tolUp = t.localTolerance * (st.absMax - st.neutral);
            const double tolDn = t.localTolerance * (st.neutral - st.absMin);
            r.fillRect(render::rect(toX(st.localMax - tolUp), by, toX(st.localMax), by + bh),
                       {kHudOk.r, kHudOk.g, kHudOk.b, 0.24f}, 4.0f);
            r.fillRect(render::rect(toX(st.localMin), by, toX(st.localMin + tolDn), by + bh),
                       {kWarn.r, kWarn.g, kWarn.b, 0.24f}, 4.0f);
            const float nx = toX(st.neutral);
            r.fillRect(render::rect(nx - 1.0f, by, nx + 1.0f, by + bh), kHudMuted);
            const float px = toX(st.smoothedIndex);
            r.fillRect(render::rect(px - 2.0f, by - 3.0f, px + 2.0f, by + bh + 3.0f), kAccent,
                       2.0f);
        } else if (!row.s.empty()) {
            r.drawTextBody(row.s, render::rect(x, y, x + w, y + row.h + 2.0f), row.size, row.c,
                          render::TextAlign::Left);
        }
        y += row.h;
    }
    return 12.0f + h;
}

// ---------------------------------------------------------------------------
// Sala di controllo (due schermi): NON l'esperienza - quella sta tutta sulla
// proiezione - e non piu' nemmeno tutto cio' che si potrebbe mostrare.
//
// Resta solo quello che serve a chi guida la postazione mentre qualcuno ha la
// fascia in testa: il SEGNALE ELABORATO della fascia (il bipolare filtrato, il
// suo spettro, l'indice che ne esce nel tempo) e le STATISTICHE DI ESERCIZIO
// (fps, pacchetti, stato del collegamento, diagnosi).
//
// Sono spariti, di proposito:
//   - le quattro tracce grezze: il grezzo non e' il segnale elaborato, e si
//     prendeva un quarto della finestra;
//   - il riquadro del "testo di sala": un rettangolo grande mezza finestra con
//     dentro un promemoria per chi scrive il programma, mai compilato;
//   - la parete di grafici della telemetria, che ripeteva in sei riquadri
//     quello che l'indice dice in uno.
//
// Non e' solo ordine. Ogni pixel di questa finestra si paga a ogni fotogramma:
// l'interfaccia si rasterizza in CPU su una bitmap grande quanto la finestra
// per il fattore di scala, e la bitmap viene azzerata, ridisegnata, copiata e
// caricata sul layer. Sul Mac Intel della mostra la finestra da 1280x800 punti
// faceva 2560x1600 px, cioe' 16 MB per fotogramma. Con meno da mostrare la
// finestra puo' essere piccola (kControlRoomW/H in shell_macos.mm), e il conto
// scende di circa tre volte.
// ---------------------------------------------------------------------------

/** Cornice di un grafico della sala di controllo, con titolo. */
void plotFrame(render::Renderer& r, render::Rect box, const wchar_t* title) {
    r.fillRect(box, kHudBg);
    r.drawRectOutline(box, kHudHairline, 1.0f);
    r.drawTextBody(title, render::rect(box.left + 8, box.top + 3, box.right - 8, box.top + 19),
                  11.0f, kHudMuted, render::TextAlign::Left);
}

/**
 * Il bipolare filtrato dell'ultimo secondo: e' letteralmente "il segnale
 * elaborato della fascia", quello da cui esce tutto il resto. La scala
 * verticale si adatta al picco invece di essere fissa: a fascia ben messa il
 * segnale sta in pochi microvolt, e una scala fissa lo mostrerebbe piatto.
 */
void drawProcessedTrace(render::Renderer& r, const app::WaveSnapshot& w, render::Rect box) {
    plotFrame(r, box, L"Segnale elaborato · bipolare AF7−AF8, 1–40 Hz · µV");
    if (w.frame == 0 || box.height() < 30.0f) return;

    constexpr int kN = config::kStftWindow;
    static std::vector<render::Point> pts;
    pts.resize(kN);

    const render::Rect lane = render::rect(box.left + 8.0f, box.top + 20.0f,
                                           box.right - 8.0f, box.bottom - 5.0f);
    const float mid  = (lane.top + lane.bottom) * 0.5f;
    const float half = lane.height() * 0.5f - 2.0f;

    float peak = 0.0f;
    for (int i = 0; i < kN; ++i) peak = std::max(peak, std::fabs(w.processed[i]));
    const float scale = std::max(10.0f, peak * 1.1f);

    r.pushClip(box);
    r.drawLine({lane.left, mid}, {lane.right, mid}, {0, 0, 0, 0.08f}, 1.0f);
    r.drawTextBody(L"±" + fixed(scale, 0),
                  render::rect(lane.right - 60.0f, lane.top, lane.right, lane.top + 13.0f),
                  10.0f, {kHudMuted.r, kHudMuted.g, kHudMuted.b, 0.6f}, render::TextAlign::Left);
    for (int i = 0; i < kN; ++i) {
        const float t = static_cast<float>(i) / (kN - 1);
        const float v = std::clamp(w.processed[i] / scale, -1.0f, 1.0f);
        pts[static_cast<std::size_t>(i)] = {lane.left + t * lane.width(), mid - v * half};
    }
    r.drawPolyline(pts.data(), pts.size(), kAccent, 1.2f);
    r.popClip();
}

/** Lo spettro dello stesso bipolare, con theta/alpha/beta evidenziate. */
void drawSpectrum(render::Renderer& r, const app::WaveSnapshot& w, render::Rect box) {
    plotFrame(r, box, L"Spettro · 1–40 Hz · theta 4–8, alpha 8–13, beta 13–30");
    if (w.frame == 0 || box.height() < 34.0f) return;

    constexpr int kHzMax = 40;
    const render::Rect area = render::rect(box.left + 8.0f, box.top + 20.0f,
                                           box.right - 8.0f, box.bottom - 15.0f);
    const float binW = area.width() / kHzMax;
    float peak = 1e-6f;
    for (int b = 1; b <= kHzMax; ++b) peak = std::max(peak, w.spectrum[b]);

    static std::vector<render::Rect> bars[3];
    for (auto& v : bars) v.clear();
    for (int b = 1; b <= kHzMax; ++b) {
        const float h  = std::clamp(w.spectrum[b] / peak, 0.0f, 1.0f) * area.height();
        const float x0 = area.left + (b - 1) * binW + 1.0f;
        const int   grp = (b >= 4 && b < 8) ? 1 : (b >= 8 && b < 13) ? 2 : (b >= 13 && b < 30) ? 0 : -1;
        const render::Rect bar = render::rect(x0, area.bottom - h, x0 + binW - 2.0f, area.bottom);
        if (grp < 0) r.fillRect(bar, {kHudMuted.r, kHudMuted.g, kHudMuted.b, 0.35f});
        else         bars[grp].push_back(bar);
    }
    r.fillRects(bars[0].data(), static_cast<int>(bars[0].size()),
               {kHudInk.r, kHudInk.g, kHudInk.b, 0.55f});
    r.fillRects(bars[1].data(), static_cast<int>(bars[1].size()), kAccent2);
    r.fillRects(bars[2].data(), static_cast<int>(bars[2].size()), kAccent);
    for (const int hz : {10, 20, 30, 40}) {
        r.drawTextBody(std::to_wstring(hz),
                      render::rect(area.left + (hz - 1) * binW - 10.0f, area.bottom + 1.0f,
                                  area.left + (hz - 1) * binW + 14.0f, area.bottom + 13.0f),
                      10.0f, {kHudMuted.r, kHudMuted.g, kHudMuted.b, 0.6f}, render::TextAlign::Left);
    }
}

/**
 * L'indice elaborato negli ultimi 60 s: e' il numero che comanda lo zoom, ed
 * e' l'unica cosa della vecchia parete di telemetria che valga uno schermo da
 * sola. La scala si prende dai valori davvero presenti nella finestra: l'indice
 * non e' normalizzato in [0,1] (nei log si vedono valori sopra 2), quindi una
 * scala fissa lo appiattirebbe contro il bordo.
 */
void drawIndexStrip(render::Renderer& r, const app::TelemetryHistory& history, render::Rect box) {
    plotFrame(r, box, L"Indice elaborato · ultimi 60 s");
    if (box.height() < 30.0f) return;

    constexpr double kWindowS = 60.0;
    const auto visible = static_cast<std::size_t>(kWindowS * config::kControlHz);
    const std::size_t count = std::min(history.size(), visible);
    if (count < 2) return;
    const std::size_t first = history.size() - count;

    const render::Rect g = render::rect(box.left + 8.0f, box.top + 20.0f,
                                        box.right - 8.0f, box.bottom - 5.0f);

    double lo = history.at(first).smoothedIndex, hi = lo;
    for (std::size_t i = 0; i < count; ++i) {
        const double v = history.at(first + i).smoothedIndex;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const double pad = std::max(0.05, (hi - lo) * 0.1);
    lo -= pad;
    hi += pad;
    if (hi <= lo) return;

    static std::vector<render::Point> pts;
    pts.clear();
    pts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double v = history.at(first + i).smoothedIndex;
        const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
        pts.push_back({g.left + (static_cast<float>(i) / static_cast<float>(count - 1)) * g.width(),
                       static_cast<float>(g.bottom - t * (g.bottom - g.top))});
    }
    r.pushClip(box);
    r.drawPolyline(pts.data(), pts.size(), kAccent, 1.6f);
    r.popClip();

    r.drawTextBody(fixed(hi, 2), render::rect(g.right - 54.0f, g.top, g.right, g.top + 13.0f),
                  10.0f, {kHudMuted.r, kHudMuted.g, kHudMuted.b, 0.6f}, render::TextAlign::Left);
    r.drawTextBody(fixed(lo, 2),
                  render::rect(g.right - 54.0f, g.bottom - 13.0f, g.right, g.bottom),
                  10.0f, {kHudMuted.r, kHudMuted.g, kHudMuted.b, 0.6f}, render::TextAlign::Left);
}

/**
 * La sala di controllo per intero. Due blocchi e basta: in alto lo stato in
 * parole (diagnosi, sorgente, esercizio), sotto i tre grafici del segnale
 * elaborato. Il ritmo verticale del blocco di testo e' calcolato dalle righe
 * che ci finiscono davvero, non fissato a mano: le righe cambiano di numero
 * (registrazione, azione consigliata) e un'altezza fissa lascerebbe un buco o
 * mangerebbe il grafico sotto.
 */
void drawControlRoom(render::Renderer& r, const app::ControlState& st,
                     const control::ZoomController& zoom, const control::CrossfadeState& cf,
                     const app::TelemetryHistory& history, double fps, diag::Code code,
                     bool landing) {
    const auto  win = r.size();
    const float pad = 10.0f;
    const float x0  = pad, x1 = win.width - pad;
    if (x1 - x0 < 220.0f || win.height < 220.0f) return;

    // ATTENZIONE, trappola gia' pagata due volte in questo programma:
    // CTFramesetterCreateFrame compone SOLO le righe che stanno per intero nel
    // riquadro. Un riquadro alto anche un solo punto meno dell'altezza di riga
    // del font non produce testo tagliato a meta': non produce NIENTE, in
    // silenzio. Le altezze qui sotto si MISURANO quindi con measureTextBody
    // invece di stimarle da (corpo + qualche punto) - stima che aveva gia'
    // fatto sparire l'intestazione e la riga dei tasti, viste vuote a schermo
    // l'08/09/2026.
    const auto lineH = [&](float size) { return r.measureTextBody(L"Hg", size).height; };

    // --- intestazione: chi sono e a che ritmo sto disegnando ---
    float y = pad;
    {
        const float h = lineH(12.0f);
        r.drawTextBody(L"MIND ZOOM · SALA DI CONTROLLO",
                      render::rect(x0, y, x1, y + h + 2.0f), 12.0f, kHudMuted,
                      render::TextAlign::Left);
        // Il renderer non ha TextAlign::Right: per allineare a destra si misura
        // la larghezza e si posiziona il riquadro.
        const std::wstring f  = fixed(fps, 0) + L" fps";
        const float        fw = r.measureTextBody(f, 12.0f).width;
        r.drawTextBody(f, render::rect(x1 - fw - 2.0f, y, x1, y + h + 2.0f), 12.0f,
                      fps >= 50.0 ? kHudOk : (fps >= 25.0 ? kWarn : kBad), render::TextAlign::Left);
        y += h + 8.0f;
    }

    // --- stato in parole: le righe che contano, e nient'altro ---
    const auto row = [&](const std::wstring& s, render::Color c, float size) {
        const float h = lineH(size);
        r.drawTextBody(s, render::rect(x0, y, x1, y + h + 2.0f), size, c,
                      render::TextAlign::Left);
        y += h + 3.0f;
    };

    if (st.replaying) {
        row(L"Sorgente: sessione dimostrativa · " + g.replayName, kAccent, 13.0f);
    } else {
        const wchar_t* bleNames[] = {L"Fascia EEG scollegata", L"Ricerca della fascia in corso…",
                                     L"Connessione alla fascia in corso…",
                                     L"Fascia EEG collegata · dati in arrivo"};
        const int bs = std::clamp(st.bleState, 0, 3);
        row(std::wstring(L"Sorgente: ") + bleNames[bs],
            bs == 3 ? kHudOk : (bs == 0 ? kBad : kWarn), 13.0f);
    }

    {
        const auto& d = diag::info(code);
        const render::Color c = d.severity == diag::Severity::Error ? kBad
                              : d.severity == diag::Severity::Warn  ? kWarn
                              : d.severity == diag::Severity::Info  ? kAccent
                                                                    : kHudOk;
        row(L"[" + toWide(d.code) + L"]  " + d.title, c, 13.0f);
        if (d.action[0]) row(std::wstring(L"→ ") + d.action, kHudInk, 11.0f);
    }

    row(L"Fase: " + phaseInWords(st, landing, zoom.locked()) +
            L"   ·   Ingrandimento " + std::to_wstring(cf.magnification) + L"×",
        kHudMuted, 12.0f);

    // Esercizio: quanto arriva e quanto se ne salva. E' la riga che dice se il
    // collegamento sta reggendo, non se il segnale e' buono. Solo con la fascia
    // vera: in riproduzione i contatori restano a zero per costruzione, e uno
    // zero in una riga di statistiche si legge come un guasto.
    if (!st.replaying) {
        row(L"Pacchetti " + std::to_wstring(st.rawPackets) + L" grezzi / " +
                std::to_wstring(st.validPackets) + L" validi   ·   " +
                std::to_wstring(st.packetLen) + L" B / " + std::to_wstring(st.packetSamples) +
                L" campioni",
            kHudMuted, 12.0f);
    }

    // Qualita' e numeri del segnale elaborato, su una riga sola.
    {
        const wchar_t* faults[] = {L"OK", L"RETE 50 Hz", L"SATURO", L"PIATTO", L"NON E' UN SEGNALE"};
        const int  fi  = std::clamp(st.signalFault, 0, 4);
        const bool bad = st.signalFault != 0 || !st.contactOk;
        const std::wstring qual = st.signalFault != 0
                                      ? std::wstring(faults[fi])
                                      : (!st.contactOk ? L"CONTATTO"
                                                       : (st.artifact ? L"ARTEFATTO" : L"OK"));
        row(L"Segnale " + qual + L"   ·   indice " + fixed(st.smoothedIndex, 3) +
                L"   ·   velocità " + fixed(st.velocity, 3),
            bad ? kBad : (st.artifact ? kWarn : kHudMuted), 12.0f);
    }

    if (st.recording) {
        const double secs = static_cast<double>(st.recordedSamples) / config::kSampleRate;
        row(L"Registra: " + fileNameOf(g.recorder.path()) + L"  (" + fixed(secs, 0) + L" s)",
            kHudOk, 11.0f);
    }

    // --- i tre grafici, che si prendono tutto lo spazio che resta ---
    const float keysH = lineH(11.0f) + 2.0f;
    const float top   = y + 4.0f;
    const float bottom = win.height - pad - keysH;
    const float H = bottom - top;
    if (H > 90.0f) {
        const float gap = 6.0f;
        const float h1 = H * 0.34f, h2 = H * 0.30f;
        const app::WaveSnapshot wave = g.wave.read();
        drawProcessedTrace(r, wave, render::rect(x0, top, x1, top + h1 - gap));
        drawSpectrum(r, wave, render::rect(x0, top + h1, x1, top + h1 + h2 - gap));
        drawIndexStrip(r, history, render::rect(x0, top + h1 + h2, x1, bottom - gap));
    }

    r.drawTextBody(L"H pannello · B bluetooth · R tenuto: riavvio · ESC esce",
                  render::rect(x0, win.height - pad - keysH, x1, win.height - pad + 2.0f),
                  11.0f, {kHudMuted.r, kHudMuted.g, kHudMuted.b, 0.55f}, render::TextAlign::Left);
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
    r.fillRect(box, kHudBg);
    r.drawRectOutline(box, {0.0f, 0.0f, 0.0f, 0.25f}, 1.0f);

    r.drawTextBody(L"Debug calibrazione (V per chiudere)",
                   render::rect(box.left + 14, box.top + 10, box.right - 14, box.top + 28),
                   13.0f, kHudMuted, render::TextAlign::Left);

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
        r.fillRect(g, {0.0f, 0.0f, 0.0f, 0.04f});
        r.drawRectOutline(g, kHudHairline, 1.0f);
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
                       render::Color{kHudMuted.r, kHudMuted.g, kHudMuted.b, kHudMuted.a * 0.8f}, render::TextAlign::Left);
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
                       render::Color{kHudMuted.r, kHudMuted.g, kHudMuted.b, kHudMuted.a * 0.8f}, render::TextAlign::Left);
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

    // Velo acqua quasi pieno sopra la foto, riquadro piatto con un filetto
    // nero: e' una finestra di servizio, non un pezzo dell'esperienza.
    r.fillRect(render::rect(0, 0, win.width, win.height), {kBg.r, kBg.g, kBg.b, 0.92f});

    const render::Rect box = render::rect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    r.fillRect(box, kPanel);
    r.drawRectOutline(box, {0.0f, 0.0f, 0.0f, 0.25f}, 1.0f);

    float y = box.top + 44.0f;
    r.drawTextDisplayCentered(L"BLUETOOTH", {cx, y}, 40.0f, kCoral);
    y += 46.0f;

    if (st.replaying) {
        r.drawTextBodyCentered(L"Sorgente: sessione dimostrativa (segnale registrato)", {cx, y},
                              16.0f, kAccent);
        y += 26.0f;
        r.drawTextBodyCentered(L"C · torna alla fascia EEG", {cx, y}, 13.0f, kMuted);
    } else {
        const wchar_t* bleNames[] = {L"Scollegata", L"Ricerca in corso", L"Connessione in corso",
                                     L"Collegata · dati in arrivo"};
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
    r.drawTextBodyCentered(L"C · collega o ricollega        X · scollega", {cx, y}, 15.0f,
                          kInk);
    y += 26.0f;
    r.drawTextBodyCentered(L"ESC · chiudi", {cx, y}, 12.0f, kMuted);
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
    r.fillRect(render::rect(0, 0, win.width, win.height), kBg);

    const std::wstring number = std::to_wstring(candidate + 1);
    r.drawText(number,
               render::rect(0, win.height * 0.5f - 190.0f, win.width, win.height * 0.5f + 60.0f),
               260.0f, kCoral, render::TextAlign::Center, true);

    std::wstring caption = L"Questo schermo";
    if (candidate >= 0 && candidate < static_cast<int>(displays.size())) {
        caption += L"  ·  " + displays[static_cast<std::size_t>(candidate)].describe();
    }
    r.drawText(caption,
               render::rect(0, win.height * 0.5f + 70.0f, win.width, win.height * 0.5f + 110.0f),
               22.0f, kInk, render::TextAlign::Center);

    r.drawText(L"Frecce per cambiare schermo   ·   INVIO per confermare",
               render::rect(0, win.height * 0.5f + 130.0f, win.width, win.height * 0.5f + 170.0f),
               18.0f, kMuted, render::TextAlign::Center);
}

/** Sullo schermo dell'operatore durante la scelta: l'elenco, col candidato evidenziato. */
void drawScreenPickerPanel(render::Renderer& r, int candidate, const std::vector<Display>& displays) {
    const auto win = r.size();
    const float cx = win.width * 0.5f;

    r.fillRect(render::rect(0, 0, win.width, win.height), {kBg.r, kBg.g, kBg.b, 0.92f});

    const float panelW = 620.0f;
    const float rowH   = 52.0f;
    const float panelH = 250.0f + rowH * static_cast<float>(displays.size());
    const render::Rect panel = render::rect(cx - panelW / 2, win.height * 0.5f - panelH / 2,
                                          cx + panelW / 2, win.height * 0.5f + panelH / 2);
    r.fillRect(panel, kPanel);
    r.drawRectOutline(panel, {0.0f, 0.0f, 0.0f, 0.25f}, 1.0f);

    float y = panel.top + 30.0f;
    r.drawTextDisplay(L"SU QUALE SCHERMO PROIETTARE?",
                      render::rect(panel.left, y, panel.right, y + 56.0f), 40.0f, kCoral);
    y += 60.0f;

    r.drawText(L"Il partecipante vedrà solo l’immagine, a schermo intero.\n"
               L"Qui restano il pannello operatore e i comandi.",
               render::rect(panel.left + 36, y, panel.right - 36, y + 60.0f), 16.0f, kMuted);
    y += 76.0f;

    for (std::size_t i = 0; i < displays.size(); ++i) {
        const bool sel = (static_cast<int>(i) == candidate);
        const render::Rect row = render::rect(panel.left + 30, y, panel.right - 30, y + rowH - 8.0f);
        if (sel) {
            r.fillRect(row, kAccent);
        }
        r.drawText(std::to_wstring(i + 1) + L".   " + displays[i].describe(),
                   render::rect(row.left + 18, row.top + 10, row.right - 18, row.bottom),
                   18.0f, sel ? kInk : kMuted, render::TextAlign::Left, sel);
        y += rowH;
    }

    y += 14.0f;
    r.drawText(L"Frecce per cambiare   ·   INVIO per confermare",
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

// Orologi delle tre schermate che si susseguono da sole: accoglienza (quanto
// manca al pulsante), esperienza (quanto manca al promemoria del riavvio),
// congedo (quanto manca al ritorno alla pagina d'ingresso). Avanzano in
// frame() con lo stesso dt di tutto il resto, quindi non hanno bisogno di un
// orologio di sistema ne' di essere atomici: li tocca solo il thread di render.
double onboardT  = 0.0;
double sessionT  = 0.0;
double offboardT = 0.0;

// Avanzamento della pressione di R, pubblicato dallo shell (setRestartHold).
// Atomico perche' su Windows/altri shell potrebbe non arrivare dallo stesso
// thread del render; su macOS arriva dal thread principale come tutto il resto.
std::atomic<double> restartHold{0.0};

// Ultima dimensione della finestra che ha disegnato l'esperienza, e se era
// quella principale: handleClick() non riceve la geometria (lo shell manda
// solo il punto) e i pulsanti sono posizionati rispetto al centro, quindi
// serve sapere su che tela erano stati disegnati l'ultimo frame.
render::Size experienceWin{0.0f, 0.0f};
bool         experienceOnMainWindow = true;

// --- FPS e diagnosi, solo thread di render ---
// FPS contati su finestre di mezzo secondo: piu' stabili di 1/dt e abbastanza
// pronti da vedere un calo quando avviene. Il calo conta come condizione
// (MZ-R01) solo se dura: un fotogramma lungo isolato non e' un problema.
double     fpsWindowT   = 0.0;
int        fpsWindowN   = 0;
double     fps          = 60.0;
double     lowFpsFor    = 0.0;
constexpr double kLowFpsThreshold = 30.0;
constexpr double kLowFpsHoldS     = 2.0;
diag::Code lastCode     = diag::Code::Landing;
bool       lastCodeSet  = false;
// Una condizione deve durare un po' prima di diventare "la" diagnosi: gli
// artefatti di movimento e i buchi di un frame vanno e vengono in un secondo,
// e ogni cambio finirebbe nel log e farebbe lampeggiare il pannello.
diag::Code pendingCode  = diag::Code::Landing;
double     pendingFor   = 0.0;
constexpr double kCodeDebounceS = 1.5;
double     sinceHeartbeat = 0.0;
constexpr double kHeartbeatS = 10.0;

/** wchar_t (UTF-32 su macOS) -> UTF-8, per scrivere nel log i testi con gli accenti. */
std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) {
        const auto c = static_cast<std::uint32_t>(*w);
        if (c < 0x80) {
            s.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (c >> 6)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (c >> 12)));
            s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (c >> 18)));
            s.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return s;
}

/** Riga di stato compatta: va nel log ogni 10 s e nello snapshot per il crash. */
std::string statusLine(const app::ControlState& st, diag::Code code, bool landing) {
    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "[stato] %s | fps=%.0f | %s | ble=%d pkt=%llu/%llu frames=%llu fresh=%d "
                  "fault=%d contact=%d | indice=%.3f vel=%.3f | %s",
                  diag::info(code).code, fps, narrow(phaseInWords(st, landing, zoom.locked()).c_str()).c_str(),
                  st.bleState, static_cast<unsigned long long>(st.rawPackets),
                  static_cast<unsigned long long>(st.validPackets),
                  static_cast<unsigned long long>(st.frames), st.signalFresh ? 1 : 0,
                  st.signalFault, st.contactOk ? 1 : 0, st.smoothedIndex, st.velocity,
                  st.replaying ? "riproduzione" : "fascia");
    return buf;
}

// --- passaggi fra le schermate ---------------------------------------------
// Scritti una volta sola perche' li chiamano sia il tasto sia il clic: se
// fossero due copie, il giorno che una acquisisce un azzeramento in piu'
// l'esperienza si comporterebbe in modo diverso a seconda di come e' stata
// avviata, ed e' il genere di differenza che non si nota provando.

/** Pagina d'ingresso -> accoglienza. */
void enterOnboarding() {
    g.landingVisible.store(false, std::memory_order_relaxed);
    g.onboardingVisible.store(true, std::memory_order_relaxed);
    onboardT = 0.0;
    // Lo zoom deve SEMPRE partire dal minimo: senza questo, il segnale gia'
    // arrivato mentre si sistemava la fascia (in banda adattiva l'esperienza
    // e' viva dal primo istante, vedi frame()) puo' aver spinto currentFocus_
    // avanti prima ancora che l'utente avesse scelto di iniziare - osservato
    // sul campo, si partiva gia' a 200x.
    g.resetHistory.store(true, std::memory_order_release);
}

/**
 * Accoglienza -> esperienza. Non fa niente finche' il pulsante e' spento:
 * e' l'unico punto in cui quella regola viene applicata, e vale sia per
 * INVIO sia per il clic.
 * @return true se e' passata davvero.
 */
bool enterExperience() {
    if (!onboardReady(onboardT)) return false;
    g.onboardingVisible.store(false, std::memory_order_relaxed);
    sessionT = 0.0;
    g.resetHistory.store(true, std::memory_order_release);
    return true;
}

/** Esperienza -> congedo (R tenuto premuto). */
void enterOffboarding() {
    g.onboardingVisible.store(false, std::memory_order_relaxed);
    g.offboardingVisible.store(true, std::memory_order_relaxed);
    offboardT = 0.0;
    restartHold.store(0.0, std::memory_order_relaxed);
    // La sessione si butta via QUI, non alla fine del congedo: cosi' la banda
    // adattiva ricomincia a riempirsi durante gli otto secondi di saluto
    // invece che dopo, e la persona seguente trova meno attesa. I campioni
    // raccolti con la fascia sul tavolo non la inquinano - senza contatto il
    // thread DSP non li passa alla banda (vedi dspThread).
    g.command.store(static_cast<int>(app::Command::RestartSession), std::memory_order_release);
    g.resetHistory.store(true, std::memory_order_release);
}

/** Congedo -> pagina d'ingresso: scade da solo, nessun tasto. */
void backToLanding() {
    g.offboardingVisible.store(false, std::memory_order_relaxed);
    g.landingVisible.store(true, std::memory_order_relaxed);
}

} // namespace

std::string debugLogPath() { return g.debugLogPath; }

void logLine(const std::string& msg) { g.pushBleLog(msg); }

std::string start(const StartOptions& opt) {
    g.running.store(true, std::memory_order_release);
    g.quitRequested.store(false, std::memory_order_release);
    // Ogni avvio riparte dalla pagina d'ingresso, anche se il processo era
    // gia' stato usato: start() e' il punto in cui l'esperienza ricomincia.
    g.landingVisible.store(true, std::memory_order_relaxed);
    // Va scritto PRIMA che parta il thread DSP, che lo legge una volta sola.
    g.adaptiveBand.store(opt.adaptiveBand, std::memory_order_relaxed);
    g.hudLevel.store(std::clamp(opt.hudLevel, 0, 2), std::memory_order_relaxed);

    g.openDebugLog(opt.debugDir);
    // Da qui in poi un crash lascia in coda al log il codice MZ-X01 con
    // l'ultimo stato noto (vedi crash.hpp).
    crash::install(g.debugLogPath);
    fpsWindowT = 0.0; fpsWindowN = 0; fps = 60.0; lowFpsFor = 0.0;
    lastCodeSet = false; sinceHeartbeat = kHeartbeatS;

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
    g.writeDebugLog(statusLine(st, lastCode, g.landingVisible.load(std::memory_order_relaxed)));
    g.closeDebugLog("fine sessione (chiusura regolare): bleState=" + std::to_string(st.bleState) +
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
            // Le schermate di apertura si attraversano una alla volta, in
            // ordine: ingresso -> accoglienza -> esperienza. Il congedo NON
            // ascolta INVIO: scade da solo, e un tasto premuto per sbaglio
            // salterebbe proprio l'istruzione di igienizzare la fascia.
            if (g.offboardingVisible.load(std::memory_order_relaxed)) return;
            if (g.landingVisible.load(std::memory_order_relaxed)) {
                enterOnboarding();
                return;
            }
            if (g.onboardingVisible.load(std::memory_order_relaxed)) {
                enterExperience();
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
            // solo le manopole ai default): tenuto premuto apposta - vedi
            // config::kRestartHoldS - perche' butta via la banda adattiva e i
            // progressi della sessione corrente, non solo la taratura.
            // Non torna dritto alla pagina d'ingresso: passa dal congedo, che
            // e' l'unico momento in cui si puo' chiedere di igienizzare la
            // fascia a chi la sta ancora togliendo.
            if (g.offboardingVisible.load(std::memory_order_relaxed)) return;
            enterOffboarding();
            g.pushBleLog("riavvio completo richiesto (R tenuto premuto)");
            return;
        case Key::H:
            // nascosto -> base -> esperto -> nascosto
            g.hudLevel.store((g.hudLevel.load(std::memory_order_relaxed) + 1) % 3,
                             std::memory_order_relaxed);
            return;
    }
}

void handleClick(float x, float y) {
    if (!initialized) return;
    // In doppio schermo il clic arriva dalla finestra dell'operatore, che
    // l'esperienza non la disegna affatto (mostra la sala di controllo): un
    // pulsante li' non c'e', e prendere per buone quelle coordinate vorrebbe
    // dire far partire l'esperienza cliccando su un grafico.
    if (!experienceOnMainWindow || experienceWin.width <= 0.0f) return;

    const auto dentro = [x, y](render::Rect b) {
        return x >= b.left && x <= b.right && y >= b.top && y <= b.bottom;
    };

    if (g.landingVisible.load(std::memory_order_relaxed)) {
        if (dentro(entryButtonRect(experienceWin, kLandingButtonDy))) enterOnboarding();
        return;
    }
    if (g.onboardingVisible.load(std::memory_order_relaxed)) {
        // enterExperience() rifiuta da sola finche' il pulsante e' spento:
        // il clic non e' una scorciatoia per saltare l'attesa.
        if (dentro(onboardButtonRect(experienceWin))) enterExperience();
        return;
    }
}

void setRestartHold(double progress) {
    restartHold.store(std::clamp(progress, 0.0, 1.0), std::memory_order_relaxed);
}

bool wantsQuit() { return g.quitRequested.load(std::memory_order_acquire); }

void frame(render::Renderer& r, double dt, const ProjectionState& proj) {
    if (!initialized) return;

    const auto st = g.state.read();
    const auto phase = static_cast<control::Phase>(st.phase);
    const bool showLanding  = g.landingVisible.load(std::memory_order_relaxed);
    const bool showOnboard  = g.onboardingVisible.load(std::memory_order_relaxed);
    const bool showOffboard = g.offboardingVisible.load(std::memory_order_relaxed);
    // "Nell'esperienza" vuol dire: nessuna delle tre schermate di contorno.
    const bool inExperience = !showLanding && !showOnboard && !showOffboard;

    // Gli orologi delle schermate che scadono da sole. Avanzano con lo stesso
    // dt del resto, quindi seguono il tempo davvero disegnato: se un
    // fotogramma si allunga, si allunga anche l'attesa, invece di scadere
    // mentre la schermata e' ancora ferma sul primo frame.
    if (showOnboard) {
        onboardT += dt;
    } else if (showOffboard) {
        offboardT += dt;
        if (offboardT >= config::kOffboardS) backToLanding();
    } else if (inExperience) {
        sessionT += dt;
    }

    const double velocity = velRender.push(st.velocity, dt, config::kVelRenderTauS);
    // Finche' si e' in una delle schermate di contorno l'esperienza non e'
    // "iniziata" per l'utente, anche se in banda adattiva il thread DSP e'
    // gia' in Interactive dal primo istante (vedi dspThread). Onboarding e'
    // l'unica fase per cui authorityVelocity torna sempre 0: usarla qui
    // impedisce che lo zoom derivi mentre ci si sistema la fascia o mentre si
    // legge l'accoglienza, cosi' non c'e' niente da annullare dopo.
    const auto zoomPhase = inExperience ? phase : control::Phase::Onboarding;
    zoom.update(velocity, dt, zoomPhase, st.phaseElapsed, g.tune);

    focusFrac += (st.calibDisplayTarget - focusFrac) * config::kCalibDisplayEma;
    animT += dt;

    // --- FPS ---
    fpsWindowT += dt;
    ++fpsWindowN;
    if (fpsWindowT >= 0.5) {
        fps = fpsWindowN / fpsWindowT;
        fpsWindowT = 0.0;
        fpsWindowN = 0;
    }
    lowFpsFor = fps < kLowFpsThreshold ? lowFpsFor + dt : 0.0;

    // --- diagnosi: nel log quando cambia, e un battito ogni 10 s ---
    // L'accoglienza vale come pagina d'ingresso: e' il momento in cui la
    // fascia viene indossata e accesa, quindi "non arrivano ancora dati" e'
    // la normalita' e non una condizione da segnalare.
    const diag::Code now =
        diag::primary(st, showLanding || showOnboard, lowFpsFor >= kLowFpsHoldS);
    if (now != pendingCode) { pendingCode = now; pendingFor = 0.0; }
    else                    { pendingFor += dt; }
    if (!lastCodeSet || (pendingCode != lastCode && pendingFor >= kCodeDebounceS)) {
        lastCode    = pendingCode;
        lastCodeSet = true;
        const auto& d = diag::info(lastCode);
        std::string msg = std::string("[") + d.code + "] " + narrow(d.title);
        if (d.action[0]) msg += " | cosa fare: " + narrow(d.action);
        g.writeDebugLog(msg);
    }
    const diag::Code code = lastCode;
    sinceHeartbeat += dt;
    if (sinceHeartbeat >= kHeartbeatS) {
        sinceHeartbeat = 0.0;
        const std::string s = statusLine(st, code, showLanding);
        g.writeDebugLog(s);
        crash::updateSnapshot(s.c_str());
    }

    if (g.resetHistory.exchange(false, std::memory_order_acq_rel)) {
        history.clear();
        zoom.reset();
        velRender.reset();
    }

    history.append(st, zoom.targetFocus(), zoom.currentFocus(), zoom.locked());

    const auto cf = zoom.crossfade();
    const bool showCard = inExperience && (phase == control::Phase::Onboarding);
    const double hold = restartHold.load(std::memory_order_relaxed);

    // L'esperienza cosi' come la vede il partecipante: pagina d'ingresso,
    // scheda di calibrazione (a schermo intero apposta: la foto dietro
    // distrarrebbe proprio nella fase che chiede piu' attenzione), altrimenti
    // la foto con la scala e - con la banda adattiva - il velo di ascolto
    // sopra, non una schermata al posto.
    const auto drawExperience = [&](render::Renderer& er) {
        if (showLanding) {
            drawLandingPage(er, st, animT);
        } else if (showOnboard) {
            drawOnboarding(er, onboardT, animT);
        } else if (showOffboard) {
            drawOffboarding(er, offboardT, animT);
        } else if (!showCard) {
            er.drawSprite(cf.activeIndex, static_cast<float>(cf.activeScale),
                         static_cast<float>(cf.activeAlpha));
            er.drawSprite(cf.activeIndex + 1, static_cast<float>(cf.nextScale),
                         static_cast<float>(cf.nextAlpha));
            drawProjectionScale(er, cf);
        } else {
            drawCalibrationCard(er, st, focusFrac, animT);
        }

        // Promemoria del riavvio: sopra tutto il resto, ma solo dentro
        // l'esperienza. Compare a sessione avviata da un po', oppure subito
        // se qualcuno sta gia' premendo R - in quel caso l'anello e' l'unica
        // conferma che il gesto sta funzionando.
        if (inExperience && (sessionT >= config::kRestartHintAfterS || hold > 0.0)) {
            drawRestartHint(er, hold, animT);
        }

        // Su che tela sono finiti i pulsanti, per handleClick(): si prende
        // dall'ultimo disegno vero invece di indovinarla, cosi' vale anche
        // quando la finestra e' stata ridimensionata in questo stesso frame.
        experienceWin = er.size();
    };

    experienceOnMainWindow = (proj.renderer == nullptr);

    if (proj.renderer) {
        // --- due schermi: la proiezione e' SOLO l'esperienza, lo schermo
        // dell'operatore e' SOLO la sala di controllo (nessuna foto: chi
        // guida vede i dati, il partecipante vede l'immagine). ---
        render::Renderer& pr = *proj.renderer;
        pr.begin(kPhotoBg);
        if (proj.choosing) drawScreenPicker(pr, proj.candidate, *proj.displays);
        else               drawExperience(pr);
        pr.end();

        // La sala di controllo si ridisegna a un terzo del refresh (~20 Hz):
        // i dati che mostra cambiano a 5 Hz, e disegnarla a 60 Hz costava meta'
        // del fotogramma - la proiezione, che e' quella che il pubblico guarda,
        // deve restare a refresh pieno. Saltando begin()/end() il layer della
        // finestra tiene l'ultima immagine.
        static unsigned dashboardTick = 0;
        if (++dashboardTick % 3 != 0) return;
        r.begin(kPhotoBg);
        drawControlRoom(r, st, zoom, cf, history, fps, code, showLanding);
    } else {
        // --- schermo unico: esperienza con il pannello operatore sopra ---
        r.begin(kPhotoBg);
        drawExperience(r);
        if (const int level = g.hudLevel.load(std::memory_order_relaxed); level > 0) {
            drawHud(r, st, zoom, cf, g.tune, level, fps, code, showLanding);
        }
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
