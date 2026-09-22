// Backend macOS del renderer: Core Graphics + Core Text + Image I/O.
//
// Stessa interfaccia del backend Windows (render/renderer.hpp), stessa
// semantica, stesse primitive. Chi disegna non cambia una riga.
//
// Il disegno e' diviso in due, e la divisione e' la ragione per cui l'app sta
// nel fotogramma:
//
//   - la FOTO sta su due CALayer e la compone la GPU (vedi Renderer::Impl).
//     E' un'immagine da 3190x3190 ridimensionata a ogni fotogramma, due volte
//     per via del crossfade: su CPU costava 8-15 fps, su GPU e' gratis.
//   - TUTTO IL RESTO (testo, pannelli, barra della scala) si disegna con Core
//     Graphics in un contesto bitmap fuori schermo, che a end() diventa il
//     contenuto di un layer sopra la foto. E' poca roba, sta larga in CPU, e
//     lascia intatte le primitive gia' scritte: chi disegna non cambia una riga.
//
// Metal non serve: la GPU la si usa gia' tutta tramite Core Animation, senza
// shader, pipeline state ne' command buffer.
//
// Il modello begin/end non e' quello di NSView, che disegna quando gli viene
// chiesto: il ciclo di gioco resta padrone del tempo, come su Windows.
//
// Due trappole di prestazioni sono documentate dove si risolvono, e sono
// entrambe costate misure col profiler: materializeSprite() (Image I/O tiene le
// immagini "pigre" e le ridecodifica a ogni disegno) e createContext() (lo
// spazio colore del contesto deve essere quello dello schermo, o Core Animation
// converte l'intera bitmap a ogni fotogramma).

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>

#include "render/renderer.hpp"

#include <algorithm>
#include <codecvt>
#include <locale>
#include <cstdint>
#include <vector>

namespace mz::render {
namespace {

/** std::wstring (UTF-32 su macOS) -> CFString. */
CFStringRef toCFString(const std::wstring& s) {
    static_assert(sizeof(wchar_t) == 4, "su macOS wchar_t e' UTF-32");
    return CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()),
        static_cast<CFIndex>(s.size() * sizeof(wchar_t)),
        kCFStringEncodingUTF32LE, false);
}

CGRect toCG(Rect r) {
    return CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

/**
 * Iowan Old Style: serif old-style, caldo e solido, scelto dopo un provino a
 * confronto (dodici candidati renderizzati con questo stesso codice). Con
 * l'identita' Biodetails resta dov'era - cifre, etichette dei pulsanti, righe
 * di servizio - perche' cambia solo il lettering delle INTESTAZIONI, che ha
 * una funzione sua (createDisplayFont).
 *
 * E' una famiglia STATICA, con facce separate per peso: non ha l'asse 'wght',
 * quindi la faccia si sceglie per nome. La soglia a 550 sta in mezzo fra i due
 * soli pesi che l'applicazione chiede (400 per il corpo, 640 per i titoli).
 *
 * E' un font di SISTEMA su macOS. CTFontCreateWithName non fallisce mai se il
 * nome non risolve (ripiega sul font di sistema), quindi anche su un Mac che
 * non lo avesse il testo esce comunque.
 */
CTFontRef createTextFont(CGFloat size, CGFloat weight) {
    return CTFontCreateWithName(weight >= 550.0 ? CFSTR("IowanOldStyle-Bold")
                                                : CFSTR("IowanOldStyle-Roman"),
                                size, nullptr);
}

/**
 * CTFontCreateWithName fa un lookup/match nel catalogo dei font di sistema:
 * non e' gratis, e va rifatto la stessa identica ricerca ogni volta che si
 * disegna testo con la stessa taglia. Le combinazioni (size, weight) distinte
 * usate nell'app sono poche (una manciata), quindi si tiene una cache
 * per-processo che le crea una sola volta: mai svuotata, ma il numero di
 * voci e' limitato a priori dal set di taglie che il codice di disegno usa.
 */
CTFontRef cachedTextFont(CGFloat size, CGFloat weight) {
    struct Entry { CGFloat size; CGFloat weight; CTFontRef font; };
    static std::vector<Entry> cache;
    for (const auto& e : cache) {
        if (e.size == size && e.weight == weight) return e.font;
    }
    CTFontRef font = createTextFont(size, weight);
    cache.push_back({size, weight, font});
    return font;
}

/**
 * Manrope: il sans geometrico del corpo del testo. Resta il font di tutto cio'
 * che si legge - paragrafi, elenchi, etichette - anche con l'identita'
 * Biodetails, che tocca solo il lettering delle intestazioni.
 *
 * NON e' di sistema: viaggia nel bundle (assets/fonts, registrato da
 * GraphicsCore::loadFonts). E' un font a variazione con il solo asse 'wght'
 * (200..800) e l'istanza di default e' la Light, quindi il peso va chiesto
 * esplicitamente via kCTFontVariationAttribute: 400 per il corpo, 600 per il
 * "grassetto". Se il file manca, CoreText ripiega sul font di sistema.
 */
CTFontRef createBodyFont(CGFloat size, bool bold) {
    const std::int64_t wghtTag    = 0x77676874;   // 'wght'
    const double       wghtValue  = bold ? 600.0 : 400.0;
    CFNumberRef tag = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &wghtTag);
    CFNumberRef val = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &wghtValue);
    const void* axisKeys[] = {tag};
    const void* axisVals[] = {val};
    CFDictionaryRef variation = CFDictionaryCreate(kCFAllocatorDefault, axisKeys, axisVals, 1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    CFRelease(tag);
    CFRelease(val);

    const void* keys[] = {kCTFontNameAttribute, kCTFontVariationAttribute};
    const void* vals[] = {CFSTR("ManropeVariable-Light"), variation};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(variation);

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);
    CTFontRef font = CTFontCreateWithFontDescriptor(desc, size, nullptr);
    CFRelease(desc);
    return font;
}

/**
 * Il lettering dei titoli: League Gothic, il sostituto misurato del lettering
 * "BIODETAILS" del poster (scarto medio 0.064 sulle larghezze di otto
 * lettere; la seconda scelta stava a 0.245). E' un font a variazione con il
 * solo asse 'wdth' (75..100): la larghezza 75, la piu' stretta, e' quella
 * che replica le proporzioni del poster (O larga 0.27 dell'altezza delle
 * maiuscole contro 0.28 misurato).
 *
 * NON e' di sistema: viaggia nel bundle (assets/fonts, registrato da
 * GraphicsCore::loadFonts). Se il file manca, CoreText ripiega sul font di
 * sistema - si vede subito, perche' il titolo esce largo il doppio.
 */
CTFontRef createDisplayFont(CGFloat size) {
    const std::int64_t wdthTag   = 0x77647468;   // 'wdth'
    const double       wdthValue = 75.0;
    CFNumberRef tag = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &wdthTag);
    CFNumberRef val = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &wdthValue);
    const void* axisKeys[] = {tag};
    const void* axisVals[] = {val};
    CFDictionaryRef variation = CFDictionaryCreate(kCFAllocatorDefault, axisKeys, axisVals, 1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    CFRelease(tag);
    CFRelease(val);

    const void* keys[] = {kCTFontNameAttribute, kCTFontVariationAttribute};
    const void* vals[] = {CFSTR("LeagueGothic-Regular"), variation};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(variation);

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);
    CTFontRef font = CTFontCreateWithFontDescriptor(desc, size, nullptr);
    CFRelease(desc);
    return font;
}

/** Stessa cache di cachedTextFont, per il lettering. */
CTFontRef cachedDisplayFont(CGFloat size) {
    struct Entry { CGFloat size; CTFontRef font; };
    static std::vector<Entry> cache;
    for (const auto& e : cache) {
        if (e.size == size) return e.font;
    }
    CTFontRef font = createDisplayFont(size);
    cache.push_back({size, font});
    return font;
}

/** Stessa cache di cachedTextFont, per il font di corpo. */
CTFontRef cachedBodyFont(CGFloat size, bool bold) {
    struct Entry { CGFloat size; bool bold; CTFontRef font; };
    static std::vector<Entry> cache;
    for (const auto& e : cache) {
        if (e.size == size && e.bold == bold) return e.font;
    }
    CTFontRef font = createBodyFont(size, bold);
    cache.push_back({size, bold, font});
    return font;
}

/**
 * I colori dell'applicazione sono scritti in sRGB, ma il contesto ora vive
 * nello spazio dello schermo (vedi createContext): CGContextSetRGB* li
 * interpreterebbe come componenti dello spazio del contesto, e su un pannello
 * piu' ampio dell'sRGB uscirebbero piu' saturi. Si passa quindi un CGColor
 * esplicitamente sRGB e la conversione la fa Core Graphics, una volta per
 * chiamata di disegno invece che su tutti i pixel del fotogramma.
 */
CGColorSpaceRef srgbSpace() {
    static CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return cs;
}

void setColor(CGContextRef ctx, Color c, bool fill) {
    const CGFloat comp[4] = {c.r, c.g, c.b, c.a};
    CGColorRef col = CGColorCreate(srgbSpace(), comp);
    if (!col) return;
    if (fill) CGContextSetFillColorWithColor(ctx, col);
    else      CGContextSetStrokeColorWithColor(ctx, col);
    CGColorRelease(col);
}

/** Rettangolo con angoli tondi, come FillRoundedRectangle di D2D. */
void addRoundedRect(CGContextRef ctx, CGRect r, CGFloat radius) {
    if (radius <= 0.0) {
        CGContextAddRect(ctx, r);
        return;
    }
    const CGFloat rr = std::min<CGFloat>(radius, std::min(r.size.width, r.size.height) * 0.5);
    CGContextBeginPath(ctx);
    CGContextMoveToPoint(ctx, CGRectGetMinX(r) + rr, CGRectGetMinY(r));
    CGContextAddArcToPoint(ctx, CGRectGetMaxX(r), CGRectGetMinY(r),
                           CGRectGetMaxX(r), CGRectGetMaxY(r), rr);
    CGContextAddArcToPoint(ctx, CGRectGetMaxX(r), CGRectGetMaxY(r),
                           CGRectGetMinX(r), CGRectGetMaxY(r), rr);
    CGContextAddArcToPoint(ctx, CGRectGetMinX(r), CGRectGetMaxY(r),
                           CGRectGetMinX(r), CGRectGetMinY(r), rr);
    CGContextAddArcToPoint(ctx, CGRectGetMinX(r), CGRectGetMinY(r),
                           CGRectGetMaxX(r), CGRectGetMinY(r), rr);
    CGContextClosePath(ctx);
}

} // namespace

// ---------------------------------------------------------------------------
// GraphicsCore
// ---------------------------------------------------------------------------

struct GraphicsCore::Impl {
    std::vector<CGImageRef> sources;

    ~Impl() { release(); }

    void release() {
        for (CGImageRef img : sources) {
            if (img) CGImageRelease(img);
        }
        sources.clear();
    }
};

GraphicsCore::GraphicsCore() : impl_(std::make_unique<Impl>()) {}
GraphicsCore::~GraphicsCore() = default;

bool GraphicsCore::init() { return true; }   // niente factory da creare

void GraphicsCore::shutdown() { impl_->release(); }

std::size_t GraphicsCore::spriteCount() const noexcept { return impl_->sources.size(); }

// Ridisegna l'immagine appena letta da Image I/O dentro un bitmap nostro, e
// restituisce una CGImage che punta a quel bitmap.
//
// Serve perche' la CGImage che esce da CGImageSourceCreateImageAtIndex resta
// legata al file compresso: non possiede i pixel, possiede un "provider" che
// chiama Image I/O per farsi decodificare al volo la porzione che serve, e i
// pixel decodificati finiscono in una cache di sistema a capienza limitata. Le
// nostre otto foto fanno 3190x3190 l'una, cioe' ~40 MB di RGBA a testa e ~326
// MB in tutto: la cache di Image I/O non ci sta nemmeno vicino, quindi butta
// via il decodificato subito e lo rifa' al disegno dopo. Con lo zoom continuo
// il disegno dopo e' il frame dopo, sempre. Misurato con `sample` sul processo
// vivo: sotto CGContextDrawImage c'era img_data_lock ->
// WebPReadPlugin::decodeImageImp -> VP8DecodeMB, cioe' il decode VP8 completo
// dentro la chiamata di disegno, a ogni fotogramma e per due sprite (il
// crossfade), raddoppiati quando c'e' anche la finestra di proiezione.
// kCGImageSourceShouldCacheImmediately non basta: forza il primo decode, ma non
// impedisce l'eviction, e infatti nel secondo `sample` VP8DecodeMB era ancora
// in cima.
//
// Un bitmap creato da noi invece non e' evictabile da nessuno: i pixel sono
// nostri e restano finche' non li rilasciamo. Il formato e' scelto identico a
// quello del contesto di rendering (32 bit little-endian, sRGB), cosi' il draw
// e' una copia scalata e basta - senza il rimescolamento di canali che si
// vedeva come vConvert_PermuteChannels_ARGB8888 in vImage.
//
// "Identico" va preso alla lettera, alfa compreso. Le foto sono opache e il
// primo tentativo usava NoneSkipFirst (xRGB), che in memoria ha lo stesso
// layout di PremultipliedFirst e sembrava gratis in piu': niente alfa da
// moltiplicare. Non lo era. Il contesto di destinazione e' PremultipliedFirst,
// quindi Core Graphics si ricostruiva il canale alfa a ogni disegno - nel
// `sample` si vedeva come ripc_AcquireRIPImageData -> CGSImageDataLock ->
// vImageOverwriteChannelsWithScalar_ARGB8888, cioe' una passata su tutti e 10
// i megapixel per riempire di 255 un canale, per sprite e per fotogramma: da
// sola il 41% del costo del disegno. Meglio pagare l'alfa una volta qui.
//
// Si tiene la risoluzione piena apposta: lo zoom arriva a 1.5x sulla scala
// "cover" (control/zoom.cpp), che su uno schermo da 2560 px larghi vorrebbe
// 3840 px di sorgente - piu' dei 3190 che abbiamo. Ridurre le foto si vedrebbe
// proprio nel momento di massimo ingrandimento, che e' il punto dell'app.
static CGImageRef materializeSprite(CGImageRef lazy) {
    const std::size_t w = CGImageGetWidth(lazy);
    const std::size_t h = CGImageGetHeight(lazy);
    if (w == 0 || h == 0) return nullptr;

    // Si resta nello spazio colore della foto stessa: cosi' questo passaggio
    // non trasforma nemmeno un pixel, e' solo una decompressione. Convertire
    // qui in sRGB cambierebbe i valori dell'immagine al microscopio, che deve
    // restare quella che e'.
    CGColorSpaceRef cs = nullptr;
    CGColorSpaceRef own = CGImageGetColorSpace(lazy);
    if (own && CGColorSpaceGetModel(own) == kCGColorSpaceModelRGB) {
        cs = CGColorSpaceRetain(own);
    }
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    CGContextRef bmp = CGBitmapContextCreate(nullptr, w, h, 8, 0, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big);
    // Le foto sono opache ma il bitmap nasce trasparente: senza questo, i bordi
    // che il draw non copre resterebbero a alfa 0.
    if (bmp) CGContextSetAlpha(bmp, 1.0);
    CGColorSpaceRelease(cs);
    if (!bmp) return nullptr;

    // Un solo decode, a scala 1:1, senza ricampionamento.
    CGContextSetInterpolationQuality(bmp, kCGInterpolationNone);
    CGContextDrawImage(bmp, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), lazy);

    CGImageRef solid = CGBitmapContextCreateImage(bmp);
    CGContextRelease(bmp);
    return solid;
}

bool GraphicsCore::loadSprites(const std::wstring& dir, const int* magnitudes, int count) {
    auto& d = *impl_;
    d.release();
    d.sources.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        CGImageRef image = nullptr;

        // Stesso ordine di preferenza del backend Windows: JPEG per primo. Su
        // macOS il WebP e' decodificabile da Image I/O solo da Big Sur in avanti,
        // quindi la stessa ragione di prudenza vale anche qui.
        for (const wchar_t* ext : {L".jpg", L".webp", L".png"}) {
            const std::wstring path = dir + L"/" + std::to_wstring(magnitudes[i]) + ext;
            CFStringRef cfPath = toCFString(path);
            if (!cfPath) continue;

            CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfPath,
                                                         kCFURLPOSIXPathStyle, false);
            CFRelease(cfPath);
            if (!url) continue;

            CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
            CFRelease(url);
            if (!src) continue;

            if (CGImageSourceGetCount(src) > 0) {
                CGImageRef lazy = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
                if (lazy) {
                    // Da qui in poi il file compresso non serve piu': i pixel
                    // sono in un bitmap nostro. Vedi materializeSprite() per il
                    // perche' non basti lasciare fare a Image I/O.
                    image = materializeSprite(lazy);
                    CGImageRelease(lazy);
                }
            }
            CFRelease(src);
            if (image) break;
        }
        if (!image) return false;
        d.sources.push_back(image);
    }
    return true;
}

bool GraphicsCore::loadFonts(const std::wstring& dir) {
    // Due font non di sistema viaggiano nel bundle, in
    // <bundle>/Contents/MacOS/fonts (ce li mette il CMakeLists), e qui si
    // registrano per il processo: Manrope per il corpo del testo e League
    // Gothic per il lettering delle intestazioni. L'accento (Iowan Old Style)
    // e' di sistema e non va registrato. Il valore di ritorno non e' fatale:
    // senza registrazione chi chiama disegna comunque, col font di sistema.
    const wchar_t* const files[] = {L"/Manrope-Variable.ttf", L"/LeagueGothic-Variable.ttf"};
    bool ok = true;
    for (const wchar_t* file : files) {
        CFStringRef cfPath = toCFString(dir + file);
        if (!cfPath) { ok = false; continue; }

        CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfPath,
                                                      kCFURLPOSIXPathStyle, false);
        CFRelease(cfPath);
        if (!url) { ok = false; continue; }

        CFErrorRef error = nullptr;
        ok = CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &error) && ok;
        if (error) CFRelease(error);
        CFRelease(url);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

// La foto non passa piu' per il contesto CPU: sta su due CALayer, e a ogni
// fotogramma cambiano solo la geometria e l'opacita' di quei layer. Il
// ridimensionamento lo fa la GPU in fase di composizione, gratis.
//
// Prima invece ogni fotogramma rifaceva su CPU il ricampionamento di una foto
// da 3190x3190 (10,2 megapixel) fino alla dimensione dello schermo, due volte -
// il crossfade ne disegna sempre due - con kCGInterpolationHigh. Nel `sample`
// erano resample_horizontal/resample_vertical + argb32_image_mark_argb32, e
// misurato col contatore in shell_macos.mm faceva 8-20 fps. Nessuna scelta di
// formato pixel poteva salvarlo: il costo era proprio il filtraggio di 10
// megapixel per sprite per fotogramma.
//
// Le foto vecchie (1024x768, vedi git) erano 13 volte piu' piccole ed e' per
// questo che con quelle non si notava niente.
//
// L'albero dei layer, dal basso:
//     view.layer                 sfondo (colore di begin())
//       +- clipLayer[0] -> picLayer[0]     sprite attivo
//       +- clipLayer[1] -> picLayer[1]     sprite successivo (crossfade)
//       +- uiLayer                          testo/pannelli, dal contesto CPU
// Il contesto CPU resta esattamente com'era per tutto il resto del disegno:
// e' poca roba e va benissimo su CPU. Cambia solo che ora e' trasparente e fa
// da velo sopra la foto, invece di essere l'intero fotogramma.
struct Renderer::Impl {
    GraphicsCore* core = nullptr;
    NSView*       view = nil;

    CGContextRef  ctx    = nullptr;   // bitmap fuori schermo, solo interfaccia
    unsigned      width  = 0;
    unsigned      height = 0;
    CGFloat       scale  = 1.0;       // fattore Retina

    // Su macOS le immagini decodificate sono gia' pronte all'uso: non esiste il
    // passaggio "carica sul device" che Direct2D richiede. Le texture per
    // finestra sono quindi le stesse CGImage del core, e spriteCount() riporta
    // quante ne ha il core - non zero, che farebbe scattare la guardia del
    // selftest per un problema che qui non esiste.
    std::size_t sprites = 0;

    CALayer* uiLayer      = nil;
    CALayer* clipLayer[2] = {nil, nil};
    CALayer* picLayer[2]  = {nil, nil};
    int      usedSprites  = 0;   // quanti sprite ha chiesto questo fotogramma

    void destroyContext() {
        if (ctx) { CGContextRelease(ctx); ctx = nullptr; }
    }

    bool createContext(unsigned w, unsigned h);
    void buildLayers();
    void layoutLayers();
};

// I layer si costruiscono una volta sola: dopo, a ogni fotogramma, si toccano
// solo frame/opacity/contents.
void Renderer::Impl::buildLayers() {
    CALayer* root = view.layer;
    if (!root || uiLayer) return;

    // Si tiene la convenzione nativa di Core Animation (origine in basso a
    // sinistra) e si converte a mano in un punto solo, invece di affidarsi a
    // geometryFlipped: la vista e' flipped e le due cose si sommerebbero.
    root.geometryFlipped = NO;
    root.masksToBounds   = YES;

    for (int i = 0; i < 2; ++i) {
        picLayer[i] = [CALayer layer];
        picLayer[i].anchorPoint  = CGPointZero;
        picLayer[i].contentsGravity = kCAGravityResize;
        // Trilinear: la foto e' quasi sempre rimpicciolita (3190 px su uno
        // schermo da 2560), ed e' il caso in cui il mipmap si vede.
        picLayer[i].minificationFilter  = kCAFilterTrilinear;
        picLayer[i].magnificationFilter = kCAFilterLinear;

        clipLayer[i] = [CALayer layer];
        clipLayer[i].anchorPoint  = CGPointZero;
        clipLayer[i].masksToBounds = YES;
        clipLayer[i].hidden        = YES;
        [clipLayer[i] addSublayer:picLayer[i]];
        [root addSublayer:clipLayer[i]];
    }

    uiLayer = [CALayer layer];
    uiLayer.anchorPoint = CGPointZero;
    [root addSublayer:uiLayer];

    layoutLayers();
}

void Renderer::Impl::layoutLayers() {
    if (!uiLayer) return;
    const CGRect b = CGRectMake(0, 0, width, height);
    uiLayer.frame = b;
    uiLayer.contentsScale = scale;
    for (int i = 0; i < 2; ++i) {
        picLayer[i].contentsScale  = scale;
        clipLayer[i].contentsScale = scale;
    }
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { impl_->destroyContext(); }

bool Renderer::Impl::createContext(unsigned w, unsigned h) {
    destroyContext();
    width  = std::max(w, 1u);
    height = std::max(h, 1u);

    const std::size_t pw = static_cast<std::size_t>(width * scale);
    const std::size_t ph = static_cast<std::size_t>(height * scale);

    // Nello spazio colore dello schermo, non in sRGB. Il layer di Core
    // Animation vuole i pixel nel profilo del pannello (qui "Color LCD"): se
    // glieli si danno in sRGB, a ogni fotogramma converte lui l'intera bitmap
    // sulla CPU. Nel `sample` era il 50% del tempo del thread principale -
    // CA::Render::copy_image -> CGColorTransformConvertUsingCMSConverter ->
    // vImageConvert_AnyToAny. Convertire qui una volta i pochi colori delle
    // primitive (vedi setColor) invece che tutti i pixel a ogni fotogramma.
    CGColorSpaceRef cs = nullptr;
    NSScreen* screen = view.window.screen ?: [NSScreen mainScreen];
    if (screen) {
        CGColorSpaceRef sc = screen.colorSpace.CGColorSpace;
        // Solo se e' davvero RGB: su un profilo esotico si ricadrebbe in un
        // contesto che CGBitmapContextCreate rifiuta.
        if (sc && CGColorSpaceGetModel(sc) == kCGColorSpaceModelRGB) {
            cs = CGColorSpaceRetain(sc);
        }
    }
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    ctx = CGBitmapContextCreate(nullptr, pw, ph, 8, 0, cs,
                                kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;

    // Core Graphics ha l'origine in basso a sinistra; l'interfaccia del renderer
    // (e tutto il codice di disegno gia' scritto) assume y crescente verso il
    // basso. Si ribalta una volta qui invece di ricordarsene a ogni chiamata.
    CGContextTranslateCTM(ctx, 0.0, static_cast<CGFloat>(ph));
    CGContextScaleCTM(ctx, scale, -scale);

    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextSetShouldAntialias(ctx, true);
    return true;
}

bool Renderer::init(GraphicsCore& core, void* nativeWindow) {
    auto& d = *impl_;
    d.core = &core;
    d.view = (__bridge NSView*)nativeWindow;
    if (!d.view) return false;

    d.view.wantsLayer = YES;
    d.scale = d.view.window ? d.view.window.backingScaleFactor : 1.0;

    const NSRect b = d.view.bounds;
    if (!d.createContext(static_cast<unsigned>(b.size.width),
                         static_cast<unsigned>(b.size.height))) {
        return false;
    }
    d.buildLayers();
    return uploadSprites();
}

void Renderer::shutdown() {
    auto& d = *impl_;
    d.destroyContext();
    d.sprites = 0;
    d.view = nil;
    d.core = nullptr;
}

bool Renderer::uploadSprites() {
    impl_->sprites = impl_->core ? impl_->core->spriteCount() : 0;
    return impl_->sprites > 0;
}

void Renderer::resize(unsigned width, unsigned height) {
    impl_->createContext(width, height);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    impl_->layoutLayers();
    [CATransaction commit];
}

Size Renderer::size() const {
    return Size{static_cast<float>(impl_->width), static_cast<float>(impl_->height)};
}

bool Renderer::ready() const noexcept { return impl_->ctx != nullptr; }

int Renderer::spriteCount() const noexcept { return static_cast<int>(impl_->sprites); }

void Renderer::begin(Color clear) {
    auto& d = *impl_;
    if (!d.ctx) return;

    // Una sola transazione per fotogramma, con le animazioni implicite spente:
    // senza questo Core Animation interpolerebbe da solo ogni cambio di frame
    // e opacita' su 0,25 s, e lo zoom risulterebbe in ritardo e molliccio.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];

    d.usedSprites = 0;

    // Lo sfondo ora e' il layer contenitore. Il contesto CPU si azzera
    // trasparente perche' e' diventato un velo sopra la foto: dove non ci
    // disegna niente si deve vedere lo sprite sotto.
    if (d.view.layer) {
        CGFloat comp[4] = {clear.r, clear.g, clear.b, clear.a};
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGColorRef bg = CGColorCreate(cs, comp);
        CGColorSpaceRelease(cs);
        d.view.layer.backgroundColor = bg;
        CGColorRelease(bg);
    }

    CGContextSaveGState(d.ctx);
    CGContextClearRect(d.ctx, CGRectMake(0, 0, d.width, d.height));
}

bool Renderer::end() {
    auto& d = *impl_;
    if (!d.ctx) return false;
    CGContextRestoreGState(d.ctx);

    CGImageRef frame = CGBitmapContextCreateImage(d.ctx);
    if (!frame) { [CATransaction commit]; return false; }

    // Consegna al layer: il compositore fa il resto. Va fatto sul thread
    // principale, che e' anche quello del ciclo di disegno.
    d.uiLayer.contents = (__bridge id)frame;
    CGImageRelease(frame);

    // Gli sprite che questo fotogramma non ha chiesto vanno nascosti, o
    // resterebbero appesi quelli del fotogramma prima (per esempio tornando
    // alla pagina d'ingresso).
    for (int i = d.usedSprites; i < 2; ++i) d.clipLayer[i].hidden = YES;

    [CATransaction commit];
    return true;
}

void Renderer::drawSprite(int index, float scale, float opacity) {
    const Size win = size();
    drawSpriteIn(index, rect(0, 0, win.width, win.height), scale, opacity);
}

void Renderer::drawSpriteIn(int index, Rect box, float scale, float opacity) {
    auto& d = *impl_;
    if (!d.ctx || !d.core) return;
    if (index < 0 || index >= static_cast<int>(d.core->impl_->sources.size())) return;
    if (opacity <= 0.004f) return;   // alpha ~0: puro overdraw, come nel backend Windows

    CGImageRef img = d.core->impl_->sources[static_cast<std::size_t>(index)];
    if (!img) return;

    const float iw = static_cast<float>(CGImageGetWidth(img));
    const float ih = static_cast<float>(CGImageGetHeight(img));
    if (iw <= 0 || ih <= 0) return;

    const float bw = box.width();
    const float bh = box.height();
    if (bw <= 0 || bh <= 0) return;

    // Scala "cover": l'immagine copre sempre il riquadro, come il layout web.
    const float cover = std::max(bw / iw, bh / ih);
    const float s     = cover * scale;

    const float w = iw * s;
    const float h = ih * s;
    const float x = box.left + (bw - w) * 0.5f;
    const float y = box.top + (bh - h) * 0.5f;

    // Oltre due sprite per fotogramma non se ne sono mai chiesti (il crossfade
    // ne usa due); se un giorno servissero, qui e' dove aggiungere layer.
    if (!d.uiLayer || d.usedSprites >= 2) return;
    const int slot = d.usedSprites++;

    // Da y verso il basso (convenzione del renderer) a y verso l'alto
    // (convenzione di Core Animation). E' l'unico punto in cui si converte.
    const CGFloat clipY = static_cast<CGFloat>(d.height) - box.bottom;
    d.clipLayer[slot].frame = CGRectMake(box.left, clipY, bw, bh);

    // Dentro il ritaglio le coordinate ripartono da zero, sempre con y in su.
    const CGFloat picY = bh - ((y - box.top) + h);
    d.picLayer[slot].frame = CGRectMake(x - box.left, picY, w, h);

    if (d.picLayer[slot].contents != (__bridge id)img) {
        d.picLayer[slot].contents = (__bridge id)img;
    }
    d.clipLayer[slot].opacity = opacity;
    d.clipLayer[slot].hidden  = NO;
}

void Renderer::fillRect(Rect r, Color color, float radius) {
    auto& d = *impl_;
    if (!d.ctx) return;
    setColor(d.ctx, color, true);
    if (radius > 0.0f) {
        addRoundedRect(d.ctx, toCG(r), radius);
        CGContextFillPath(d.ctx);
    } else {
        CGContextFillRect(d.ctx, toCG(r));
    }
}

void Renderer::fillRectGradient(Rect r, Color top, Color bottom, float radius) {
    auto& d = *impl_;
    if (!d.ctx) return;

    CGContextSaveGState(d.ctx);
    if (radius > 0.0f) addRoundedRect(d.ctx, toCG(r), radius);
    else               CGContextAddRect(d.ctx, toCG(r));
    CGContextClip(d.ctx);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat comps[8] = {top.r, top.g, top.b, top.a, bottom.r, bottom.g, bottom.b, bottom.a};
    CGGradientRef  grad    = CGGradientCreateWithColorComponents(cs, comps, nullptr, 2);

    // Leggermente diagonale (10 gradi dalla verticale, verso destra scendendo)
    // invece di puramente verticale: da' profondita' senza essere appariscente.
    // La linea passa per il centro del rettangolo, cosi' l'inclinazione si
    // vede allo stesso modo qualunque sia la larghezza.
    constexpr CGFloat kTiltDeg = 10.0;
    const CGFloat cx    = (r.left + r.right) * 0.5;
    const CGFloat halfH = (r.bottom - r.top) * 0.5;
    const CGFloat dx    = halfH * std::tan(kTiltDeg * M_PI / 180.0);
    CGContextDrawLinearGradient(d.ctx, grad, CGPointMake(cx - dx, r.top),
                               CGPointMake(cx + dx, r.bottom), 0);
    CGGradientRelease(grad);
    CGColorSpaceRelease(cs);
    CGContextRestoreGState(d.ctx);
}

void Renderer::fillRectShadow(Rect r, Color color, float radius, Color shadowColor,
                              float shadowBlur, Point shadowOffset) {
    auto& d = *impl_;
    if (!d.ctx) return;

    CGContextSaveGState(d.ctx);
    CGColorRef sc = CGColorCreateGenericRGB(shadowColor.r, shadowColor.g, shadowColor.b,
                                            shadowColor.a);
    // L'offset e' in coordinate utente e passa per la stessa CTM ribaltata del
    // resto: dy positivo qui vuol dire "verso il basso", come ovunque altrove.
    CGContextSetShadowWithColor(d.ctx, CGSizeMake(shadowOffset.x, shadowOffset.y), shadowBlur, sc);
    CGColorRelease(sc);

    setColor(d.ctx, color, true);
    if (radius > 0.0f) {
        addRoundedRect(d.ctx, toCG(r), radius);
        CGContextFillPath(d.ctx);
    } else {
        CGContextFillRect(d.ctx, toCG(r));
    }
    CGContextRestoreGState(d.ctx);
}

// Funzione di ripartizione della normale standard: quanta parte di un bordo
// sfocato con sigma 1 e' "coperta" alla distanza x (positiva verso l'interno).
inline float gaussCdf(float x) noexcept {
    return 0.5f * (1.0f + std::erf(x / std::sqrt(2.0f)));
}

// Le due primitive "sfocate" non usano il blur di Core Graphics: l'unico
// disponibile e' quello delle ombre, e il trucco classico (disegnare la forma
// fuori dal ritaglio con un'ombra che ricade dentro) qui non funzionava -
// con la forma spostata fuori dalla finestra Core Graphics non disegnava
// nemmeno l'ombra. Si costruisce invece il profilo gaussiano a gradini, con
// forme concentriche: per un bordo sfocato a queste dimensioni (sigma di
// qualche pixel) i gradini non si vedono.

void Renderer::fillRectBlurred(Rect r, Color color, float radius, float sigma) {
    auto& d = *impl_;
    if (!d.ctx) return;
    if (sigma <= 0.05f) { fillRect(r, color, radius); return; }

    // Pillole annidate da -2 sigma (fuori) a +2 sigma (dentro); l'opacita' di
    // ciascuna e' quella che, composta sulle precedenti, riproduce la CDF
    // gaussiana. L'ultima e' piena: da +2 sigma in dentro il colore e' intero.
    constexpr int kSteps = 10;
    float covered = 0.0f;
    for (int k = 0; k < kSteps; ++k) {
        const float inset  = (-2.0f + 4.0f * (static_cast<float>(k) + 0.5f) / kSteps) * sigma;
        const float target = (k == kSteps - 1) ? 1.0f : gaussCdf(inset / sigma);
        const float alpha  = (target - covered) / std::max(1.0f - covered, 1e-4f);
        covered            = target;
        const Rect ring = rect(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset);
        if (ring.width() <= 0.0f || ring.height() <= 0.0f) break;
        fillRect(ring, {color.r, color.g, color.b, color.a * std::clamp(alpha, 0.0f, 1.0f)},
                 std::max(0.0f, radius - inset));
    }
}

void Renderer::fillInnerGlow(Rect r, Color color, float radius, float sigma, float spread) {
    auto& d = *impl_;
    if (!d.ctx || sigma <= 0.05f) return;

    CGContextSaveGState(d.ctx);
    if (radius > 0.0f) addRoundedRect(d.ctx, toCG(r), radius);
    else               CGContextAddRect(d.ctx, toCG(r));
    CGContextClip(d.ctx);

    // La parte piena (spread) e' un anello solo; poi fasce concentriche
    // disgiunte fino a 3 sigma: l'ombra interna vale 1 - CDF(x/sigma) alla
    // profondita' x oltre lo spread, e ogni fascia prende il valore al suo
    // centro. Non si sovrappongono, quindi niente composizione da compensare.
    if (spread > 0.0f) {
        const float half = spread * 0.5f;
        drawRectOutline(rect(r.left + half, r.top + half, r.right - half, r.bottom - half), color,
                        spread, std::max(0.0f, radius - half));
    }
    constexpr int kBands = 8;
    const float   w      = 3.0f * sigma / kBands;
    for (int k = 0; k < kBands; ++k) {
        const float soft  = (static_cast<float>(k) + 0.5f) * w;
        const float depth = spread + soft;
        const float alpha = 1.0f - gaussCdf(soft / sigma);
        if (alpha < 0.004f) break;
        const Rect band = rect(r.left + depth, r.top + depth, r.right - depth, r.bottom - depth);
        drawRectOutline(band, {color.r, color.g, color.b, color.a * alpha}, w,
                        std::max(0.0f, radius - depth));
    }
    CGContextRestoreGState(d.ctx);
}

void Renderer::drawRectOutlineGradient(Rect r, const Color* stops, const float* positions,
                                       int count, Point from, Point to, float stroke,
                                       float radius) {
    auto& d = *impl_;
    if (!d.ctx || !stops || !positions || count < 2) return;

    CGContextSaveGState(d.ctx);
    CGContextSetLineWidth(d.ctx, stroke);
    if (radius > 0.0f) addRoundedRect(d.ctx, toCG(r), radius);
    else               CGContextAddRect(d.ctx, toCG(r));
    CGContextReplacePathWithStrokedPath(d.ctx);
    CGContextClip(d.ctx);

    std::vector<CGFloat> comps(static_cast<std::size_t>(count) * 4);
    std::vector<CGFloat> locs(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        comps[i * 4 + 0] = stops[i].r;
        comps[i * 4 + 1] = stops[i].g;
        comps[i * 4 + 2] = stops[i].b;
        comps[i * 4 + 3] = stops[i].a;
        locs[i]          = positions[i];
    }
    CGColorSpaceRef cs   = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef   grad = CGGradientCreateWithColorComponents(cs, comps.data(), locs.data(),
                                                               static_cast<std::size_t>(count));
    CGContextDrawLinearGradient(d.ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation |
                                    kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(grad);
    CGColorSpaceRelease(cs);
    CGContextRestoreGState(d.ctx);
}

void Renderer::fillCircle(Point center, float radius, Color color) {
    auto& d = *impl_;
    if (!d.ctx || radius <= 0.0f) return;
    setColor(d.ctx, color, true);
    CGContextFillEllipseInRect(
        d.ctx, CGRectMake(center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f));
}

void Renderer::fillRects(const Rect* rects, int count, Color color) {
    auto& d = *impl_;
    if (!d.ctx || !rects || count <= 0) return;
    setColor(d.ctx, color, true);

    // Antialiasing spento per tutto il gruppo. A un pixel di lato non arrotonda
    // nessuno spigolo - non c'e' spigolo da arrotondare - ma manda ogni
    // rettangolo nel rasterizzatore con copertura parziale: misurato col
    // profiler, aa_render e argb32_mark_constmask erano i tre quarti del costo
    // dell'intera schermata d'ingresso. Spento, i puntini restano netti e il
    // campo torna dentro il fotogramma.
    CGContextSaveGState(d.ctx);
    CGContextSetShouldAntialias(d.ctx, false);

    // A blocchi, per non tenere in piedi un vettore grande quanto il campo.
    constexpr int kBatch = 512;
    CGRect buf[kBatch];
    int n = 0;
    for (int i = 0; i < count; ++i) {
        buf[n++] = toCG(rects[i]);
        if (n == kBatch) { CGContextFillRects(d.ctx, buf, static_cast<std::size_t>(n)); n = 0; }
    }
    if (n > 0) CGContextFillRects(d.ctx, buf, static_cast<std::size_t>(n));
    CGContextRestoreGState(d.ctx);
}

void Renderer::fillCircleGradient(Point center, float radius, Color inner, Color mid,
                                  Color outer) {
    auto& d = *impl_;
    if (!d.ctx || radius <= 0.0f) return;

    CGContextSaveGState(d.ctx);
    CGContextAddEllipseInRect(
        d.ctx, CGRectMake(center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f));
    CGContextClip(d.ctx);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat comps[12] = {
        inner.r, inner.g, inner.b, inner.a, mid.r, mid.g, mid.b, mid.a,
        outer.r, outer.g, outer.b, outer.a,
    };
    const CGFloat locs[3] = {0.0, 0.55, 1.0};
    CGGradientRef  grad   = CGGradientCreateWithColorComponents(cs, comps, locs, 3);
    const CGPoint  c      = CGPointMake(center.x, center.y);
    CGContextDrawRadialGradient(d.ctx, grad, c, 0.0, c, radius, 0);
    CGGradientRelease(grad);
    CGColorSpaceRelease(cs);
    CGContextRestoreGState(d.ctx);
}

void Renderer::drawArc(Point center, float radius, float startDeg, float endDeg, float thickness,
                       Color color) {
    auto& d = *impl_;
    if (!d.ctx || radius <= 0.0f) return;

    // Punto per punto invece di CGContextAddArc: cosi' l'angolo si legge nella
    // stessa convenzione (y verso il basso) di tutto il resto del file, senza
    // doversi fidare di come CG interpreta "senso orario" sotto una CTM
    // ribaltata.
    const double startRad = startDeg * M_PI / 180.0;
    const double endRad   = endDeg * M_PI / 180.0;
    const int    segments = std::max(2, static_cast<int>(std::abs(endDeg - startDeg) / 3.0) + 1);

    setColor(d.ctx, color, false);
    CGContextSetLineWidth(d.ctx, thickness);
    CGContextSetLineCap(d.ctx, kCGLineCapRound);
    CGContextBeginPath(d.ctx);
    for (int i = 0; i <= segments; ++i) {
        const double t  = startRad + (endRad - startRad) * (static_cast<double>(i) / segments);
        const float  px = center.x + radius * static_cast<float>(std::cos(t));
        const float  py = center.y + radius * static_cast<float>(std::sin(t));
        if (i == 0) CGContextMoveToPoint(d.ctx, px, py);
        else        CGContextAddLineToPoint(d.ctx, px, py);
    }
    CGContextStrokePath(d.ctx);
}

void Renderer::drawRectOutline(Rect r, Color color, float stroke, float radius) {
    auto& d = *impl_;
    if (!d.ctx) return;
    setColor(d.ctx, color, false);
    CGContextSetLineWidth(d.ctx, stroke);
    if (radius > 0.0f) {
        addRoundedRect(d.ctx, toCG(r), radius);
        CGContextStrokePath(d.ctx);
    } else {
        CGContextStrokeRect(d.ctx, toCG(r));
    }
}

void Renderer::drawLine(Point a, Point b, Color color, float stroke) {
    auto& d = *impl_;
    if (!d.ctx) return;
    setColor(d.ctx, color, false);
    CGContextSetLineWidth(d.ctx, stroke);
    CGContextBeginPath(d.ctx);
    CGContextMoveToPoint(d.ctx, a.x, a.y);
    CGContextAddLineToPoint(d.ctx, b.x, b.y);
    CGContextStrokePath(d.ctx);
}

void Renderer::drawPolyline(const Point* pts, std::size_t count, Color color, float stroke) {
    auto& d = *impl_;
    if (!d.ctx || !pts || count < 2) return;
    setColor(d.ctx, color, false);
    CGContextSetLineWidth(d.ctx, stroke);
    CGContextSetLineJoin(d.ctx, kCGLineJoinRound);
    CGContextBeginPath(d.ctx);
    CGContextMoveToPoint(d.ctx, pts[0].x, pts[0].y);
    for (std::size_t i = 1; i < count; ++i) {
        CGContextAddLineToPoint(d.ctx, pts[i].x, pts[i].y);
    }
    CGContextStrokePath(d.ctx);
}

void Renderer::pushClip(Rect r) {
    if (!impl_->ctx) return;
    CGContextSaveGState(impl_->ctx);
    CGContextClipToRect(impl_->ctx, toCG(r));
}

void Renderer::popClip() {
    if (impl_->ctx) CGContextRestoreGState(impl_->ctx);
}

namespace {

/** Corpo comune di drawText/drawTextBody: il font arriva gia' fatto, non lo possiede. */
void drawFramedText(CGContextRef ctx, const std::wstring& text, Rect box, Color color,
                    TextAlign align, CTFontRef font, float tracking = 0.0f) {
    if (!ctx || text.empty()) return;

    CFStringRef cf = toCFString(text);
    if (!cf) return;

    CGColorRef cgColor = CGColorCreateGenericRGB(color.r, color.g, color.b, color.a);

    CTTextAlignment ctAlign = (align == TextAlign::Center) ? kCTTextAlignmentCenter
                                                           : kCTTextAlignmentLeft;
    CTParagraphStyleSetting settings[] = {
        {kCTParagraphStyleSpecifierAlignment, sizeof(ctAlign), &ctAlign},
    };
    CTParagraphStyleRef para = CTParagraphStyleCreate(settings, 1);

    // La spaziatura (kCTKernAttributeName, in punti) va nel dizionario solo se
    // richiesta: con la chiave presente a 0 CoreText spegne la crenatura
    // del font, che invece per il testo normale deve restare.
    const double kern = tracking;
    CFNumberRef kernRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kern);
    CFStringRef keys[]   = {kCTFontAttributeName, kCTForegroundColorAttributeName,
                            kCTParagraphStyleAttributeName, kCTKernAttributeName};
    CFTypeRef   values[] = {font, cgColor, para, kernRef};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(values), tracking != 0.0f ? 4 : 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(kernRef);

    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cf, attrs);
    CTFramesetterRef      setter     = CTFramesetterCreateWithAttributedString(attributed);

    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, nullptr, CGRectMake(0, 0, box.width(), box.height()));
    CTFrameRef frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, nullptr);

    CGContextSaveGState(ctx);
    // Il testo si compone in coordinate y-su: si annulla il ribaltamento globale
    // per il tempo di disegnarlo, invece di ribaltare ogni glifo.
    CGContextTranslateCTM(ctx, box.left, box.top + box.height());
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CTFrameDraw(frame, ctx);
    CGContextRestoreGState(ctx);

    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(setter);
    CFRelease(attributed);
    CFRelease(attrs);
    CFRelease(para);
    CGColorRelease(cgColor);
    CFRelease(cf);
}

/** Corpo comune di drawTextCentered/drawTextBodyCentered: stessa idea, font non posseduto. */
void drawCenteredText(CGContextRef ctx, const std::wstring& text, Point center, Color color,
                      CTFontRef font, float tracking = 0.0f) {
    if (!ctx || text.empty()) return;

    CFStringRef cf = toCFString(text);
    if (!cf) return;

    CGColorRef cgColor = CGColorCreateGenericRGB(color.r, color.g, color.b, color.a);

    const double kern = tracking;
    CFNumberRef kernRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kern);
    CFStringRef keys[]   = {kCTFontAttributeName, kCTForegroundColorAttributeName,
                            kCTKernAttributeName};
    CFTypeRef   values[] = {font, cgColor, kernRef};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(values), tracking != 0.0f ? 3 : 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(kernRef);

    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cf, attrs);
    CTLineRef              line      = CTLineCreateWithAttributedString(attributed);

    CGFloat      ascent = 0, descent = 0, leading = 0;
    const double width  = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

    CGContextSaveGState(ctx);
    // Stessa idea del ribaltamento locale in drawText, ma l'origine locale va
    // esattamente sulla baseline: e' il punto che CTLineDraw prende per buono,
    // e sposandola di (ascesa - discesa)/2 sotto il centro la riga risulta
    // centrata sul punto dato invece che ancorata al top di un riquadro.
    CGContextTranslateCTM(ctx, center.x - static_cast<CGFloat>(width) * 0.5,
                          center.y + (ascent - descent) * 0.5);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextSetTextPosition(ctx, 0, 0);
    CTLineDraw(line, ctx);
    CGContextRestoreGState(ctx);

    CFRelease(line);
    CFRelease(attributed);
    CFRelease(attrs);
    CGColorRelease(cgColor);
    CFRelease(cf);
}

} // namespace

void Renderer::drawText(const std::wstring& text, Rect box, float fontSize, Color color,
                        TextAlign align, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = cachedTextFont(fontSize, bold ? 640.0 : 400.0);
    drawFramedText(impl_->ctx, text, box, color, align, font);
}

void Renderer::drawTextBody(const std::wstring& text, Rect box, float fontSize, Color color,
                            TextAlign align, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = cachedBodyFont(fontSize, bold);
    drawFramedText(impl_->ctx, text, box, color, align, font);
}

void Renderer::drawTextCentered(const std::wstring& text, Point center, float fontSize,
                                Color color, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = cachedTextFont(fontSize, bold ? 640.0 : 400.0);
    drawCenteredText(impl_->ctx, text, center, color, font);
}

void Renderer::drawTextBodyCentered(const std::wstring& text, Point center, float fontSize,
                                    Color color, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = cachedBodyFont(fontSize, bold);
    drawCenteredText(impl_->ctx, text, center, color, font);
}

Size Renderer::measureTextBody(const std::wstring& text, float fontSize, bool bold,
                               float maxWidth) const {
    if (text.empty()) return {0.0f, 0.0f};

    CFStringRef cf = toCFString(text);
    if (!cf) return {0.0f, 0.0f};

    CTFontRef       font   = cachedBodyFont(fontSize, bold);
    CFStringRef     keys[] = {kCTFontAttributeName};
    CFTypeRef       vals[] = {font};
    CFDictionaryRef attrs  = CFDictionaryCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(vals), 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cf, attrs);
    CTFramesetterRef      setter     = CTFramesetterCreateWithAttributedString(attributed);

    CFRange fitted{};
    const CGSize suggerita = CTFramesetterSuggestFrameSizeWithConstraints(
        setter, CFRangeMake(0, 0), nullptr, CGSizeMake(maxWidth, 100000.0), &fitted);

    CFRelease(setter);
    CFRelease(attributed);
    CFRelease(attrs);
    CFRelease(cf);

    // Arrotondato per eccesso: una misura che sottostima di mezzo pixel fa
    // sparire l'ultimo glifo quando il risultato viene usato come larghezza
    // del riquadro in cui poi si disegna.
    return {static_cast<float>(std::ceil(suggerita.width)),
            static_cast<float>(std::ceil(suggerita.height))};
}

void Renderer::drawParagraph(const std::wstring& text, Rect box, float fontSize, Color color,
                             TextAlign align, float lineHeight) {
    if (!impl_->ctx || text.empty()) return;

    CFStringRef cf = toCFString(text);
    if (!cf) return;

    CTFontRef  font    = cachedBodyFont(fontSize, false);
    CGColorRef cgColor = CGColorCreateGenericRGB(color.r, color.g, color.b, color.a);

    CTTextAlignment ctAlign = (align == TextAlign::Center) ? kCTTextAlignmentCenter
                                                           : kCTTextAlignmentLeft;
    // Altezza di riga FISSA in punti, non un moltiplicatore: LineHeightMultiple
    // moltiplica il leading naturale del font (per Manrope ~1.35 del corpo),
    // quindi 1.5 dava righe distanti il doppio del voluto e l'ultima usciva
    // dal riquadro. Fissando minimo e massimo allo stesso valore, "interlinea
    // 1.5" vuol dire davvero 1.5 volte il corpo, che e' come si ragiona
    // impaginando.
    const CGFloat altezzaRiga = fontSize * lineHeight;
    CTParagraphStyleSetting settings[] = {
        {kCTParagraphStyleSpecifierAlignment, sizeof(ctAlign), &ctAlign},
        {kCTParagraphStyleSpecifierMinimumLineHeight, sizeof(altezzaRiga), &altezzaRiga},
        {kCTParagraphStyleSpecifierMaximumLineHeight, sizeof(altezzaRiga), &altezzaRiga},
    };
    CTParagraphStyleRef para = CTParagraphStyleCreate(settings, 3);

    CFStringRef keys[]   = {kCTFontAttributeName, kCTForegroundColorAttributeName,
                            kCTParagraphStyleAttributeName};
    CFTypeRef   values[] = {font, cgColor, para};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(values), 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cf, attrs);
    CTFramesetterRef      setter     = CTFramesetterCreateWithAttributedString(attributed);

    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, nullptr, CGRectMake(0, 0, box.width(), box.height()));
    CTFrameRef frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, nullptr);

    CGContextSaveGState(impl_->ctx);
    CGContextTranslateCTM(impl_->ctx, box.left, box.top + box.height());
    CGContextScaleCTM(impl_->ctx, 1.0, -1.0);
    CTFrameDraw(frame, impl_->ctx);
    CGContextRestoreGState(impl_->ctx);

    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(setter);
    CFRelease(attributed);
    CFRelease(attrs);
    CFRelease(para);
    CGColorRelease(cgColor);
    CFRelease(cf);
}

void Renderer::drawTextDisplay(const std::wstring& text, Rect box, float fontSize, Color color,
                               TextAlign align, float tracking) {
    if (!impl_->ctx) return;
    drawFramedText(impl_->ctx, text, box, color, align, cachedDisplayFont(fontSize), tracking);
}

void Renderer::drawTextDisplayCentered(const std::wstring& text, Point center, float fontSize,
                                       Color color, float tracking) {
    if (!impl_->ctx) return;
    drawCenteredText(impl_->ctx, text, center, color, cachedDisplayFont(fontSize), tracking);
}

Size Renderer::measureTextDisplay(const std::wstring& text, float fontSize,
                                  float tracking) const {
    if (text.empty()) return {0.0f, 0.0f};

    CFStringRef cf = toCFString(text);
    if (!cf) return {0.0f, 0.0f};

    CTFontRef   font = cachedDisplayFont(fontSize);
    const double kern = tracking;
    CFNumberRef kernRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kern);
    CFStringRef keys[] = {kCTFontAttributeName, kCTKernAttributeName};
    CFTypeRef   vals[] = {font, kernRef};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(vals), tracking != 0.0f ? 2 : 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(kernRef);

    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, cf, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(attributed);
    CGFloat ascent = 0, descent = 0, leading = 0;
    const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CFRelease(line);
    CFRelease(attributed);
    CFRelease(attrs);
    CFRelease(cf);
    return {static_cast<float>(std::ceil(width)),
            static_cast<float>(std::ceil(ascent + descent + leading))};
}

namespace {

/** Costruisce la path cubica di fillCubicPath/pushClipCubicPath; chi chiama la rilascia. */
CGMutablePathRef cubicPath(const Point* pts, int count) {
    if (!pts || count < 4) return nullptr;
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathMoveToPoint(path, nullptr, pts[0].x, pts[0].y);
    for (int i = 1; i + 2 < count; i += 3) {
        CGPathAddCurveToPoint(path, nullptr, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y,
                              pts[i + 2].x, pts[i + 2].y);
    }
    CGPathCloseSubpath(path);
    return path;
}

} // namespace

void Renderer::fillCubicPath(const Point* pts, int count, Color color) {
    auto& d = *impl_;
    if (!d.ctx) return;
    CGMutablePathRef path = cubicPath(pts, count);
    if (!path) return;
    setColor(d.ctx, color, true);
    CGContextBeginPath(d.ctx);
    CGContextAddPath(d.ctx, path);
    CGContextFillPath(d.ctx);
    CGPathRelease(path);
}

namespace {

/** Costruisce la path dei poligoni di fillPolygons/pushClipPolygons; chi chiama la rilascia. */
CGMutablePathRef polygonPath(const Point* pts, const int* lengths, int shapes) {
    if (!pts || !lengths || shapes <= 0) return nullptr;
    CGMutablePathRef path = CGPathCreateMutable();
    int base = 0;
    for (int s = 0; s < shapes; ++s) {
        const int n = lengths[s];
        if (n >= 3) {
            CGPathMoveToPoint(path, nullptr, pts[base].x, pts[base].y);
            for (int i = 1; i < n; ++i) {
                CGPathAddLineToPoint(path, nullptr, pts[base + i].x, pts[base + i].y);
            }
            CGPathCloseSubpath(path);
        }
        base += n;
    }
    return path;
}

} // namespace

void Renderer::fillPolygons(const Point* pts, const int* lengths, int shapes, Color color) {
    auto& d = *impl_;
    if (!d.ctx) return;
    CGMutablePathRef path = polygonPath(pts, lengths, shapes);
    if (!path) return;
    setColor(d.ctx, color, true);
    CGContextBeginPath(d.ctx);
    CGContextAddPath(d.ctx, path);
    // Non-zero e non even-odd: i contorni sono tutti nello stesso verso, e i
    // bordi in comune fra due pezzi adiacenti devono sparire, non bucare.
    CGContextFillPath(d.ctx);
    CGPathRelease(path);
}

void Renderer::pushClipPolygons(const Point* pts, const int* lengths, int shapes) {
    auto& d = *impl_;
    if (!d.ctx) return;
    CGContextSaveGState(d.ctx);
    CGMutablePathRef path = polygonPath(pts, lengths, shapes);
    if (!path) {
        CGContextClipToRect(d.ctx, CGRectZero);
        return;
    }
    CGContextBeginPath(d.ctx);
    CGContextAddPath(d.ctx, path);
    CGContextClip(d.ctx);
    CGPathRelease(path);
}

void Renderer::pushClipCubicPath(const Point* pts, int count) {
    auto& d = *impl_;
    if (!d.ctx) return;
    // Sempre in coppia con popClip, anche se la path non e' valida: chi
    // chiama non deve contare i casi.
    CGContextSaveGState(d.ctx);
    CGMutablePathRef path = cubicPath(pts, count);
    if (!path) {
        CGContextClipToRect(d.ctx, CGRectZero);
        return;
    }
    CGContextBeginPath(d.ctx);
    CGContextAddPath(d.ctx, path);
    CGContextClip(d.ctx);
    CGPathRelease(path);
}

} // namespace mz::render
