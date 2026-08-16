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

void Renderer::drawText(const std::wstring& text, Rect box, float fontSize, Color color,
                        TextAlign align, bool bold) {
    auto& d = *impl_;
    if (!d.ctx || text.empty()) return;

    CFStringRef cf = toCFString(text);
    if (!cf) return;

    // Helvetica Neue e' la controparte ragionevole di Segoe UI: presente su ogni
    // macOS, stesse proporzioni umanistiche, stessa leggibilita' a corpo piccolo.
    CTFontRef font = CTFontCreateWithName(
        bold ? CFSTR("HelveticaNeue-Medium") : CFSTR("HelveticaNeue"), fontSize, nullptr);

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

    CGContextSaveGState(d.ctx);
    // Il testo si compone in coordinate y-su: si annulla il ribaltamento globale
    // per il tempo di disegnarlo, invece di ribaltare ogni glifo.
    CGContextTranslateCTM(d.ctx, box.left, box.top + box.height());
    CGContextScaleCTM(d.ctx, 1.0, -1.0);
    CTFrameDraw(frame, d.ctx);
    CGContextRestoreGState(d.ctx);

    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(setter);
    CFRelease(attributed);
    CFRelease(attrs);
    CFRelease(para);
    CGColorRelease(cgColor);
    CFRelease(font);
    CFRelease(cf);
}

} // namespace mz::render
