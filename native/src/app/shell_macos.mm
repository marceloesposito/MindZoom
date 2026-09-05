// Shell macOS: finestra Cocoa, ciclo di render, tasti. La logica (DSP,
// calibrazione, disegno) vive in app/experience.cpp/.hpp, portabile.
//
// Doppio schermo: come main.cpp (Windows), una seconda finestra senza bordo
// per il partecipante (solo immagine) quando ci sono almeno due monitor. Lo
// stato della scelta (displays/choosing/candidate/projIndex) vive qui, non in
// experience.cpp: e' gestione di finestre, non logica dell'esperienza - vedi
// ProjectionState in app/experience.hpp.

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>

#include "app/diagnostics.hpp"
#include "app/displays.hpp"
#include "app/experience.hpp"
#include "app/platform.hpp"
#include "config.hpp"
#include "render/renderer.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

/** argv (UTF-8) -> std::wstring (UTF-32 su macOS), come altrove nel port. */
std::wstring toWide(const char* utf8) {
    NSString* s = [NSString stringWithUTF8String:utf8];
    if (!s) return {};
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (!d) return {};
    const auto* p = static_cast<const wchar_t*>(d.bytes);
    return std::wstring(p, d.length / sizeof(wchar_t));
}

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
    bool         listScreens  = false;   // --schermi: elenca i monitor e esce
    // Pannello operatore all'avvio: 0 nascosto (default, per la mostra),
    // --pannello = 1 (base), --pannello-esperto = 2. Il tasto H cicla comunque.
    int          hudLevel     = 0;
};

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--senza-log") {
            o.record = false;
        } else if (a == "--calibrazione") {
            o.adaptiveBand = false;
        } else if (a == "--riproduci" && i + 1 < argc) {
            o.replayPath = toWide(argv[++i]);
        } else if (a == "--schermo-singolo") {
            o.singleScreen = true;
        } else if (a == "--schermi") {
            o.listScreens = true;
        } else if (a == "--proiezione-finestra") {
            o.projWindowed = true;
        } else if (a == "--pannello") {
            o.hudLevel = 1;
        } else if (a == "--pannello-esperto") {
            o.hudLevel = 2;
        }
    }
    if (!o.replayPath.empty()) o.record = false;
    return o;
}

/**
 * Errore che impedisce di partire: finestra con codice, spiegazione e cosa
 * fare, e la stessa riga appesa a debug/mindzoom-avvio.log - il log di
 * sessione a questo punto non esiste ancora, e un errore d'avvio che non
 * lascia traccia e' il piu' difficile da raccontare al telefono.
 */
void fatal(mz::app::diag::Code code, NSString* detail, const std::wstring& debugDir) {
    const auto& d = mz::app::diag::info(code);
    NSString* title  = [NSString stringWithFormat:@"[%s] %ls", d.code, d.title];
    NSString* action = [NSString stringWithFormat:@"Cosa fare: %ls", d.action];
    NSString* body   = detail.length ? [NSString stringWithFormat:@"%@\n\n%@", detail, action]
                                     : action;

    if (!debugDir.empty()) {
        mz::app::platform::ensureDirectory(debugDir);
        const std::string path = std::string(debugDir.begin(), debugDir.end()) +
                                 "/mindzoom-avvio.log";
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            const std::time_t t = std::time(nullptr);
            char when[32]{};
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            std::fprintf(f, "%s %s\n  %s\n", when, title.UTF8String, body.UTF8String);
            std::fclose(f);
        }
    }

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = body;
    alert.alertStyle = NSAlertStyleCritical;
    [alert runModal];
}

/** --schermi: elenco dei monitor rilevati, senza aprire nulla. Su stdout: si usa da terminale. */
int reportScreens() {
    const auto displays = mz::app::enumerateDisplays();

    std::printf("Schermi rilevati: %zu\n\n", displays.size());
    for (std::size_t i = 0; i < displays.size(); ++i) {
        const std::wstring desc = displays[i].describe();
        const std::string  descNarrow(desc.begin(), desc.end());
        const std::string  name(displays[i].deviceName.begin(), displays[i].deviceName.end());
        std::printf("%zu.  %s\n     %s\n", i + 1, descNarrow.c_str(), name.c_str());
    }

    if (displays.size() < 2) {
        std::printf("\nCon un solo schermo il programma resta a finestra unica.\n");
    } else {
        const int stored = mz::app::matchStoredChoice(displays);
        if (stored >= 0) {
            std::printf("\nProiezione: schermo %d (memorizzato)\n", stored + 1);
        } else {
            std::printf("\nProiezione: da scegliere al prossimo avvio\n");
        }
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Vista: riceve i tasti, la finestra scrive direttamente sul suo layer
// (Renderer::end() lo fa gia', vedi render/renderer_macos.mm).
// ---------------------------------------------------------------------------

// Quanto tenere premuto R prima che scatti il riavvio completo invece del
// semplice reset delle manopole. Ne' cosi' corto da scattare per sbaglio su
// un tap normale, ne' cosi' lungo da sembrare che il tasto non risponda.
static const NSTimeInterval kRHoldSeconds = 0.9;

// ---------------------------------------------------------------------------
// Delegate: dichiarato qui (l'implementazione resta piu' sotto, dopo MZView)
// perche' MZView deve poter chiamare handlePickerKeyCode:chars: - i tasti
// della scelta schermo vanno intercettati PRIMA che arrivino a
// experience::handleKey, esattamente come handlePickerKey() in main.cpp.
// ---------------------------------------------------------------------------

@class MZView;

@interface MZAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow*                _window;
    MZView*                  _view;
    // Guida il ciclo di disegno agganciato al refresh reale dello schermo -
    // vedi il commento su -startDisplayLinkForScreen: per il perche' non e' un NSTimer.
    CVDisplayLinkRef         _displayLink;
    mz::render::GraphicsCore _graphics;
    mz::render::Renderer     _renderer;
    NSSize                   _lastSize;
    std::chrono::steady_clock::time_point _lastTick;
    Options                  _opt;

    // --- doppio schermo, vedi il commento in cima al file ---
    std::vector<mz::app::Display> _displays;
    NSWindow*                     _projWindow;
    MZView*                       _projView;
    mz::render::Renderer          _projRenderer;
    NSSize                        _projLastSize;
    BOOL                          _choosing;
    int                           _candidate;
    int                           _projIndex;
}

- (instancetype)initWithOptions:(Options)opt;

/** true se il tasto e' stato consumato dalla scelta schermo (frecce/INVIO/cifre). */
- (BOOL)handlePickerKeyCode:(unsigned short)keyCode chars:(NSString*)chars;

/** Tasto P: riapre la scelta senza riavviare, solo se la proiezione esiste gia'. */
- (void)reopenPicker;

/** Avvia (o riavvia) il CVDisplayLink agganciato al refresh dello schermo dato. */
- (void)startDisplayLinkForScreen:(NSScreen*)screen;

/** La finestra e' passata a un altro schermo: il link deve seguirla. */
- (void)windowDidChangeScreen:(NSNotification*)note;

@end

@interface MZView : NSView {
    NSTimer* _rHoldTimer;
    BOOL     _rHoldFired;
}
// Debole: il delegate possiede la vista, non il contrario. Serve solo per
// intercettare i tasti della scelta schermo (frecce/INVIO/cifre) PRIMA che
// arrivino a experience::handleKey - vedi handlePickerKeyCode:chars: sotto.
@property (nonatomic, weak) MZAppDelegate* controller;
@end

@implementation MZView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }

- (void)startRHold {
    [_rHoldTimer invalidate];
    _rHoldFired = NO;
    _rHoldTimer = [NSTimer scheduledTimerWithTimeInterval:kRHoldSeconds
                                                    target:self
                                                  selector:@selector(rHoldFired:)
                                                  userInfo:nil
                                                   repeats:NO];
}

- (void)rHoldFired:(NSTimer*)timer {
    (void)timer;
    _rHoldFired = YES;
    _rHoldTimer = nil;
    mz::app::experience::handleKey(mz::app::experience::Key::RHold);
}

- (void)cancelRHold {
    [_rHoldTimer invalidate];
    _rHoldTimer = nil;
    _rHoldFired = NO;
}

- (BOOL)resignFirstResponder {
    // Altrimenti un cambio di focus a meta' pressione (Cmd+Tab, un alert)
    // lascia il timer armato e un R tenuto premuto altrove potrebbe ancora
    // far scattare il riavvio.
    [self cancelRHold];
    return [super resignFirstResponder];
}

- (void)keyDown:(NSEvent*)event {
    using mz::app::experience::Key;

    // Durante la scelta dello schermo, frecce/INVIO/cifre hanno un altro
    // significato: si intercettano qui, prima di experience::handleKey.
    // Mirror di "if (g.choosing && handlePickerKey(wp)) return 0;" in
    // main.cpp. Il resto (ESC compreso) passa oltre invariato.
    if ([self.controller handlePickerKeyCode:event.keyCode
                                        chars:event.charactersIgnoringModifiers]) {
        return;
    }

    switch (event.keyCode) {
        case 53:  mz::app::experience::handleKey(Key::Escape); return;   // esc
        case 36:  mz::app::experience::handleKey(Key::Enter);  return;   // return
        case 123: mz::app::experience::handleKey(Key::Left);   return;
        case 124: mz::app::experience::handleKey(Key::Right);  return;
        case 125: mz::app::experience::handleKey(Key::Down);   return;
        case 126: mz::app::experience::handleKey(Key::Up);     return;
        default: break;
    }

    NSString* chars = [event.charactersIgnoringModifiers lowercaseString];
    if (chars.length == 0) return;
    const bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    const unichar ch = [chars characterAtIndex:0];

    if (ch == 'r') {
        // R e' l'unico tasto con due azioni distinte (tap contro tenuto
        // premuto): l'azione vera parte da keyUp: o dal timer, non da qui -
        // altrimenti un tap normale scatterebbe subito anche l'azione "hold".
        // isARepeat: ignora gli autorepeat di macOS, il timer e' il nostro.
        if (!event.isARepeat) [self startRHold];
        return;
    }

    switch (ch) {
        case 's': mz::app::experience::handleKey(shift ? Key::ShiftS : Key::S); return;
        case 'l': mz::app::experience::handleKey(Key::L); return;
        case 'h': mz::app::experience::handleKey(Key::H); return;
        case 'q': mz::app::experience::handleKey(Key::Q); return;
        case 'd': mz::app::experience::handleKey(Key::D); return;
        case 'b': mz::app::experience::handleKey(Key::B); return;
        case 'c': mz::app::experience::handleKey(Key::C); return;
        case 'x': mz::app::experience::handleKey(Key::X); return;
        case 'v': mz::app::experience::handleKey(Key::V); return;
        case 'm': mz::app::experience::handleKey(Key::M); return;
        case 'k': mz::app::experience::handleKey(Key::K); return;
        case 'e': mz::app::experience::handleKey(shift ? Key::ShiftE : Key::E); return;
        case 'p':
            // Non passa da experience::handleKey: la scelta schermo e'
            // gestione di finestre (vedi il commento in cima al file), non
            // fa parte di Key. Su Windows e' D; qui D e' gia' preso (dati
            // sintetici), quindi P - "proiezione".
            [self.controller reopenPicker];
            return;
        default: return;
    }
}

- (void)keyUp:(NSEvent*)event {
    NSString* chars = [event.charactersIgnoringModifiers lowercaseString];
    if (chars.length == 0 || [chars characterAtIndex:0] != 'r') return;

    const BOOL alreadyFired = _rHoldFired;
    [self cancelRHold];
    // Se il timer non e' scattato prima del rilascio, era un tap normale:
    // l'azione "hold" e' gia' partita da rHoldFired, non va duplicata.
    if (!alreadyFired) mz::app::experience::handleKey(mz::app::experience::Key::R);
}

@end

// ---------------------------------------------------------------------------
// Delegate: possiede finestre, grafica e il timer che guida il render.
// (Interfaccia dichiarata piu' sopra, prima di MZView - vedi il commento li'.)
// ---------------------------------------------------------------------------

namespace {

/**
 * NSScreen che corrisponde a un app::Display, tramite CGDirectDisplayID
 * (salvato in Display::handle da enumerateDisplaysNative). Non si assume che
 * [NSScreen screens] e _displays abbiano lo stesso ordine - enumerateDisplays()
 * riordina col primario per primo, [NSScreen screens] non da' questa garanzia
 * per iscritto - quindi si cerca per identita', non per indice.
 */
NSScreen* screenForDisplay(const mz::app::Display& d) {
    const auto wanted = static_cast<CGDirectDisplayID>(reinterpret_cast<std::uintptr_t>(d.handle));
    for (NSScreen* s in [NSScreen screens]) {
        NSNumber* num = s.deviceDescription[@"NSScreenNumber"];
        if (num && num.unsignedIntValue == wanted) return s;
    }
    return nil;
}

} // namespace

@implementation MZAppDelegate

- (instancetype)initWithOptions:(Options)opt {
    self = [super init];
    if (self) {
        _opt = opt;
        _candidate = -1;
        _projIndex = -1;
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)tick:(NSTimer*)timer {
    (void)timer;

    if (mz::app::experience::wantsQuit()) {
        [_window performClose:nil];
        return;
    }

    const NSSize size = _view.bounds.size;
    if (size.width != _lastSize.width || size.height != _lastSize.height) {
        _lastSize = size;
        _renderer.resize(static_cast<unsigned>(size.width), static_cast<unsigned>(size.height));
    }

    if (_projWindow) {
        const NSSize psize = _projView.bounds.size;
        if (psize.width != _projLastSize.width || psize.height != _projLastSize.height) {
            _projLastSize = psize;
            _projRenderer.resize(static_cast<unsigned>(psize.width),
                                 static_cast<unsigned>(psize.height));
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - _lastTick).count();
    _lastTick = now;

    mz::app::experience::ProjectionState proj;
    if (_projWindow) {
        proj.renderer  = &_projRenderer;
        proj.choosing  = _choosing;
        proj.candidate = _candidate;
        proj.displays  = &_displays;
    }
    mz::app::experience::frame(_renderer, dt, proj);
}

/** Sposta la finestra di proiezione sullo schermo indicato, a tutto schermo. */
- (void)placeProjection:(int)index {
    if (!_projWindow || index < 0 || index >= static_cast<int>(_displays.size())) return;

    NSScreen* screen = screenForDisplay(_displays[static_cast<std::size_t>(index)]);
    if (!screen) return;   // schermo sparito fra l'enumerazione e ora: niente crash, si aspetta il prossimo cambio

    [_projWindow setFrame:screen.frame display:YES];
    [_projWindow orderFrontRegardless];
    _candidate = index;
}

/** Conferma lo schermo di proiezione e ricorda la scelta per la prossima volta. */
- (void)confirmProjection:(int)index {
    if (index < 0 || index >= static_cast<int>(_displays.size())) return;

    _projIndex = index;
    _choosing  = NO;
    [self placeProjection:index];
    mz::app::saveDisplayChoice(_displays[static_cast<std::size_t>(index)].deviceName,
                                mz::app::layoutSignature(_displays));

    // La finestra dell'operatore deve tornare a ricevere i tasti: durante la
    // scelta poteva averla persa se l'utente aveva cliccato sulla proiezione.
    [_window makeKeyAndOrderFront:nil];
    [_view.window makeFirstResponder:_view];
}

- (void)reopenPicker {
    if (_displays.size() < 2 || !_projWindow) return;
    _choosing = YES;
    [self placeProjection:(_projIndex >= 0 ? _projIndex : 1)];
}

- (BOOL)handlePickerKeyCode:(unsigned short)keyCode chars:(NSString*)chars {
    if (!_choosing) return NO;
    const int n = static_cast<int>(_displays.size());
    if (n <= 0) return NO;

    const int cur = (_candidate < 0) ? 0 : _candidate;

    switch (keyCode) {
        case 123: case 126:   // sinistra, su
            [self placeProjection:(cur - 1 + n) % n];
            return YES;
        case 124: case 125:   // destra, giu'
            [self placeProjection:(cur + 1) % n];
            return YES;
        case 36:              // invio
            [self confirmProjection:cur];
            return YES;
        default:
            break;
    }

    // Anche i tasti numerici: con piu' di due schermi e' piu' rapido.
    if (chars.length > 0) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= '1' && c < static_cast<unichar>('1' + n)) {
            [self confirmProjection:(c - '1')];
            return YES;
        }
    }
    return NO;
}

- (void)screenParametersChanged:(NSNotification*)note {
    (void)note;
    // Uno schermo scollegato o aggiunto: la finestra di proiezione potrebbe
    // essere finita fuori dal desktop visibile. Si riparte dalla scelta -
    // mirror esatto di WM_DISPLAYCHANGE in main.cpp, stessa condizione:
    // se e' sceso a un solo schermo non si fa nulla (limite gia' presente li').
    _displays = mz::app::enumerateDisplays();
    if (_projWindow && _displays.size() >= 2) {
        const int match = mz::app::matchStoredChoice(_displays);
        if (match >= 0) {
            [self confirmProjection:match];
        } else {
            _choosing = YES;
            [self placeProjection:(_displays.size() > 1 ? 1 : 0)];
        }
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;

    const NSRect frame = NSMakeRect(0, 0, 1280, 800);
    const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    _window.title = @"Mind Zoom";
    _window.delegate = self;
    [_window center];

    _view = [[MZView alloc] initWithFrame:frame];
    _view.controller = self;
    _window.contentView = _view;

    // --- schermi ---
    _displays = mz::app::enumerateDisplays();
    const bool wantDual = !_opt.singleScreen && _displays.size() >= 2;

    if (_opt.projWindowed) {
        _projWindow = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(640, 0, 800, 500)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _projWindow.title = @"Mind Zoom · proiezione (simulata)";
        _projView = [[MZView alloc] initWithFrame:NSMakeRect(0, 0, 800, 500)];
        // A differenza della proiezione vera (borderless, mai key) questa
        // finestra puo' prendere il focus: i tasti devono funzionare lo stesso.
        _projView.controller = self;
        _projWindow.contentView = _projView;
        [_projWindow orderFront:nil];
    } else if (wantDual) {
        // Senza bordo, senza titolo: canBecomeKeyWindow torna NO di default per
        // una finestra borderless (vedi NSWindow), quindi non ruba mai il focus
        // da sola - i tasti restano tutti alla finestra dell'operatore.
        _projWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 480)
                                                    styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        _projWindow.level = NSScreenSaverWindowLevel;   // "sopra tutto", equivalente a WS_EX_TOPMOST
        _projWindow.hasShadow = NO;
        _projWindow.ignoresMouseEvents = YES;
        _projWindow.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                         NSWindowCollectionBehaviorStationary;
        _projView = [[MZView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
        _projWindow.contentView = _projView;
    }

    const std::wstring exeDir = mz::app::platform::exeDirectory();
    const std::wstring assetsDir = exeDir + L"/assets";
    const std::wstring recordingsDir = exeDir + L"/registrazioni";
    const std::wstring debugDir = exeDir + L"/debug";
    mz::app::platform::ensureDirectory(recordingsDir);
    mz::app::platform::ensureDirectory(debugDir);

    if (!_graphics.init() || !_graphics.loadSprites(assetsDir, mz::config::kScaleLabels.data(),
                                                     mz::config::kTotalImages)) {
        NSMutableString* nomi = [NSMutableString string];
        for (int i = 0; i < mz::config::kTotalImages; ++i) {
            if (i > 0) [nomi appendString:@", "];
            [nomi appendFormat:@"%d", mz::config::kScaleLabels[i]];
        }
        fatal(mz::app::diag::Code::ImagesMissing,
              [NSString stringWithFormat:
                  @"Cercate in:\n%s\n\nServono %d immagini in .webp, .jpg o .png, "
                  @"chiamate con l'ingrandimento: %@.",
                  std::string(assetsDir.begin(), assetsDir.end()).c_str(),
                  mz::config::kTotalImages, nomi],
              debugDir);
        [NSApp terminate:nil];
        return;
    }

    // Best-effort: se il font non si trova o non si registra, drawText ripiega
    // da solo sul font di sistema (vedi render/renderer_macos.mm). Non e' un
    // motivo per rifiutarsi di partire.
    _graphics.loadFonts(exeDir + L"/fonts");

    [_window makeKeyAndOrderFront:nil];
    [_view.window makeFirstResponder:_view];

    if (!_renderer.init(_graphics, (__bridge void*)_view)) {
        fatal(mz::app::diag::Code::RenderInit,
              @"Inizializzazione del rendering (Core Graphics) fallita.", debugDir);
        [NSApp terminate:nil];
        return;
    }

    if (_projWindow && !_projRenderer.init(_graphics, (__bridge void*)_projView)) {
        // La proiezione e' un di piu': se non si inizializza si continua a
        // schermo singolo invece di negare l'esperienza.
        [_projWindow orderOut:nil];
        _projWindow = nil;
        _projView   = nil;
    }

    mz::app::experience::StartOptions startOpt;
    startOpt.assetsDir     = assetsDir;
    startOpt.recordingsDir = recordingsDir;
    startOpt.record        = _opt.record;
    startOpt.replayPath    = _opt.replayPath;
    startOpt.debugDir      = debugDir;
    startOpt.adaptiveBand  = _opt.adaptiveBand;
    startOpt.hudLevel      = _opt.hudLevel;

    const std::string err = mz::app::experience::start(startOpt);
    if (!err.empty()) {
        fatal(mz::app::diag::Code::StartFailed,
              [NSString stringWithFormat:@"Dettaglio: %s", err.c_str()], debugDir);
        [NSApp terminate:nil];
        return;
    }

    // --- scelta dello schermo di proiezione ---
    if (_projWindow && !_opt.projWindowed) {
        const int stored = mz::app::matchStoredChoice(_displays);
        if (stored >= 0) {
            [self confirmProjection:stored];
        } else {
            // Candidato di partenza: il primo schermo NON primario, quasi
            // sempre quello giusto. Resta comunque da confermare guardando.
            _choosing = YES;
            [self placeProjection:1];
        }
        [[NSNotificationCenter defaultCenter]
            addObserver:self
               selector:@selector(screenParametersChanged:)
                   name:NSApplicationDidChangeScreenParametersNotification
                 object:nil];
    }

    _lastSize = NSZeroSize;
    _projLastSize = NSZeroSize;
    _lastTick = std::chrono::steady_clock::now();
    [self startDisplayLinkForScreen:_window.screen];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(windowDidChangeScreen:)
                                                 name:NSWindowDidChangeScreenNotification
                                               object:_window];

    [NSApp activateIgnoringOtherApps:YES];
}

/**
 * Il ciclo di disegno era un NSTimer a 1/60s: non e' agganciato al refresh
 * reale dello schermo, e' soggetto al coalescing/tolleranza che macOS applica
 * ai timer, e slitta con qualunque altra attivita' sul run loop principale -
 * a schermo si vede come cadenza irregolare dei fotogrammi, anche quando il
 * disegno di ogni singolo fotogramma e' velocissimo. CVDisplayLink e'
 * l'equivalente del vsync di DXGI sul ramo Windows (vedi il commento in cima
 * al file e nel README): il sistema chiama il blocco esattamente al ritmo del
 * refresh del monitor scelto. Il blocco gira su un thread ad alta priorita'
 * dedicato di CoreVideo, non sul thread principale: si passa subito la palla
 * al thread principale con dispatch_async, che e' quello che tocca
 * NSView/CALayer. weakSelf spezza il ciclo di retain fra self e il blocco che
 * self stesso possiede tramite _displayLink.
 */
- (void)startDisplayLinkForScreen:(NSScreen*)screen {
    if (_displayLink) {
        CVDisplayLinkStop(_displayLink);
        CVDisplayLinkRelease(_displayLink);
        _displayLink = nullptr;
    }

    CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink);

    NSNumber* screenNumber = screen.deviceDescription[@"NSScreenNumber"];
    if (screenNumber) {
        CVDisplayLinkSetCurrentCGDisplay(_displayLink, screenNumber.unsignedIntValue);
    }

    __weak MZAppDelegate* weakSelf = self;
    CVDisplayLinkSetOutputHandler(_displayLink, ^CVReturn(
        CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf tick:nil];
        });
        return kCVReturnSuccess;
    });

    CVDisplayLinkStart(_displayLink);
}

/** La finestra e' passata a un altro schermo (l'utente l'ha trascinata): il
 *  link deve seguirla, altrimenti resta agganciato al refresh del monitor di
 *  partenza - su due schermi con refresh diverso si tornerebbe a battere. */
- (void)windowDidChangeScreen:(NSNotification*)note {
    (void)note;
    if (_displayLink) {
        NSNumber* screenNumber = _window.screen.deviceDescription[@"NSScreenNumber"];
        if (screenNumber) {
            CVDisplayLinkSetCurrentCGDisplay(_displayLink, screenNumber.unsignedIntValue);
        }
    }
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (_displayLink) {
        CVDisplayLinkStop(_displayLink);
        CVDisplayLinkRelease(_displayLink);
        _displayLink = nullptr;
    }
    if (_projWindow) {
        [_projWindow orderOut:nil];
        _projWindow = nil;
    }
    mz::app::experience::stop();
    [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    (void)app;
    return YES;
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        const Options opt = parseOptions(argc, argv);
        if (opt.listScreens) return reportScreens();

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        MZAppDelegate* delegate = [[MZAppDelegate alloc] initWithOptions:opt];
        NSApp.delegate = delegate;

        [NSApp run];
    }
    return 0;
}
