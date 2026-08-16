// Controparte macOS di platform_win32.cpp: enumerare gli schermi e trovare dove
// si scrive la scelta.
//
// ATTENZIONE: non e' mai stato compilato. Scritto su Windows, dove non esiste un
// toolchain Objective-C.

#import <Cocoa/Cocoa.h>

#include "app/platform.hpp"

#include <string>

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
NSString* choiceDir(bool createDir) {
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
    NSString* dir = choiceDir(forWrite);
    if (!dir) return nullptr;

    NSString* path = [dir stringByAppendingPathComponent:@"schermo.txt"];
    return std::fopen(path.fileSystemRepresentation, forWrite ? "wt" : "rt");
}

} // namespace mz::app::platform
