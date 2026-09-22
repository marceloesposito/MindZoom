// Controparte macOS di platform_win32.cpp: enumerare gli schermi e trovare dove
// si scrive la scelta.
//
// enumerateDisplaysNative() e' verificata dal test "Enumerazione degli schermi"
// (tests/test_main.cpp): su questa macchina rileva correttamente 1 schermo. Non
// era mai stata usata dallo shell macOS pero' (nessuna modalita' a due schermi
// finche' non l'ha portata experience.cpp/shell_macos.mm) - la firma resta la
// stessa di platform_win32.cpp apposta.

#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>

#include "app/platform.hpp"

#include <string>
#include <vector>

namespace mz::app::platform {
namespace {

std::wstring toWide(NSString* s) {
    if (!s) return {};
    // NSString e' UTF-16; su macOS wchar_t e' UTF-32. Si passa per UTF-32
    // esplicito invece di copiare unichar in wchar_t, che romperebbe le coppie
    // surrogate - improbabile in un nome di schermo, ma gratis da fare bene.
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (!d) return {};
    const auto* p = static_cast<const wchar_t*>(d.bytes);
    return std::wstring(p, d.length / sizeof(wchar_t));
}

/** ~/Library/Application Support/MindZoom, creata se manca. */
NSString* supportDir(bool createDir) {
    NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (dirs.count == 0) return nil;

    NSString* dir = [dirs[0] stringByAppendingPathComponent:@"MindZoom"];
    if (createDir) {
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil];
    }
    return dir;
}

} // namespace

std::vector<Display> enumerateDisplaysNative() {
    std::vector<Display> out;

    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (screens.count == 0) return out;

    // Cocoa ha l'origine in basso a sinistra e il resto del programma ragiona in
    // coordinate y-giu' come Windows. Si converte qui, una volta: l'altezza dello
    // schermo con la barra dei menu (screens[0]) e' il riferimento.
    const CGFloat totale = screens[0].frame.origin.y + screens[0].frame.size.height;

    NSUInteger i = 0;
    for (NSScreen* s in screens) {
        const NSRect f = s.frame;

        Display d;
        // CGDirectDisplayID e' un uint32: ci sta in un puntatore e chi lo usa lo
        // riconverte. Passare per un intero evita di dover esporre il tipo.
        NSNumber* num = s.deviceDescription[@"NSScreenNumber"];
        d.handle = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(num ? num.unsignedIntValue : 0u));

        const int top = static_cast<int>(totale - (f.origin.y + f.size.height));
        d.bounds = ScreenRect{static_cast<int>(f.origin.x), top,
                              static_cast<int>(f.origin.x + f.size.width),
                              static_cast<int>(top + f.size.height)};

        // Il nome deve restare ASCII e STABILE fra un avvio e l'altro: e' la
        // chiave con cui si ritrova la scelta memorizzata. localizedName sarebbe
        // piu' bello da leggere ma cambia con la lingua di sistema e con il
        // monitor collegato, quindi non va bene come chiave.
        d.deviceName = L"Display " + std::to_wstring(i + 1);
        d.primary    = (i == 0);   // screens[0] e' quello con la barra dei menu
        out.push_back(std::move(d));
        ++i;
    }
    return out;
}

std::FILE* openChoiceFile(bool forWrite) {
    NSString* dir = supportDir(forWrite);
    if (!dir) return nullptr;

    NSString* path = [dir stringByAppendingPathComponent:@"schermo.txt"];
    return std::fopen(path.fileSystemRepresentation, forWrite ? "wt" : "rt");
}

std::wstring dataDirectory() {
    return toWide(supportDir(true));
}

std::wstring exeDirectory() {
    // _NSGetExecutablePath puo' restituire un percorso con "..", link simbolici
    // ecc.: si passa da NSString per normalizzarlo, come fa gia' supportDir().
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // prima chiamata: solo per sapere size
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};

    NSString* full = [[NSString stringWithUTF8String:buf.data()]
                          stringByResolvingSymlinksInPath];
    return toWide([full stringByDeletingLastPathComponent]);
}

void ensureDirectory(const std::wstring& path) {
    // path e' sempre ASCII qui (sottocartelle come "registrazioni" o "assets"),
    // quindi l'allargamento byte-per-byte da wchar_t a char e' sicuro.
    NSString* s = [[NSString alloc] initWithBytes:path.data()
                                            length:path.size() * sizeof(wchar_t)
                                          encoding:NSUTF32LittleEndianStringEncoding];
    [[NSFileManager defaultManager] createDirectoryAtPath:s
                             withIntermediateDirectories:YES
                                              attributes:nil
                                                   error:nil];
}

} // namespace mz::app::platform
