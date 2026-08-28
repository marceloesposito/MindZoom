// Backend macOS del renderer: Core Graphics + Core Text + Image I/O.
//
// Stessa interfaccia del backend Windows (render/renderer.hpp), stessa
// semantica, stesse primitive. Chi disegna non cambia una riga.
//
// Scelta di Core Graphics e non Metal: il carico e' due quad texturati in
// crossfade piu' qualche primitiva 2D, cioe' esattamente cio' per cui Direct2D
// era stato scelto sull'altra sponda. Metal chiederebbe shader, pipeline state e
// un command buffer per fare la stessa cosa.
//
// Il modello begin/end non e' quello di NSView, che disegna quando gli viene
// chiesto: qui si disegna in un contesto bitmap fuori schermo e a end() lo si
// consegna al layer della vista. Cosi' il ciclo di gioco resta padrone del
// tempo, come su Windows.
//
// ATTENZIONE: questo file non e' mai stato compilato. E' stato scritto su
// Windows, dove non esiste un toolchain Objective-C. Va considerato un punto di
// partenza verificato nella logica ma non nella sintassi.

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include "render/renderer.hpp"

#include <algorithm>
#include <codecvt>
#include <locale>
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
 * confronto (dodici candidati renderizzati con questo stesso codice).
 *
 * Ha preso il posto di Space Grotesk, che era un grottesco geometrico -
 * tecnico, squadrato, giusto per un pannello di strumentazione ma in
 * contraddizione con una schermata che chiede di rilassarsi. Un serif porta le
 * grazie e il ritmo della pagina stampata: si legge come un libro invece che
 * come un cruscotto.
 *
 * DIFFERENZA rispetto ai font a variazione usati prima qui: Iowan e' una
 * famiglia STATICA, con facce separate per peso. Non ha l'asse 'wght', quindi
 * chiedere una variazione non avrebbe effetto e si otterrebbe sempre il
 * Roman: la faccia si sceglie per nome. La soglia a 550 sta in mezzo fra i due
 * soli pesi che l'applicazione chiede (400 per il corpo, 640 per i titoli).
 *
 * E' un font di SISTEMA, presente su macOS - per questo non viene impacchettato
 * come si faceva con Space Grotesk. CTFontCreateWithName non fallisce mai se il
 * nome non risolve (ripiega sul font di sistema), quindi anche su un Mac che
 * non lo avesse il testo esce comunque.
 */
CTFontRef createTextFont(CGFloat size, CGFloat weight) {
    return CTFontCreateWithName(weight >= 550.0 ? CFSTR("IowanOldStyle-Bold")
                                                : CFSTR("IowanOldStyle-Roman"),
                                size, nullptr);
}

/**
 * Helvetica Neue: la controparte umanistica di Segoe UI, presente su ogni
 * macOS - e il font da cui e' nato lo stile svizzero (Akzidenz-Grotesk prima,
 * Helvetica poi). Fa da corpo neutro sotto le intestazioni in Iowan Old Style:
 * l'accoppiata serif per i titoli e sans per il corpo e' la stessa
 * dell'impaginato editoriale, e mantiene leggibili le righe piccole di
 * diagnostica, dove le grazie a 12px si impasterebbero.
 */
CTFontRef createBodyFont(CGFloat size, bool bold) {
    return CTFontCreateWithName(bold ? CFSTR("HelveticaNeue-Medium") : CFSTR("HelveticaNeue"),
                                size, nullptr);
}

void setColor(CGContextRef ctx, Color c, bool fill) {
    if (fill) CGContextSetRGBFillColor(ctx, c.r, c.g, c.b, c.a);
    else      CGContextSetRGBStrokeColor(ctx, c.r, c.g, c.b, c.a);
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

bool GraphicsCore::loadSprites(const std::wstring& dir, int count) {
    auto& d = *impl_;
    d.release();
    d.sources.reserve(static_cast<std::size_t>(count));

    for (int i = 1; i <= count; ++i) {
        CGImageRef image = nullptr;

        // Stesso ordine di preferenza del backend Windows: JPEG per primo. Su
        // macOS il WebP e' decodificabile da Image I/O solo da Big Sur in avanti,
        // quindi la stessa ragione di prudenza vale anche qui.
        for (const wchar_t* ext : {L".jpg", L".webp", L".png"}) {
            const std::wstring path = dir + L"/" + std::to_wstring(i) + ext;
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
                image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
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
    // Oggi non serve a niente: sia il font d'accento (Iowan Old Style) sia
    // quello di corpo (Helvetica Neue) sono di sistema, e la cartella non
    // esiste nemmeno nel bundle. Resta perche' e' l'unico punto in cui
    // impacchettare un font tornerebbe utile - per esempio se si volesse un
    // carattere non presente su ogni Mac - e perche' il valore di ritorno non
    // e' mai stato fatale: chi chiama disegna comunque, col font di sistema.
    const std::wstring path = dir + L"/Accent.ttf";
    CFStringRef cfPath = toCFString(path);
    if (!cfPath) return false;

    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfPath,
                                                  kCFURLPOSIXPathStyle, false);
    CFRelease(cfPath);
    if (!url) return false;

    CFErrorRef error = nullptr;
    const bool ok = CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &error);
    if (error) CFRelease(error);
    CFRelease(url);
    return ok;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

struct Renderer::Impl {
    GraphicsCore* core = nullptr;
    NSView*       view = nil;

    CGContextRef  ctx    = nullptr;   // bitmap fuori schermo
    unsigned      width  = 0;
    unsigned      height = 0;
    CGFloat       scale  = 1.0;       // fattore Retina

    // Su macOS le immagini decodificate sono gia' pronte all'uso: non esiste il
    // passaggio "carica sul device" che Direct2D richiede. Le texture per
    // finestra sono quindi le stesse CGImage del core, e spriteCount() riporta
    // quante ne ha il core - non zero, che farebbe scattare la guardia del
    // selftest per un problema che qui non esiste.
    std::size_t sprites = 0;

    void destroyContext() {
        if (ctx) { CGContextRelease(ctx); ctx = nullptr; }
    }

    bool createContext(unsigned w, unsigned h);
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { impl_->destroyContext(); }

bool Renderer::Impl::createContext(unsigned w, unsigned h) {
    destroyContext();
    width  = std::max(w, 1u);
    height = std::max(h, 1u);

    const std::size_t pw = static_cast<std::size_t>(width * scale);
    const std::size_t ph = static_cast<std::size_t>(height * scale);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
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
}

Size Renderer::size() const {
    return Size{static_cast<float>(impl_->width), static_cast<float>(impl_->height)};
}

bool Renderer::ready() const noexcept { return impl_->ctx != nullptr; }

int Renderer::spriteCount() const noexcept { return static_cast<int>(impl_->sprites); }

void Renderer::begin(Color clear) {
    auto& d = *impl_;
    if (!d.ctx) return;
    CGContextSaveGState(d.ctx);
    setColor(d.ctx, clear, true);
    CGContextFillRect(d.ctx, CGRectMake(0, 0, d.width, d.height));
}

bool Renderer::end() {
    auto& d = *impl_;
    if (!d.ctx) return false;
    CGContextRestoreGState(d.ctx);

    CGImageRef frame = CGBitmapContextCreateImage(d.ctx);
    if (!frame) return false;

    // Consegna al layer: il compositore fa il resto. Va fatto sul thread
    // principale, che e' anche quello del ciclo di disegno.
    d.view.layer.contents = (__bridge id)frame;
    CGImageRelease(frame);
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

    CGContextSaveGState(d.ctx);
    CGContextClipToRect(d.ctx, toCG(box));
    CGContextSetAlpha(d.ctx, opacity);

    // Il contesto e' gia' ribaltato per avere y verso il basso; CGContextDrawImage
    // disegna con l'origine in basso, quindi va ribaltato di nuovo localmente,
    // altrimenti le immagini uscirebbero capovolte.
    CGContextTranslateCTM(d.ctx, x, y + h);
    CGContextScaleCTM(d.ctx, 1.0, -1.0);
    CGContextDrawImage(d.ctx, CGRectMake(0, 0, w, h), img);

    CGContextRestoreGState(d.ctx);
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

void Renderer::fillCircle(Point center, float radius, Color color) {
    auto& d = *impl_;
    if (!d.ctx || radius <= 0.0f) return;
    setColor(d.ctx, color, true);
    CGContextFillEllipseInRect(
        d.ctx, CGRectMake(center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f));
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
                    TextAlign align, CTFontRef font) {
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
                      CTFontRef font) {
    if (!ctx || text.empty()) return;

    CFStringRef cf = toCFString(text);
    if (!cf) return;

    CGColorRef cgColor = CGColorCreateGenericRGB(color.r, color.g, color.b, color.a);

    CFStringRef keys[]   = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef   values[] = {font, cgColor};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(values), 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

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
    CTFontRef font = createTextFont(fontSize, bold ? 640.0 : 400.0);
    drawFramedText(impl_->ctx, text, box, color, align, font);
    CFRelease(font);
}

void Renderer::drawTextBody(const std::wstring& text, Rect box, float fontSize, Color color,
                            TextAlign align, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = createBodyFont(fontSize, bold);
    drawFramedText(impl_->ctx, text, box, color, align, font);
    CFRelease(font);
}

void Renderer::drawTextCentered(const std::wstring& text, Point center, float fontSize,
                                Color color, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = createTextFont(fontSize, bold ? 640.0 : 400.0);
    drawCenteredText(impl_->ctx, text, center, color, font);
    CFRelease(font);
}

void Renderer::drawTextBodyCentered(const std::wstring& text, Point center, float fontSize,
                                    Color color, bool bold) {
    if (!impl_->ctx) return;
    CTFontRef font = createBodyFont(fontSize, bold);
    drawCenteredText(impl_->ctx, text, center, color, font);
    CFRelease(font);
}

} // namespace mz::render
