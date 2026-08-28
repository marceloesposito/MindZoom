// Shell macOS: finestra Cocoa, ciclo di render, tasti. La logica (DSP,
// calibrazione, disegno) vive in app/experience.cpp/.hpp, portabile.
//
// Prima versione: una sola finestra (operatore e partecipante coincidono).
// La proiezione a due schermi resta solo nel ramo Windows (main.cpp) - vedi
// il commento in app/experience.hpp.

#import <Cocoa/Cocoa.h>

#include "app/experience.hpp"
#include "app/platform.hpp"
#include "config.hpp"
#include "render/renderer.hpp"

#include <chrono>
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
        }
    }
    if (!o.replayPath.empty()) o.record = false;
    return o;
}

void fatal(NSString* text) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Mind Zoom";
    alert.informativeText = text;
    alert.alertStyle = NSAlertStyleCritical;
    [alert runModal];
}

} // namespace

// ---------------------------------------------------------------------------
// Vista: riceve i tasti, la finestra scrive direttamente sul suo layer
// (Renderer::end() lo fa gia', vedi render/renderer_macos.mm).
// ---------------------------------------------------------------------------

@interface MZView : NSView
@end

@implementation MZView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }

- (void)keyDown:(NSEvent*)event {
    using mz::app::experience::Key;

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

    switch ([chars characterAtIndex:0]) {
        case 's': mz::app::experience::handleKey(shift ? Key::ShiftS : Key::S); return;
        case 'l': mz::app::experience::handleKey(Key::L); return;
        case 'r': mz::app::experience::handleKey(Key::R); return;
        case 'h': mz::app::experience::handleKey(Key::H); return;
        case 'q': mz::app::experience::handleKey(Key::Q); return;
        case 'd': mz::app::experience::handleKey(Key::D); return;
        case 'b': mz::app::experience::handleKey(Key::B); return;
        case 'c': mz::app::experience::handleKey(Key::C); return;
        case 'x': mz::app::experience::handleKey(Key::X); return;
        case 'v': mz::app::experience::handleKey(Key::V); return;
        case 'm': mz::app::experience::handleKey(Key::M); return;
        case 'k': mz::app::experience::handleKey(Key::K); return;
        default: return;
    }
}

@end

// ---------------------------------------------------------------------------
// Delegate: possiede finestra, grafica e il timer che guida il render.
// ---------------------------------------------------------------------------

@interface MZAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow*             _window;
    MZView*                _view;
    NSTimer*               _timer;
    mz::render::GraphicsCore _graphics;
    mz::render::Renderer     _renderer;
    NSSize                   _lastSize;
    std::chrono::steady_clock::time_point _lastTick;
    Options                  _opt;
}
@end

@implementation MZAppDelegate

- (instancetype)initWithOptions:(Options)opt {
    self = [super init];
    if (self) { _opt = opt; }
    return self;
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

    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - _lastTick).count();
    _lastTick = now;

    mz::app::experience::frame(_renderer, dt);
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
    _window.contentView = _view;

    const std::wstring exeDir = mz::app::platform::exeDirectory();
    const std::wstring assetsDir = exeDir + L"/assets";
    const std::wstring recordingsDir = exeDir + L"/registrazioni";
    const std::wstring debugDir = exeDir + L"/debug";
    mz::app::platform::ensureDirectory(recordingsDir);
    mz::app::platform::ensureDirectory(debugDir);

    if (!_graphics.init() || !_graphics.loadSprites(assetsDir, mz::config::kTotalImages)) {
        fatal([NSString stringWithFormat:
                  @"Immagini non caricate da:\n%s\n\nServono 1..%d in .webp, .jpg o .png.",
                  std::string(assetsDir.begin(), assetsDir.end()).c_str(),
                  mz::config::kTotalImages]);
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
        fatal(@"Inizializzazione del rendering (Core Graphics) fallita.");
        [NSApp terminate:nil];
        return;
    }

    mz::app::experience::StartOptions startOpt;
    startOpt.assetsDir     = assetsDir;
    startOpt.recordingsDir = recordingsDir;
    startOpt.record        = _opt.record;
    startOpt.replayPath    = _opt.replayPath;
    startOpt.debugDir      = debugDir;
    startOpt.adaptiveBand  = _opt.adaptiveBand;

    const std::string err = mz::app::experience::start(startOpt);
    if (!err.empty()) {
        fatal([NSString stringWithFormat:@"Impossibile avviare: %s", err.c_str()]);
        [NSApp terminate:nil];
        return;
    }

    _lastSize = NSZeroSize;
    _lastTick = std::chrono::steady_clock::now();
    _timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                      target:self
                                    selector:@selector(tick:)
                                    userInfo:nil
                                     repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];

    [NSApp activateIgnoringOtherApps:YES];
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [_timer invalidate];
    _timer = nil;
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

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        MZAppDelegate* delegate = [[MZAppDelegate alloc] initWithOptions:opt];
        NSApp.delegate = delegate;

        [NSApp run];
    }
    return 0;
}
