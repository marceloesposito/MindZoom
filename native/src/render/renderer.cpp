#include "render/renderer.hpp"

#include <algorithm>

namespace mz::render {
namespace {

D2D1_COLOR_F toD2D(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

} // namespace

bool Renderer::init(HWND hwnd) {
    hwnd_ = hwnd;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()))) return false;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.put())))) {
        return false;
    }

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic_.put())))) {
        return false;
    }

    return createTarget();
}

void Renderer::shutdown() {
    sprites_.clear();
    sources_.clear();
    formats_.clear();
    brush_  = nullptr;
    target_ = nullptr;
    wic_    = nullptr;
    dwrite_ = nullptr;
    d2d_    = nullptr;
}

bool Renderer::createTarget() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const auto size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 1)),
                                  static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 1)));

    target_ = nullptr;
    brush_  = nullptr;
    sprites_.clear();

    if (FAILED(d2d_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                            D2D1::HwndRenderTargetProperties(hwnd_, size),
                                            target_.put()))) {
        return false;
    }

    // Il filtro lineare conta: lo zoom scala le immagini ben oltre 1:1 e il
    // nearest neighbour renderebbe visibili i pixel durante il crossfade.
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush_.put()))) {
        return false;
    }

    // Ricrea le bitmap dalle sorgenti WIC già decodificate.
    sprites_.resize(sources_.size());
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        target_->CreateBitmapFromWicBitmap(sources_[i].get(), nullptr, sprites_[i].put());
    }
    return true;
}

bool Renderer::loadSprites(const std::wstring& dir, int count) {
    sources_.clear();
    sources_.reserve(static_cast<std::size_t>(count));

    for (int i = 1; i <= count; ++i) {
        // Il WebP lo decodifica WIC, ma il codec è un componente separato:
        // presente d'ufficio su Windows 11, non garantito su 10. Si ripiega sul
        // PNG con lo stesso nome, così basta affiancare i file convertiti.
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        bool opened = false;
        for (const wchar_t* ext : {L".webp", L".png"}) {
            const std::wstring path = dir + L"\\" + std::to_wstring(i) + ext;
            decoder = nullptr;
            if (SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
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
        if (FAILED(wic_->CreateFormatConverter(converter.put()))) return false;
        if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut))) {
            return false;
        }
        sources_.push_back(converter);
    }

    sprites_.clear();
    sprites_.resize(sources_.size());
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        if (FAILED(target_->CreateBitmapFromWicBitmap(sources_[i].get(), nullptr,
                                                      sprites_[i].put()))) {
            return false;
        }
    }
    return true;
}

void Renderer::resize(UINT width, UINT height) {
    if (target_) target_->Resize(D2D1::SizeU(std::max<UINT>(width, 1), std::max<UINT>(height, 1)));
}

D2D1_SIZE_F Renderer::size() const {
    return target_ ? target_->GetSize() : D2D1::SizeF(0, 0);
}

void Renderer::begin(Color clear) {
    target_->BeginDraw();
    target_->Clear(toD2D(clear));
}

bool Renderer::end() {
    const HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        createTarget();
        return false;
    }
    return SUCCEEDED(hr);
}

void Renderer::drawSprite(int index, float scale, float opacity) {
    if (index < 0 || index >= static_cast<int>(sprites_.size())) return;
    if (!sprites_[static_cast<std::size_t>(index)]) return;
    if (opacity <= 0.004f) return;   // alpha ~0: puro overdraw, come nel JS

    ID2D1Bitmap* bmp = sprites_[static_cast<std::size_t>(index)].get();
    const D2D1_SIZE_F img = bmp->GetSize();
    const D2D1_SIZE_F win = target_->GetSize();
    if (img.width <= 0 || img.height <= 0) return;

    // Scala "cover": l'immagine copre sempre la finestra, come il layout web.
    const float cover = std::max(win.width / img.width, win.height / img.height);
    const float s     = cover * scale;

    const float w = img.width * s;
    const float h = img.height * s;
    const float x = (win.width - w) * 0.5f;
    const float y = (win.height - h) * 0.5f;

    bmp->GetSize();
    target_->DrawBitmap(bmp, D2D1::RectF(x, y, x + w, y + h), opacity,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Renderer::fillRect(D2D1_RECT_F rect, Color color, float radius) {
    brush_->SetColor(toD2D(color));
    if (radius > 0.0f) {
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.get());
    } else {
        target_->FillRectangle(rect, brush_.get());
    }
}

void Renderer::drawRectOutline(D2D1_RECT_F rect, Color color, float stroke, float radius) {
    brush_->SetColor(toD2D(color));
    if (radius > 0.0f) {
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.get(), stroke);
    } else {
        target_->DrawRectangle(rect, brush_.get(), stroke);
    }
}

IDWriteTextFormat* Renderer::formatFor(float size, bool bold, TextAlign align) {
    for (auto& f : formats_) {
        if (f.size == size && f.bold == bold && f.align == align) return f.format.get();
    }

    CachedFormat cf{size, bold, align, {}};
    if (FAILED(dwrite_->CreateTextFormat(
            L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"it-IT",
            cf.format.put()))) {
        return nullptr;
    }
    cf.format->SetTextAlignment(align == TextAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                          : DWRITE_TEXT_ALIGNMENT_LEADING);
    cf.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    formats_.push_back(cf);
    return formats_.back().format.get();
}

void Renderer::drawText(const std::wstring& text, D2D1_RECT_F box, float fontSize, Color color,
                        TextAlign align, bool bold) {
    IDWriteTextFormat* fmt = formatFor(fontSize, bold, align);
    if (!fmt) return;

    brush_->SetColor(toD2D(color));
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, box, brush_.get());
}

} // namespace mz::render
