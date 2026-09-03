// Backend Windows del renderer: Direct2D + DirectWrite + WIC, tutto dal Windows
// SDK e senza dipendenze esterne.
//
// L'ordine degli include conta. windows.h definisce DrawText come macro verso
// DrawTextW: se d2d1.h viene letto dopo, il metodo ID2D1RenderTarget::DrawText
// finisce dichiarato come DrawTextW e non è più chiamabile col suo nome. Quindi
// prima windows.h, poi si toglie la macro, poi i header Direct2D.
#include <windows.h>

#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winrt/base.h>

#include "render/renderer.hpp"

#include <algorithm>
#include <vector>

namespace mz::render {
namespace {

D2D1_COLOR_F toD2D(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
D2D1_RECT_F  toD2D(Rect r) { return D2D1::RectF(r.left, r.top, r.right, r.bottom); }

/**
 * DPI del monitor su cui sta la finestra.
 *
 * Serve a far sì che le coordinate passate al renderer siano DIP e non pixel:
 * un pannello largo 520 deve occupare la stessa porzione di schermo su un
 * monitor a 96 DPI e su un 4K a 192, altrimenti su quest'ultimo l'interfaccia
 * risulta grande la metà e il testo illeggibile.
 *
 * GetDpiForWindow esiste da Windows 10 1607: caricata dinamicamente, con il DPI
 * di sistema come ripiego sulle versioni precedenti.
 */
float windowDpi(HWND hwnd) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpi = UINT(WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<GetDpi>(
                reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")))) {
            const UINT dpi = fn(hwnd);
            if (dpi > 0) return static_cast<float>(dpi);
        }
    }
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return (dpi > 0) ? static_cast<float>(dpi) : 96.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// GraphicsCore
// ---------------------------------------------------------------------------

struct GraphicsCore::Impl {
    winrt::com_ptr<ID2D1Factory>       d2d;
    winrt::com_ptr<IDWriteFactory>     dwrite;
    winrt::com_ptr<IWICImagingFactory> wic;

    std::vector<winrt::com_ptr<IWICFormatConverter>> sources;

    struct CachedFormat {
        float                             size;
        bool                              bold;
        TextAlign                         align;
        winrt::com_ptr<IDWriteTextFormat> format;
    };
    std::vector<CachedFormat> formats;

    IDWriteTextFormat* formatFor(float size, bool bold, TextAlign align);
};

GraphicsCore::GraphicsCore() : impl_(std::make_unique<Impl>()) {}
GraphicsCore::~GraphicsCore() = default;

bool GraphicsCore::init() {
    auto& d = *impl_;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d.d2d.put()))) return false;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(d.dwrite.put())))) {
        return false;
    }

    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(d.wic.put())));
}

void GraphicsCore::shutdown() {
    auto& d = *impl_;
    d.formats.clear();
    d.sources.clear();
    d.wic    = nullptr;
    d.dwrite = nullptr;
    d.d2d    = nullptr;
}

std::size_t GraphicsCore::spriteCount() const noexcept { return impl_->sources.size(); }

bool GraphicsCore::loadSprites(const std::wstring& dir, const int* magnitudes, int count) {
    auto& d = *impl_;
    d.sources.clear();
    d.sources.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        // Ordine di preferenza: JPEG per primo perché è l'unico che ogni Windows
        // sa decodificare senza componenti aggiuntivi (il codec WebP di WIC c'è
        // d'ufficio su 11 ma non su 10). WebP e PNG restano accettati, comodi in
        // sviluppo dove si lavora direttamente sugli asset del progetto web.
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        bool opened = false;
        for (const wchar_t* ext : {L".jpg", L".webp", L".png"}) {
            const std::wstring path = dir + L"\\" + std::to_wstring(magnitudes[i]) + ext;
            decoder = nullptr;
            if (SUCCEEDED(d.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnLoad,
                                                           decoder.put()))) {
                opened = true;
                break;
            }
        }
        if (!opened) return false;

        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.put()))) return false;

        // D2D vuole BGRA premoltiplicato: la conversione va fatta una volta sola,
        // in caricamento, non per frame.
        winrt::com_ptr<IWICFormatConverter> converter;
        if (FAILED(d.wic->CreateFormatConverter(converter.put()))) return false;
        if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut))) {
            return false;
        }
        d.sources.push_back(converter);
    }
    return true;
}

IDWriteTextFormat* GraphicsCore::Impl::formatFor(float size, bool bold, TextAlign align) {
    for (auto& f : formats) {
        if (f.size == size && f.bold == bold && f.align == align) return f.format.get();
    }

    CachedFormat cf{size, bold, align, {}};
    if (FAILED(dwrite->CreateTextFormat(
            L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"it-IT",
            cf.format.put()))) {
        return nullptr;
    }
    cf.format->SetTextAlignment(align == TextAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                          : DWRITE_TEXT_ALIGNMENT_LEADING);
    cf.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    formats.push_back(cf);
    return formats.back().format.get();
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

struct Renderer::Impl {
    GraphicsCore* core = nullptr;
    HWND          hwnd = nullptr;

    winrt::com_ptr<ID2D1HwndRenderTarget>    target;
    winrt::com_ptr<ID2D1SolidColorBrush>     brush;
    std::vector<winrt::com_ptr<ID2D1Bitmap>> sprites;

    bool createTarget();
    bool upload();
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::init(GraphicsCore& core, void* nativeWindow) {
    impl_->core = &core;
    impl_->hwnd = static_cast<HWND>(nativeWindow);
    return impl_->createTarget();
}

void Renderer::shutdown() {
    auto& d = *impl_;
    d.sprites.clear();
    d.brush  = nullptr;
    d.target = nullptr;
    d.core   = nullptr;
}

bool Renderer::Impl::createTarget() {
    if (!core || !core->impl_->d2d) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const auto sz = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 1)),
                                static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 1)));

    target = nullptr;
    brush  = nullptr;
    sprites.clear();

    if (FAILED(core->impl_->d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, sz), target.put()))) {
        return false;
    }

    // Il filtro lineare conta: lo zoom scala le immagini ben oltre 1:1 e il
    // nearest neighbour renderebbe visibili i pixel durante il crossfade.
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Da qui in poi le coordinate passate al renderer sono DIP, non pixel: D2D
    // scala da solo. E' cio' che rende l'interfaccia della stessa dimensione
    // FISICA su schermi con densita' diverse - e size() torna anch'essa in DIP,
    // quindi tutta la geometria proporzionale gia' scritta continua a valere.
    const float dpi = windowDpi(hwnd);
    target->SetDpi(dpi, dpi);

    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush.put()))) {
        return false;
    }

    upload();
    return true;
}

bool Renderer::Impl::upload() {
    if (!core || !target) return false;

    sprites.clear();
    sprites.resize(core->spriteCount());
    for (std::size_t i = 0; i < core->spriteCount(); ++i) {
        if (FAILED(target->CreateBitmapFromWicBitmap(core->impl_->sources[i].get(), nullptr,
                                                     sprites[i].put()))) {
            return false;
        }
    }
    return true;
}

bool Renderer::uploadSprites() { return impl_->upload(); }

void Renderer::resize(unsigned width, unsigned height) {
    if (!impl_->target) return;

    impl_->target->Resize(D2D1::SizeU(std::max<unsigned>(width, 1u),
                                      std::max<unsigned>(height, 1u)));

    // Una finestra trascinata su uno schermo con scala diversa riceve un resize:
    // se il DPI non si aggiorna qui, l'interfaccia resta tarata sul monitor di
    // partenza e sull'altro esce di dimensione sbagliata.
    const float dpi = windowDpi(impl_->hwnd);
    impl_->target->SetDpi(dpi, dpi);
}

Size Renderer::size() const {
    if (!impl_->target) return Size{0.0f, 0.0f};
    const auto s = impl_->target->GetSize();
    return Size{s.width, s.height};
}

bool Renderer::ready() const noexcept { return static_cast<bool>(impl_->target); }

int Renderer::spriteCount() const noexcept { return static_cast<int>(impl_->sprites.size()); }

void Renderer::begin(Color clear) {
    impl_->target->BeginDraw();
    impl_->target->Clear(toD2D(clear));
}

bool Renderer::end() {
    const HRESULT hr = impl_->target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        impl_->createTarget();
        return false;
    }
    return SUCCEEDED(hr);
}

void Renderer::drawSprite(int index, float scale, float opacity) {
    const Size win = size();
    drawSpriteIn(index, rect(0, 0, win.width, win.height), scale, opacity);
}

void Renderer::drawSpriteIn(int index, Rect box, float scale, float opacity) {
    auto& d = *impl_;
    if (index < 0 || index >= static_cast<int>(d.sprites.size())) return;
    if (!d.sprites[static_cast<std::size_t>(index)]) return;
    if (opacity <= 0.004f) return;   // alpha ~0: puro overdraw, come nel JS

    ID2D1Bitmap* bmp = d.sprites[static_cast<std::size_t>(index)].get();
    const D2D1_SIZE_F img = bmp->GetSize();
    if (img.width <= 0 || img.height <= 0) return;

    const float bw = box.width();
    const float bh = box.height();
    if (bw <= 0 || bh <= 0) return;

    // Scala "cover": l'immagine copre sempre il riquadro, come il layout web.
    const float cover = std::max(bw / img.width, bh / img.height);
    const float s     = cover * scale;

    const float w = img.width * s;
    const float h = img.height * s;
    const float x = box.left + (bw - w) * 0.5f;
    const float y = box.top + (bh - h) * 0.5f;

    // Ritaglio: senza, la miniatura sborderebbe dal suo riquadro appena lo zoom
    // supera 1:1, e a schermo intero il ritaglio non costa nulla comunque.
    d.target->PushAxisAlignedClip(toD2D(box), D2D1_ANTIALIAS_MODE_ALIASED);
    d.target->DrawBitmap(bmp, D2D1::RectF(x, y, x + w, y + h), opacity,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    d.target->PopAxisAlignedClip();
}

void Renderer::fillRect(Rect r, Color color, float radius) {
    auto& d = *impl_;
    d.brush->SetColor(toD2D(color));
    if (radius > 0.0f) {
        d.target->FillRoundedRectangle(D2D1::RoundedRect(toD2D(r), radius, radius), d.brush.get());
    } else {
        d.target->FillRectangle(toD2D(r), d.brush.get());
    }
}

void Renderer::fillRects(const Rect* rects, int count, Color color) {
    auto& d = *impl_;
    if (!rects || count <= 0) return;
    // Direct2D non ha un equivalente di CGContextFillRects, ma il costo che il
    // raggruppamento evita su macOS - costruire un colore per ogni puntino -
    // qui non c'e': il pennello si imposta una volta e i rettangoli vanno in
    // fila.
    d.brush->SetColor(toD2D(color));
    for (int i = 0; i < count; ++i) d.target->FillRectangle(toD2D(rects[i]), d.brush.get());
}

void Renderer::drawRectOutline(Rect r, Color color, float stroke, float radius) {
    auto& d = *impl_;
    d.brush->SetColor(toD2D(color));
    if (radius > 0.0f) {
        d.target->DrawRoundedRectangle(D2D1::RoundedRect(toD2D(r), radius, radius),
                                       d.brush.get(), stroke);
    } else {
        d.target->DrawRectangle(toD2D(r), d.brush.get(), stroke);
    }
}

void Renderer::drawLine(Point a, Point b, Color color, float stroke) {
    auto& d = *impl_;
    d.brush->SetColor(toD2D(color));
    d.target->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), d.brush.get(), stroke);
}

void Renderer::drawPolyline(const Point* pts, std::size_t count, Color color, float stroke) {
    auto& d = *impl_;
    if (!pts || count < 2 || !d.core || !d.core->impl_->d2d) return;

    winrt::com_ptr<ID2D1PathGeometry> geometry;
    if (FAILED(d.core->impl_->d2d->CreatePathGeometry(geometry.put()))) return;

    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put()))) return;

    sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (std::size_t i = 1; i < count; ++i) {
        sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;

    d.brush->SetColor(toD2D(color));
    d.target->DrawGeometry(geometry.get(), d.brush.get(), stroke);
}

void Renderer::pushClip(Rect r) {
    impl_->target->PushAxisAlignedClip(toD2D(r), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderer::popClip() { impl_->target->PopAxisAlignedClip(); }

void Renderer::drawText(const std::wstring& text, Rect box, float fontSize, Color color,
                        TextAlign align, bool bold) {
    auto& d = *impl_;
    IDWriteTextFormat* fmt = d.core ? d.core->impl_->formatFor(fontSize, bold, align) : nullptr;
    if (!fmt) return;

    d.brush->SetColor(toD2D(color));
    d.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, toD2D(box),
                       d.brush.get());
}

} // namespace mz::render
