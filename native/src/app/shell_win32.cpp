// Shell Windows: finestre Win32, ciclo di render, tasti. La logica (DSP,
// calibrazione, disegno) vive in app/experience.cpp/.hpp, portabile.
//
// Gemello di app/shell_macos.mm, e sostituisce app/main.cpp - che conteneva
// LOGICA E SHELL insieme, 2073 righe in cui il disegno dell'esperienza e la
// gestione delle finestre erano lo stesso file. Da qui in poi le due
// piattaforme condividono experience.cpp e si differenziano solo qui.
//
// Doppio schermo: una seconda finestra senza bordo per il partecipante (solo
// immagine) quando ci sono almeno due monitor. Lo stato della scelta
// (displays/choosing/candidate/projIndex) vive qui, non in experience.cpp:
// e' gestione di finestre, non logica dell'esperienza - vedi ProjectionState
// in app/experience.hpp.
//
// NON ANCORA ESEGUITO SU UNA MACCHINA WINDOWS (stato all'8 settembre 2026):
// scritto portando riga per riga shell_macos.mm, che gira. Vale quello che
// dice il commento in experience.hpp sul doppio schermo, moltiplicato: qui
// non e' stato provato nemmeno il caso a schermo singolo.

#include <windows.h>

#ifdef DrawText
#undef DrawText
#endif

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include "app/crash.hpp"
#include "app/diagnostics.hpp"
#include "app/displays.hpp"
#include "app/experience.hpp"
#include "app/platform.hpp"
#include "config.hpp"
#include "render/renderer.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using namespace mz;

struct Options {
    bool         record = true;
    std::wstring replayPath;
    // Su questo ramo la banda adattiva e' il comportamento normale;
    // --calibrazione rimette quella a due fasi, per poterle confrontare nella
    // stessa giornata e sulla stessa testa.
    bool         adaptiveBand = true;
    bool         singleScreen = false;   // --schermo-singolo: ignora il secondo monitor
    // --proiezione-finestra: la proiezione in una finestra normale sullo
    // STESSO schermo, per provare la modalita' a due schermi senza un secondo
    // monitor (sviluppo e prove; in mostra non serve).
    bool         projWindowed = false;
    bool         listScreens  = false;   // --schermi: elenca i monitor ed esce
    // Pannello operatore all'avvio: 0 nascosto (default, per la mostra),
    // --pannello = 1 (base), --pannello-esperto = 2. Il tasto H cicla comunque.
    int          hudLevel     = 0;
    // --selftest: disegna un fotogramma completo su una finestra mai mostrata
    // e riporta l'esito col codice d'uscita, senza fascia e senza schermo.
    // Lo esegue package.bat prima di produrre l'archivio.
    bool         selfTest     = false;
    bool         badArg       = false;
    std::wstring badArgText;
};

// Dimensione della finestra dell'operatore quando esiste la proiezione, cioe'
// quando quella finestra non e' piu' l'esperienza ma solo la sala di controllo
// (vedi drawControlRoom in experience.cpp). Stessi numeri di shell_macos.mm:
// ogni pixel si paga a ogni fotogramma, e questa e' la finestra che nessuno
// guarda per l'estetica. E' anche il minimo che regge il contenuto - i tre
// grafici del segnale elaborato e le otto righe del blocco di stato.
constexpr int kControlRoomW = 700;
constexpr int kControlRoomH = 500;

/** Stato dello shell. Uno solo, come il processo: windowProc ci arriva da qui. */
struct Shell {
    HWND opHwnd   = nullptr;
    HWND projHwnd = nullptr;

    render::GraphicsCore graphics;
    render::Renderer     renderer;
    render::Renderer     projRenderer;

    Options opt;

    std::vector<app::Display> displays;
    bool                      choosing  = false;
    int                       candidate = -1;
    int                       projIndex = -1;

    // Pressione prolungata di R: Windows manda WM_KEYDOWN ripetuti, ma il
    // conto vero lo fa l'orologio - un autorepeat perso allunga il gesto
    // invece di sballare la soglia. Stessa scelta del timer in shell_macos.mm.
    bool                                  rHoldActive = false;
    bool                                  rHoldFired  = false;
    std::chrono::steady_clock::time_point rHoldStart{};

    std::chrono::steady_clock::time_point lastTick{};
    unsigned lastW = 0, lastH = 0;
    unsigned projLastW = 0, projLastH = 0;
};

Shell g;

/** Riga di comando -> Options. Stesse opzioni, stessi nomi, dello shell macOS. */
Options parseOptions(PWSTR cmdLine) {
    Options o;

    int     argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return o;

    for (int i = 0; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--senza-log") {
            o.record = false;
        } else if (a == L"--calibrazione") {
            o.adaptiveBand = false;
        } else if (a == L"--riproduci" && i + 1 < argc) {
            o.replayPath = argv[++i];
        } else if (a == L"--schermo-singolo") {
            o.singleScreen = true;
        } else if (a == L"--schermi") {
            o.listScreens = true;
        } else if (a == L"--proiezione-finestra") {
            o.projWindowed = true;
        } else if (a == L"--pannello") {
            o.hudLevel = 1;
        } else if (a == L"--pannello-esperto") {
            o.hudLevel = 2;
        } else if (a == L"--selftest") {
            o.selfTest = true;
        } else {
            o.badArg     = true;
            o.badArgText = a;
        }
    }
    LocalFree(argv);

    if (!o.replayPath.empty()) o.record = false;
    return o;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr,
                                      0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

/**
 * Errore che impedisce di partire: finestra con codice, spiegazione e cosa
 * fare, e la stessa riga appesa a debug\mindzoom-avvio.log - il log di
 * sessione a questo punto non esiste ancora, e un errore d'avvio che non
 * lascia traccia e' il piu' difficile da raccontare al telefono.
 */
void fatal(app::diag::Code code, const std::wstring& detail, const std::wstring& debugDir) {
    const auto& d = app::diag::info(code);

    const std::wstring title  = L"[" + std::wstring(d.code, d.code + std::strlen(d.code)) + L"] " +
                                d.title;
    const std::wstring action = std::wstring(L"Cosa fare: ") + d.action;
    const std::wstring body   = detail.empty() ? action : (detail + L"\n\n" + action);

    if (!debugDir.empty()) {
        app::platform::ensureDirectory(debugDir);
        const std::wstring path = debugDir + L"\\mindzoom-avvio.log";

        // _wfopen e localtime fanno scattare C4996 ("usa la variante _s"). Le
        // varianti sicure non danno niente in piu' qui - il file e' un
        // artefatto nostro, e siamo su un thread solo, prima che ne parta
        // qualunque altro - e cambiarle vorrebbe dire codice diverso da quello
        // che gira su macOS a poche righe di distanza.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        if (FILE* f = _wfopen(path.c_str(), L"a")) {
            const std::time_t t = std::time(nullptr);
            char when[32]{};
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            std::fprintf(f, "%s %s\n  %s\n", when, narrow(title).c_str(), narrow(body).c_str());
            std::fclose(f);
        }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }

    MessageBoxW(g.opHwnd, body.c_str(), title.c_str(), MB_ICONERROR | MB_OK);
}

/**
 * --schermi: elenco dei monitor rilevati, senza aprire nulla.
 *
 * L'eseguibile e' compilato per il sottosistema grafico (niente console
 * propria), quindi stdout da solo non arriva da nessuna parte quando lo si
 * lancia da cmd. AttachConsole(ATTACH_PARENT_PROCESS) aggancia la console di
 * chi ha lanciato: da terminale il testo compare li', da doppio clic si
 * ripiega su una finestra di messaggio.
 */
int reportScreens() {
    const auto displays = app::enumerateDisplays();

    std::wstring out = L"Schermi rilevati: " + std::to_wstring(displays.size()) + L"\n\n";
    for (std::size_t i = 0; i < displays.size(); ++i) {
        out += std::to_wstring(i + 1) + L".  " + displays[i].describe() + L"\n     " +
               displays[i].deviceName + L"\n";
    }

    if (displays.size() < 2) {
        out += L"\nCon un solo schermo il programma resta a finestra unica.\n";
    } else {
        const int stored = app::matchStoredChoice(displays);
        out += (stored >= 0) ? (L"\nProiezione: schermo " + std::to_wstring(stored + 1) +
                                L" (memorizzato)\n")
                             : L"\nProiezione: da scegliere al prossimo avvio\n";
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); h && h != INVALID_HANDLE_VALUE) {
            const std::string utf8 = narrow(out);
            DWORD             written = 0;
            WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        FreeConsole();
    } else {
        MessageBoxW(nullptr, out.c_str(), L"Mind Zoom · schermi", MB_ICONINFORMATION);
    }
    return 0;
}

/** Sposta la finestra di proiezione sullo schermo indicato, a tutto schermo. */
void placeProjection(int index) {
    if (!g.projHwnd || index < 0 || index >= static_cast<int>(g.displays.size())) return;

    const app::ScreenRect& b = g.displays[static_cast<std::size_t>(index)].bounds;
    SetWindowPos(g.projHwnd, HWND_TOPMOST, b.left, b.top, b.right - b.left, b.bottom - b.top,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    g.candidate = index;
}

/** Conferma lo schermo di proiezione e ricorda la scelta per la prossima volta. */
void confirmProjection(int index) {
    if (index < 0 || index >= static_cast<int>(g.displays.size())) return;

    g.projIndex = index;
    g.choosing  = false;
    placeProjection(index);
    app::saveDisplayChoice(g.displays[static_cast<std::size_t>(index)].deviceName,
                           app::layoutSignature(g.displays));

    // La finestra dell'operatore deve tornare a ricevere i tasti: durante la
    // scelta poteva averli persi.
    if (g.opHwnd) SetForegroundWindow(g.opHwnd);
}

/** La finestra dell'operatore diventa la sala di controllo: piu' piccola. */
void applyControlRoomGeometry() {
    if (!g.opHwnd) return;

    RECT r{0, 0, kControlRoomW, kControlRoomH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g.opHwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowTextW(g.opHwnd, L"Mind Zoom · sala di controllo");

    // Al centro dello schermo su cui gia' sta.
    if (HMONITOR mon = MonitorFromWindow(g.opHwnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            const int w = r.right - r.left;
            const int h = r.bottom - r.top;
            SetWindowPos(g.opHwnd, nullptr,
                         mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2,
                         mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

/**
 * La finestra di proiezione, creandola al momento se manca.
 *
 * Prima esisteva solo se all'avvio c'erano gia' due schermi: collegarne uno a
 * programma aperto non la faceva comparire, e lanciando la DEMO - che passa
 * --schermo-singolo di proposito - il doppio schermo era irraggiungibile senza
 * riavviare. Era anche il motivo per cui il tasto P non poteva essere
 * annunciato nel pannello operatore: li' non avrebbe fatto niente.
 *
 * Creandola qui, P diventa vero ovunque. Il flag --schermo-singolo resta
 * rispettato all'AVVIO; premere P e' una richiesta esplicita e successiva, e
 * vince.
 */
bool ensureProjectionWindow() {
    if (g.projHwnd) return true;

    g.displays = app::enumerateDisplays();
    if (g.displays.size() < 2) return false;

    g.projHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"MindZoomProjection",
                                 L"Mind Zoom", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (!g.projHwnd) return false;

    if (!g.projRenderer.init(g.graphics, g.projHwnd)) {
        // Stessa politica dell'avvio: la proiezione e' un di piu', se non si
        // inizializza si resta a schermo singolo invece di negare l'esperienza.
        DestroyWindow(g.projHwnd);
        g.projHwnd = nullptr;
        return false;
    }

    applyControlRoomGeometry();
    return true;
}

void reopenPicker() {
    if (!ensureProjectionWindow()) return;
    g.choosing = true;
    placeProjection(g.projIndex >= 0 ? g.projIndex : 1);
}

/**
 * Tasti della scelta schermo: frecce, INVIO e cifre hanno un altro significato
 * mentre si sceglie, e vanno intercettati PRIMA di experience::handleKey.
 * @return true se il tasto e' stato consumato qui.
 */
bool handlePickerKey(WPARAM wp) {
    if (!g.choosing) return false;
    const int n = static_cast<int>(g.displays.size());
    if (n <= 0) return false;

    const int cur = (g.candidate < 0) ? 0 : g.candidate;

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
            break;
    }

    // Anche i tasti numerici: con piu' di due schermi e' piu' rapido.
    if (wp >= '1' && wp < static_cast<WPARAM>('1' + n)) {
        confirmProjection(static_cast<int>(wp - '1'));
        return true;
    }
    return false;
}

/**
 * Un giro di disegno. Chiamato dal ciclo di messaggi, non da WM_PAINT: il
 * ciclo di gioco resta padrone del tempo, come su macOS.
 */
void frame() {
    if (app::experience::wantsQuit()) {
        if (g.opHwnd) PostMessageW(g.opHwnd, WM_CLOSE, 0, 0);
        return;
    }

    // Avanzamento della pressione di R: lo misura lo shell, l'anello lo disegna
    // experience.cpp. Vedi config::kRestartHoldS - la stessa costante per il
    // gesto e per l'anello che lo rappresenta.
    if (g.rHoldActive && !g.rHoldFired) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g.rHoldStart).count();
        const double progress = elapsed / config::kRestartHoldS;
        if (progress < 1.0) {
            app::experience::setRestartHold(progress);
        } else {
            // Anello pieno nel fotogramma in cui parte il riavvio: senza,
            // l'ultimo spicchio non si vede mai e il gesto sembra scattare
            // "quasi" alla fine.
            app::experience::setRestartHold(1.0);
            g.rHoldFired = true;
            app::experience::handleKey(app::experience::Key::RHold);
        }
    }

    RECT rc{};
    if (g.opHwnd && GetClientRect(g.opHwnd, &rc)) {
        const unsigned w = static_cast<unsigned>(rc.right - rc.left);
        const unsigned h = static_cast<unsigned>(rc.bottom - rc.top);
        if (w != g.lastW || h != g.lastH) {
            g.lastW = w;
            g.lastH = h;
            g.renderer.resize(w, h);
        }
    }

    if (g.projHwnd && GetClientRect(g.projHwnd, &rc)) {
        const unsigned w = static_cast<unsigned>(rc.right - rc.left);
        const unsigned h = static_cast<unsigned>(rc.bottom - rc.top);
        if (w != g.projLastW || h != g.projLastH) {
            g.projLastW = w;
            g.projLastH = h;
            g.projRenderer.resize(w, h);
        }
    }

    const auto   now = std::chrono::steady_clock::now();
    const double dt  = std::chrono::duration<double>(now - g.lastTick).count();
    g.lastTick = now;

    app::experience::ProjectionState proj;
    if (g.projHwnd) {
        proj.renderer  = &g.projRenderer;
        proj.choosing  = g.choosing;
        proj.candidate = g.candidate;
        proj.displays  = &g.displays;
    }
    app::experience::frame(g.renderer, dt, proj);
}

/** Traduce un WM_KEYDOWN in un Key dell'esperienza, e lo consegna. */
void onKeyDown(WPARAM wp, LPARAM lp) {
    using app::experience::Key;

    // Durante la scelta dello schermo frecce/INVIO/cifre valgono altro.
    if (handlePickerKey(wp)) return;

    switch (wp) {
        case VK_ESCAPE: app::experience::handleKey(Key::Escape); return;
        case VK_RETURN: app::experience::handleKey(Key::Enter);  return;
        case VK_LEFT:   app::experience::handleKey(Key::Left);   return;
        case VK_RIGHT:  app::experience::handleKey(Key::Right);  return;
        case VK_UP:     app::experience::handleKey(Key::Up);     return;
        case VK_DOWN:   app::experience::handleKey(Key::Down);   return;
        default: break;
    }

    const bool shift  = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool repeat = (lp & 0x40000000) != 0;   // bit 30: autorepeat

    if (wp == 'R') {
        // R e' l'unico tasto con due azioni distinte (tap contro tenuto
        // premuto): l'azione vera parte da WM_KEYUP o dal conto in frame(),
        // non da qui - altrimenti un tap normale scatterebbe subito anche
        // l'azione "hold". Gli autorepeat di Windows si ignorano, il tempo lo
        // misuriamo noi.
        if (!repeat && !g.rHoldActive) {
            g.rHoldActive = true;
            g.rHoldFired  = false;
            g.rHoldStart  = std::chrono::steady_clock::now();
            app::experience::setRestartHold(0.0);
        }
        return;
    }

    switch (wp) {
        case 'S': app::experience::handleKey(shift ? Key::ShiftS : Key::S); return;
        case 'L': app::experience::handleKey(Key::L); return;
        case 'H': app::experience::handleKey(Key::H); return;
        case 'Q': app::experience::handleKey(Key::Q); return;
        case 'D': app::experience::handleKey(Key::D); return;
        case 'B': app::experience::handleKey(Key::B); return;
        case 'C': app::experience::handleKey(Key::C); return;
        case 'X': app::experience::handleKey(Key::X); return;
        case 'V': app::experience::handleKey(Key::V); return;
        case 'M': app::experience::handleKey(Key::M); return;
        case 'K': app::experience::handleKey(Key::K); return;
        case 'E': app::experience::handleKey(shift ? Key::ShiftE : Key::E); return;
        case 'P':
            // Non passa da experience::handleKey: la scelta schermo e'
            // gestione di finestre, non fa parte di Key. Stesso tasto dello
            // shell macOS, e lo stesso che il pannello annuncia.
            reopenPicker();
            return;
        default: return;
    }
}

void onKeyUp(WPARAM wp) {
    if (wp != 'R') return;

    const bool alreadyFired = g.rHoldFired;
    g.rHoldActive = false;
    g.rHoldFired  = false;
    app::experience::setRestartHold(0.0);

    // Se il conto non e' arrivato in fondo prima del rilascio, era un tap
    // normale: l'azione "hold" e' gia' partita da sola, non va duplicata.
    if (!alreadyFired) app::experience::handleKey(app::experience::Key::R);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            onKeyDown(wp, lp);
            return 0;

        case WM_KEYUP:
            onKeyUp(wp);
            return 0;

        case WM_KILLFOCUS:
            // Un cambio di focus a meta' pressione (Alt+Tab, un dialogo)
            // lascerebbe il conto armato, e un R tenuto premuto altrove
            // potrebbe ancora far scattare il riavvio.
            g.rHoldActive = false;
            g.rHoldFired  = false;
            app::experience::setRestartHold(0.0);
            return 0;

        case WM_LBUTTONDOWN:
            // Coordinate della finestra, origine in alto a sinistra: le stesse
            // in cui disegna il Renderer. Solo la finestra dell'operatore -
            // sulla proiezione i pulsanti non ci sono.
            if (hwnd == g.opHwnd) {
                app::experience::handleClick(static_cast<float>(GET_X_LPARAM(lp)),
                                             static_cast<float>(GET_Y_LPARAM(lp)));
            }
            return 0;

        case WM_DISPLAYCHANGE:
            // Uno schermo scollegato o aggiunto: la finestra di proiezione
            // potrebbe essere finita fuori dal desktop visibile. Si riparte
            // dalla scelta.
            g.displays = app::enumerateDisplays();
            if (g.projHwnd && g.displays.size() >= 2) {
                const int match = app::matchStoredChoice(g.displays);
                if (match >= 0) {
                    confirmProjection(match);
                } else {
                    g.choosing = true;
                    placeProjection(g.displays.size() > 1 ? 1 : 0);
                }
            }
            return 0;

        case WM_DPICHANGED:
            // Finestra trascinata su uno schermo con scala diversa. Windows
            // propone il rettangolo giusto in lp: seguirlo e' l'unico modo di
            // non finire di dimensione sbagliata. Riguarda la finestra
            // dell'operatore; quella di proiezione e' incollata al suo monitor.
            if (hwnd == g.opHwnd) {
                const RECT* suggerito = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, suggerito->left, suggerito->top,
                             suggerito->right - suggerito->left,
                             suggerito->bottom - suggerito->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;

        case WM_ERASEBKGND:
            // Il renderer ridipinge tutto a ogni fotogramma: lasciare che
            // Windows cancelli lo sfondo per conto suo produce sfarfallio.
            return 1;

        case WM_DESTROY:
            // Solo la finestra dell'operatore chiude l'applicazione: la
            // proiezione viene distrutta insieme, non e' lei a comandare.
            if (hwnd == g.opHwnd) PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * Consapevolezza del DPI, prima di qualunque finestra.
 *
 * Senza questa dichiarazione Windows tratta il processo come scritto per il
 * 1996 e gli mente sulle coordinate. A scala 100% non si nota; su un portatile
 * 4K al 150% - la configurazione normale di un portatile moderno - l'immagine
 * viene disegnata a risoluzione ridotta e poi ingrandita dal sistema, e con
 * due schermi a scala diversa i rettangoli di GetMonitorInfo non corrispondono
 * piu' a dove stanno davvero i monitor: la proiezione puo' finire su quello
 * sbagliato.
 */
void enableDpiAwareness() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto fn = reinterpret_cast<SetCtx>(
                reinterpret_cast<void*>(
                    GetProcAddress(user32, "SetProcessDpiAwarenessContext")))) {
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
    }
    // Ripiego per Windows 8.1 e per i 10 anteriori al 1703.
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        using SetAw = HRESULT(WINAPI*)(int);
        if (auto fn = reinterpret_cast<SetAw>(
                reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")))) {
            fn(2);   // PROCESS_PER_MONITOR_DPI_AWARE
            FreeLibrary(shcore);
            return;
        }
        FreeLibrary(shcore);
    }
    SetProcessDPIAware();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    (void)showCmd;

    g.opt = parseOptions(cmdLine);

    if (g.opt.badArg && !g.opt.selfTest) {
        MessageBoxW(nullptr,
                    (L"Opzione non riconosciuta: " + g.opt.badArgText + L"\n\n"
                     L"Opzioni disponibili:\n"
                     L"  --riproduci FILE.mzr    rigioca una sessione registrata,\n"
                     L"                          senza usare la fascia\n"
                     L"  --senza-log             non registrare questa sessione\n"
                     L"  --calibrazione          calibrazione a due fasi invece\n"
                     L"                          della banda adattiva\n"
                     L"  --schermi               elenca i monitor rilevati ed esce\n"
                     L"  --schermo-singolo       non usare il secondo schermo\n"
                     L"  --proiezione-finestra   proiezione simulata in finestra\n"
                     L"  --pannello              pannello operatore all'avvio\n"
                     L"  --pannello-esperto      pannello operatore, tutte le letture\n\n"
                     L"Senza opzioni il programma registra da solo la sessione.")
                        .c_str(),
                    L"Mind Zoom", MB_ICONINFORMATION);
        return 1;
    }

    // PRIMA di qualunque finestra, e prima di enumerare gli schermi.
    enableDpiAwareness();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    if (g.opt.listScreens) return reportScreens();

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

    g.opHwnd = CreateWindowExW(0, wc.lpszClassName, L"Mind Zoom", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr,
                               instance, nullptr);
    if (!g.opHwnd) return 1;

    // --- schermi ---
    // Col selftest niente proiezione: la finestra resta una, mai mostrata.
    g.displays = app::enumerateDisplays();
    const bool wantDual = !g.opt.selfTest && !g.opt.singleScreen && g.displays.size() >= 2;

    if (g.opt.selfTest) {
        // niente seconda finestra
    } else if (g.opt.projWindowed) {
        // Proiezione simulata: finestra normale sullo stesso schermo. A
        // differenza di quella vera puo' prendere il focus, e i tasti devono
        // funzionare lo stesso - stessa windowProc, quindi funzionano.
        g.projHwnd = CreateWindowExW(0, wc.lpszClassName, L"Mind Zoom · proiezione (simulata)",
                                     WS_OVERLAPPEDWINDOW, 640, 0, 800, 500, nullptr, nullptr,
                                     instance, nullptr);
        if (g.projHwnd) ShowWindow(g.projHwnd, SW_SHOW);
    } else if (wantDual) {
        // Senza bordo, sopra tutto, e WS_EX_NOACTIVATE perche' non rubi mai il
        // focus: i tasti restano tutti alla finestra dell'operatore.
        g.projHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, pc.lpszClassName,
                                     L"Mind Zoom", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr,
                                     instance, nullptr);
    }

    // La proiezione esiste: la finestra principale non mostra piu' l'esperienza
    // ma la sala di controllo, e allora si rimpicciolisce.
    if (g.projHwnd) applyControlRoomGeometry();

    // Quello che l'app LEGGE sta accanto all'eseguibile (assets, font);
    // quello che SCRIVE sta in %LOCALAPPDATA%\MindZoom - l'eseguibile puo'
    // stare in Programmi, dove un utente non amministratore non scrive.
    // Vedi platform::dataDirectory().
    const std::wstring exeDir    = app::platform::exeDirectory();
    const std::wstring assetsDir = exeDir + L"\\assets";
    const std::wstring dataDir   = [&exeDir] {
        const std::wstring d = app::platform::dataDirectory();
        return d.empty() ? exeDir : d;
    }();
    const std::wstring recordingsDir = dataDir + L"\\registrazioni";
    const std::wstring debugDir      = dataDir + L"\\debug";
    app::platform::ensureDirectory(recordingsDir);
    app::platform::ensureDirectory(debugDir);

    // Le immagini vanno decodificate PRIMA di creare i render target: e'
    // createTarget() a caricarne le bitmap di device, e se le sorgenti non ci
    // sono ancora ne carica zero. Il risultato non e' un errore, e' un
    // programma che gira senza mostrare nulla.
    if (!g.graphics.init() ||
        !g.graphics.loadSprites(assetsDir, config::kScaleLabels.data(), config::kTotalImages)) {
        std::wstring nomi;
        for (int i = 0; i < config::kTotalImages; ++i) {
            if (i > 0) nomi += L", ";
            nomi += std::to_wstring(config::kScaleLabels[i]);
        }
        if (g.opt.selfTest) return 3;   // senza finestre: l'esito e' il codice d'uscita
        fatal(app::diag::Code::ImagesMissing,
              L"Cercate in:\n" + assetsDir + L"\n\nServono " +
                  std::to_wstring(config::kTotalImages) +
                  L" immagini in .jpg, .webp o .png, chiamate con l'ingrandimento: " + nomi +
                  L".\n\nSe i file ci sono ma sono .webp, su Windows 10 puo' mancare il "
                  L"codec: usa gli stessi file convertiti in .jpg (li produce mz_convert).",
              debugDir);
        return 3;
    }

    // Best-effort: se i font non si trovano o non si registrano, il renderer
    // ripiega da solo sul font di sistema. Non e' un motivo per rifiutarsi di
    // partire.
    g.graphics.loadFonts(exeDir + L"\\fonts");

    // Col selftest la finestra non si mostra mai: il render target si crea
    // comunque (Direct2D disegna su una finestra nascosta senza lamentarsi),
    // ed e' proprio la catena grafica che si vuole verificare.
    if (!g.opt.selfTest) {
        ShowWindow(g.opHwnd, SW_SHOW);
        SetForegroundWindow(g.opHwnd);
    }

    if (!g.renderer.init(g.graphics, g.opHwnd)) {
        if (g.opt.selfTest) return 2;
        fatal(app::diag::Code::RenderInit, L"Inizializzazione di Direct2D fallita.", debugDir);
        return 2;
    }

    if (g.opt.selfTest) {
        const bool drawn = app::experience::selfTest(g.renderer);
        g.renderer.shutdown();
        g.graphics.shutdown();
        DestroyWindow(g.opHwnd);
        CoUninitialize();
        return drawn ? 0 : 4;
    }

    if (g.projHwnd && !g.projRenderer.init(g.graphics, g.projHwnd)) {
        // La proiezione e' un di piu': se non si inizializza si continua a
        // schermo singolo invece di negare l'esperienza.
        DestroyWindow(g.projHwnd);
        g.projHwnd = nullptr;
    }

    app::experience::StartOptions startOpt;
    startOpt.assetsDir     = assetsDir;
    startOpt.recordingsDir = recordingsDir;
    startOpt.record        = g.opt.record;
    startOpt.replayPath    = g.opt.replayPath;
    startOpt.debugDir      = debugDir;
    startOpt.adaptiveBand  = g.opt.adaptiveBand;
    startOpt.hudLevel      = g.opt.hudLevel;

    const std::string err = app::experience::start(startOpt);
    if (!err.empty()) {
        const std::wstring wide(err.begin(), err.end());
        fatal(app::diag::Code::StartFailed, L"Dettaglio: " + wide, debugDir);
        return 4;
    }

    // --- scelta dello schermo di proiezione ---
    if (g.projHwnd && !g.opt.projWindowed) {
        const int stored = app::matchStoredChoice(g.displays);
        if (stored >= 0) {
            confirmProjection(stored);
        } else {
            // Candidato di partenza: il primo schermo NON primario, quasi
            // sempre quello giusto. Resta comunque da confermare guardando.
            g.choosing = true;
            placeProjection(1);
        }
    }

    // Geometria vera, nel log: gli fps da soli non si sanno interpretare
    // (vedi experience::logLine). Sono i PIXEL a decidere quanto costa un
    // fotogramma, e con la scala di Windows punti e pixel non coincidono.
    {
        RECT rc{};
        GetClientRect(g.opHwnd, &rc);
        // GetDpiForSystem sarebbe piu' diretta ma esiste solo da Windows 10
        // 1607, e nominarla qui creerebbe una dipendenza di IMPORT: su un
        // Windows piu' vecchio l'eseguibile non partirebbe affatto, con un
        // errore del loader invece che di programma. GetDeviceCaps c'e'
        // sempre, ed e' lo stesso ripiego che usa il renderer.
        HDC         dc    = GetDC(nullptr);
        const float dpi   = dc ? static_cast<float>(GetDeviceCaps(dc, LOGPIXELSX)) : 96.0f;
        if (dc) ReleaseDC(nullptr, dc);
        const float scale = (dpi > 0.0f) ? dpi / 96.0f : 1.0f;
        const float w     = static_cast<float>(rc.right - rc.left);
        const float h     = static_cast<float>(rc.bottom - rc.top);
        char        buf[256];
        std::snprintf(buf, sizeof buf,
                      "grafica: finestra %.0fx%.0f px, scala %.2fx (%.0f DPI) -> %.0fx%.0f pt "
                      "(%.1f Mpx) | proiezione=%d",
                      w, h, scale, dpi, w / scale, h / scale, w * h / 1.0e6,
                      g.projHwnd ? 1 : 0);
        app::experience::logLine(buf);
    }

    g.lastTick = std::chrono::steady_clock::now();

    // Ciclo di messaggi e di disegno insieme: si smaltiscono i messaggi in
    // attesa, poi si disegna un fotogramma. Il ritmo lo detta la presentazione
    // di Direct2D, che su un HwndRenderTarget e' sincronizzata col refresh -
    // e' l'equivalente del CVDisplayLink dello shell macOS. Senza quel freno
    // questo ciclo girerebbe a vuoto bruciando un core.
    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        frame();
    }

    app::experience::stop();
    g.renderer.shutdown();
    g.projRenderer.shutdown();
    g.graphics.shutdown();
    CoUninitialize();
    return 0;
}
