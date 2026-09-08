// Backend Windows del renderer: Direct2D + DirectWrite + WIC, tutto dal Windows
// SDK e senza dipendenze esterne.
//
// Stessa interfaccia del backend macOS (render/renderer.hpp), stessa semantica,
// stesse primitive. Chi disegna non cambia una riga.
//
// L'ordine degli include conta. windows.h definisce DrawText come macro verso
// DrawTextW: se d2d1.h viene letto dopo, il metodo ID2D1RenderTarget::DrawText
// finisce dichiarato come DrawTextW e non è più chiamabile col suo nome. Quindi
// prima windows.h, poi si toglie la macro, poi i header Direct2D.
//
// NOTA SULLE SFOCATURE. Direct2D 1.0 - quello di ID2D1HwndRenderTarget, che è
// il target usato qui - non ha effetti: niente blur gaussiano, niente ombre.
// Averli vorrebbe dire ID2D1DeviceContext su swap chain DXGI, cioè riscrivere
// creazione del target, presentazione e device lost. Non serve: il backend
// macOS NON usa il blur di sistema nemmeno lui (l'unico disponibile in Core
// Graphics è quello delle ombre, e il trucco classico lì non funzionava), e
// costruisce il profilo gaussiano a gradini con forme concentriche. Qui si
// porta lo STESSO algoritmo, che è fatto di sole fillRect/drawRectOutline:
// stesso risultato, e il target resta quello semplice. Vedi fillRectBlurred,
// fillInnerGlow e fillRectShadow.
#include <windows.h>

#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <winrt/base.h>

#include "render/renderer.hpp"

#include <algorithm>
#include <cmath>
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

// Funzione di ripartizione della normale standard: quanta parte di un bordo
// sfocato con sigma 1 e' "coperta" alla distanza x (positiva verso l'interno).
// Gemella di quella in renderer_macos.mm: le due sfocature a gradini devono
// dare lo stesso profilo sulle due piattaforme, o il pannello esce diverso.
inline float gaussCdf(float x) noexcept {
    return 0.5f * (1.0f + std::erf(x / std::sqrt(2.0f)));
}

/** Le tre voci tipografiche, le stesse tre del backend macOS. */
enum class Role { Accent, Body, Display };

/**
 * Il primo nome di famiglia presente nel sistema, fra quelli proposti.
 *
 * Serve al font d'accento. Su macOS e' Iowan Old Style, un serif old-style
 * caldo che li' e' DI SISTEMA e qui non esiste: non e' impacchettabile (non ne
 * abbiamo il file ne' la licenza per ridistribuirlo) e va sostituito con il
 * piu' vicino fra quelli che un Windows ha gia'. Constantia per prima -
 * old-style, stesso asse umanista, presente da Vista; Georgia come rete di
 * sicurezza, perche' c'e' su qualunque Windows dal '98; poi Palatino e Cambria.
 *
 * NON VERIFICATO A SCHERMO: la scelta e' su base tipografica, non su un
 * provino affiancato come quello che aveva portato a Iowan. Va guardata su una
 * macchina Windows prima della mostra - e' l'unico punto del porto in cui il
 * risultato e' per forza diverso da macOS, non solo potenzialmente.
 */
std::wstring pickAccentFamily(IDWriteFactory* dwrite) {
    static const wchar_t* const candidates[] = {L"Constantia", L"Georgia",
                                                L"Palatino Linotype", L"Cambria"};
    if (dwrite) {
        winrt::com_ptr<IDWriteFontCollection> system;
        if (SUCCEEDED(dwrite->GetSystemFontCollection(system.put(), FALSE)) && system) {
            for (const wchar_t* name : candidates) {
                UINT32 index = 0;
                BOOL   found = FALSE;
                if (SUCCEEDED(system->FindFamilyName(name, &index, &found)) && found) {
                    return name;
                }
            }
        }
    }
    return L"Georgia";
}

/**
 * Collezione DirectWrite con dentro UN solo file di font, piu' il nome della
 * famiglia che contiene.
 *
 * I due font non di sistema (Manrope per il corpo, League Gothic per il
 * lettering) viaggiano accanto all'eseguibile e vanno resi visibili a
 * DirectWrite senza installarli. La strada moderna - IDWriteFactory5 e il font
 * set builder - evita di dover scrivere a mano un IDWriteFontCollectionLoader,
 * che era l'unico modo prima di Windows 10 1607 ed e' parecchio codice per
 * ottenere la stessa cosa.
 *
 * Il nome della famiglia si LEGGE dalla collezione invece di scriverlo qui:
 * se sia "Manrope" o "Manrope Variable" dipende da come e' stato costruito il
 * file, e indovinarlo sbagliato vuol dire ripiegare in silenzio sul font di
 * sistema - cioe' esattamente il difetto che non si nota finche' non e' in
 * mostra.
 */
struct FontFile {
    winrt::com_ptr<IDWriteFontCollection> collection;
    std::wstring                          family;

    bool valid() const noexcept { return collection && !family.empty(); }
};

FontFile loadFontFile(IDWriteFactory* dwrite, const std::wstring& path) {
    FontFile out;
    if (!dwrite) return out;

    winrt::com_ptr<IDWriteFactory5> f5;
    if (FAILED(dwrite->QueryInterface(__uuidof(IDWriteFactory5),
                                      reinterpret_cast<void**>(f5.put())))) {
        return out;   // Windows troppo vecchio: chi chiama ripiega sul font di sistema
    }

    winrt::com_ptr<IDWriteFontFile> file;
    if (FAILED(f5->CreateFontFileReference(path.c_str(), nullptr, file.put()))) return out;

    winrt::com_ptr<IDWriteFontSetBuilder1> builder;
    if (FAILED(f5->CreateFontSetBuilder(builder.put()))) return out;
    if (FAILED(builder->AddFontFile(file.get()))) return out;

    winrt::com_ptr<IDWriteFontSet> set;
    if (FAILED(builder->CreateFontSet(set.put()))) return out;

    winrt::com_ptr<IDWriteFontCollection1> collection1;
    if (FAILED(f5->CreateFontCollectionFromFontSet(set.get(), collection1.put()))) return out;
    if (collection1->GetFontFamilyCount() == 0) return out;

    winrt::com_ptr<IDWriteFontFamily> family;
    if (FAILED(collection1->GetFontFamily(0, family.put()))) return out;

    winrt::com_ptr<IDWriteLocalizedStrings> names;
    if (FAILED(family->GetFamilyNames(names.put()))) return out;
    if (names->GetCount() == 0) return out;

    UINT32 length = 0;
    if (FAILED(names->GetStringLength(0, &length))) return out;
    std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
    if (FAILED(names->GetString(0, name.data(), length + 1))) return out;
    name.resize(length);

    out.collection = collection1.try_as<IDWriteFontCollection>();
    out.family     = std::move(name);
    return out;
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

    std::wstring accentFamily;   // serif di sistema, scelto in init()
    FontFile     body;           // Manrope, dal file accanto all'eseguibile
    FontFile     display;        // League Gothic, idem

    struct CachedFormat {
        Role                              role;
        float                             size;
        bool                              bold;
        TextAlign                         align;
        winrt::com_ptr<IDWriteTextFormat> format;
    };
    std::vector<CachedFormat> formats;

    IDWriteTextFormat* formatFor(Role role, float size, bool bold, TextAlign align);
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

    d.accentFamily = pickAccentFamily(d.dwrite.get());

    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(d.wic.put())));
}

void GraphicsCore::shutdown() {
    auto& d = *impl_;
    d.formats.clear();
    d.sources.clear();
    d.body    = {};
    d.display = {};
    d.wic     = nullptr;
    d.dwrite  = nullptr;
    d.d2d     = nullptr;
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

bool GraphicsCore::loadFonts(const std::wstring& dir) {
    // Due font non di sistema viaggiano accanto all'eseguibile, in <exe>\fonts
    // (ce li mette il CMakeLists), e qui si rendono visibili a DirectWrite per
    // il solo processo: Manrope per il corpo del testo e League Gothic per il
    // lettering delle intestazioni. L'accento e' un serif di sistema e non
    // passa di qui. Il valore di ritorno non e' fatale: senza collezione i
    // formati ripiegano sul font di sistema, esattamente come su macOS.
    auto& d = *impl_;
    d.formats.clear();   // i formati gia' costruiti puntano alle collezioni vecchie

    d.body    = loadFontFile(d.dwrite.get(), dir + L"\\Manrope-Variable.ttf");
    d.display = loadFontFile(d.dwrite.get(), dir + L"\\LeagueGothic-Variable.ttf");
    return d.body.valid() && d.display.valid();
}

IDWriteTextFormat* GraphicsCore::Impl::formatFor(Role role, float size, bool bold,
                                                 TextAlign align) {
    for (auto& f : formats) {
        if (f.role == role && f.size == size && f.bold == bold && f.align == align) {
            return f.format.get();
        }
    }

    // Famiglia, collezione e selettori peso/larghezza per ciascuna delle tre
    // voci. I pesi ricalcano quelli chiesti a CoreText nel backend macOS.
    const wchar_t*        family     = accentFamily.c_str();
    IDWriteFontCollection* collection = nullptr;
    DWRITE_FONT_WEIGHT     weight     = bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                             : DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STRETCH    stretch    = DWRITE_FONT_STRETCH_NORMAL;

    if (role == Role::Body && body.valid()) {
        family     = body.family.c_str();
        collection = body.collection.get();
        // Manrope e' a variazione sul solo asse 'wght' e la sua istanza di
        // default e' la Light: il peso va chiesto esplicitamente, o il corpo
        // del testo esce troppo chiaro. 400 e 600, come su macOS - il font set
        // builder espande le istanze con nome, quindi il peso qui basta a
        // selezionare quella giusta senza toccare le API degli assi.
        weight = bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    } else if (role == Role::Display && display.valid()) {
        family     = display.family.c_str();
        collection = display.collection.get();
        weight     = DWRITE_FONT_WEIGHT_NORMAL;
        // League Gothic e' a variazione sul solo asse 'wdth' (75..100), e la
        // larghezza 75 e' quella che replica le proporzioni del poster.
        // DWRITE_FONT_STRETCH_CONDENSED E' il 75%: la scala di usWidthClass
        // che DirectWrite usa per lo stretch e la scala dell'asse 'wdth' sono
        // la stessa cosa, quindi qui non serve nessuna API sugli assi.
        stretch = DWRITE_FONT_STRETCH_CONDENSED;
    }

    CachedFormat cf{role, size, bold, align, {}};
    if (FAILED(dwrite->CreateTextFormat(family, collection, weight, DWRITE_FONT_STYLE_NORMAL,
                                        stretch, size, L"it-IT", cf.format.put()))) {
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
    winrt::com_ptr<ID2D1StrokeStyle>         roundStroke;
    std::vector<winrt::com_ptr<ID2D1Bitmap>> sprites;

    // popClip() e' uno solo ma i ritagli sono di due specie: rettangolare
    // (PushAxisAlignedClip, economico) e a forma libera (PushLayer, per la
    // macchia e per il campo di metaball). Direct2D vuole che si chiudano con
    // la chiamata giusta e nell'ordine inverso, quindi qui si tiene traccia di
    // cosa si e' aperto. Senza questa pila, un pushClipCubicPath chiuso da
    // PopAxisAlignedClip fa saltare l'intero EndDraw del fotogramma.
    std::vector<unsigned char> clips;   // 0 = axis aligned, 1 = layer

    bool createTarget();
    bool upload();

    /** Path chiusa dalle curve cubiche di fillCubicPath/pushClipCubicPath. */
    winrt::com_ptr<ID2D1PathGeometry> cubicGeometry(const Point* pts, int count) const;
    /** Path dai poligoni di fillPolygons/pushClipPolygons, riempimento non-zero. */
    winrt::com_ptr<ID2D1PathGeometry> polygonGeometry(const Point* pts, const int* lengths,
                                                      int shapes) const;
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
    d.clips.clear();
    d.roundStroke = nullptr;
    d.brush       = nullptr;
    d.target      = nullptr;
    d.core        = nullptr;
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
    clips.clear();

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

    // Estremi e giunzioni tonde: le usa drawArc (indicatori di progresso) e la
    // spezzata dei grafici, dove uno spigolo vivo si nota.
    roundStroke = nullptr;
    core->impl_->d2d->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
        nullptr, 0, roundStroke.put());

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
    impl_->clips.clear();
    impl_->target->BeginDraw();
    impl_->target->Clear(toD2D(clear));
}

bool Renderer::end() {
    // Un ritaglio lasciato aperto (un return anticipato fra push e pop) farebbe
    // fallire EndDraw e sparire l'intero fotogramma. Si chiude qui quello che e'
    // rimasto: meglio un fotogramma con un ritaglio di troppo che nessuno.
    auto& d = *impl_;
    while (!d.clips.empty()) {
        if (d.clips.back() == 1) d.target->PopLayer();
        else                     d.target->PopAxisAlignedClip();
        d.clips.pop_back();
    }

    const HRESULT hr = d.target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        d.createTarget();
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

// ---------------------------------------------------------------------------
// Geometrie di servizio
// ---------------------------------------------------------------------------

winrt::com_ptr<ID2D1PathGeometry> Renderer::Impl::cubicGeometry(const Point* pts,
                                                                int count) const {
    winrt::com_ptr<ID2D1PathGeometry> geometry;
    if (!pts || count < 4 || !core || !core->impl_->d2d) return geometry;
    if (FAILED(core->impl_->d2d->CreatePathGeometry(geometry.put()))) return {};

    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put()))) return {};

    sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i + 2 < count; i += 3) {
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(pts[i].x, pts[i].y),
                                            D2D1::Point2F(pts[i + 1].x, pts[i + 1].y),
                                            D2D1::Point2F(pts[i + 2].x, pts[i + 2].y)));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return {};
    return geometry;
}

winrt::com_ptr<ID2D1PathGeometry> Renderer::Impl::polygonGeometry(const Point* pts,
                                                                   const int* lengths,
                                                                   int shapes) const {
    winrt::com_ptr<ID2D1PathGeometry> geometry;
    if (!pts || !lengths || shapes <= 0 || !core || !core->impl_->d2d) return geometry;
    if (FAILED(core->impl_->d2d->CreatePathGeometry(geometry.put()))) return {};

    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put()))) return {};

    // Non-zero e non even-odd: i contorni sono tutti nello stesso verso, e i
    // bordi in comune fra due pezzi adiacenti devono sparire, non bucare.
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    int  base  = 0;
    bool empty = true;
    for (int s = 0; s < shapes; ++s) {
        const int n = lengths[s];
        if (n >= 3) {
            sink->BeginFigure(D2D1::Point2F(pts[base].x, pts[base].y), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < n; ++i) {
                sink->AddLine(D2D1::Point2F(pts[base + i].x, pts[base + i].y));
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            empty = false;
        }
        base += n;
    }
    if (FAILED(sink->Close()) || empty) return {};
    return geometry;
}

// ---------------------------------------------------------------------------
// Primitive piene
// ---------------------------------------------------------------------------

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
    //
    // L'antialiasing invece si spegne anche qui, ed e' la parte che conta. A un
    // pixel di lato non c'e' nessuno spigolo da arrotondare, ma ogni
    // rettangolo passa dal percorso a copertura parziale: sul backend macOS il
    // profiler dava tre quarti del costo dell'intera schermata d'ingresso
    // proprio li', e la schermata d'ingresso ne disegna decine di migliaia per
    // fotogramma.
    const D2D1_ANTIALIAS_MODE previous = d.target->GetAntialiasMode();
    d.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    d.brush->SetColor(toD2D(color));
    for (int i = 0; i < count; ++i) d.target->FillRectangle(toD2D(rects[i]), d.brush.get());
    d.target->SetAntialiasMode(previous);
}

void Renderer::fillCircle(Point center, float radius, Color color) {
    auto& d = *impl_;
    if (radius <= 0.0f) return;
    d.brush->SetColor(toD2D(color));
    d.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y), radius, radius),
                          d.brush.get());
}

void Renderer::fillCubicPath(const Point* pts, int count, Color color) {
    auto& d = *impl_;
    auto geometry = d.cubicGeometry(pts, count);
    if (!geometry) return;
    d.brush->SetColor(toD2D(color));
    d.target->FillGeometry(geometry.get(), d.brush.get());
}

void Renderer::fillPolygons(const Point* pts, const int* lengths, int shapes, Color color) {
    auto& d = *impl_;
    auto geometry = d.polygonGeometry(pts, lengths, shapes);
    if (!geometry) return;
    d.brush->SetColor(toD2D(color));
    d.target->FillGeometry(geometry.get(), d.brush.get());
}

// ---------------------------------------------------------------------------
// Sfumature
// ---------------------------------------------------------------------------

namespace {

/** Pennello a sfumatura lineare; nullptr se non si riesce a costruirlo. */
winrt::com_ptr<ID2D1LinearGradientBrush> linearBrush(ID2D1RenderTarget* target,
                                                     const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                     D2D1_POINT_2F from, D2D1_POINT_2F to) {
    winrt::com_ptr<ID2D1LinearGradientBrush> out;
    if (!target || !stops || count < 2) return out;

    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    if (FAILED(target->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                    D2D1_EXTEND_MODE_CLAMP, collection.put()))) {
        return out;
    }
    target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                      collection.get(), out.put());
    return out;
}

} // namespace

void Renderer::fillRectGradient(Rect r, Color top, Color bottom, float radius) {
    auto& d = *impl_;

    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, toD2D(top)},
        {1.0f, toD2D(bottom)},
    };

    // Leggermente diagonale (10 gradi dalla verticale, verso destra scendendo)
    // invece di puramente verticale: da' profondita' senza essere appariscente.
    // La linea passa per il centro del rettangolo, cosi' l'inclinazione si
    // vede allo stesso modo qualunque sia la larghezza.
    constexpr float kTiltDeg = 10.0f;
    const float cx    = (r.left + r.right) * 0.5f;
    const float halfH = (r.bottom - r.top) * 0.5f;
    const float dx    = halfH * std::tan(kTiltDeg * 3.14159265358979f / 180.0f);

    auto brush = linearBrush(d.target.get(), stops, 2, D2D1::Point2F(cx - dx, r.top),
                             D2D1::Point2F(cx + dx, r.bottom));
    if (!brush) return;

    if (radius > 0.0f) {
        d.target->FillRoundedRectangle(D2D1::RoundedRect(toD2D(r), radius, radius), brush.get());
    } else {
        d.target->FillRectangle(toD2D(r), brush.get());
    }
}

void Renderer::fillCircleGradient(Point center, float radius, Color inner, Color mid,
                                  Color outer) {
    auto& d = *impl_;
    if (radius <= 0.0f) return;

    const D2D1_GRADIENT_STOP stops[3] = {
        {0.0f,  toD2D(inner)},
        {0.55f, toD2D(mid)},
        {1.0f,  toD2D(outer)},
    };

    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    if (FAILED(d.target->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2,
                                                      D2D1_EXTEND_MODE_CLAMP,
                                                      collection.put()))) {
        return;
    }

    winrt::com_ptr<ID2D1RadialGradientBrush> brush;
    const auto c = D2D1::Point2F(center.x, center.y);
    if (FAILED(d.target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(0, 0), radius, radius),
            collection.get(), brush.put()))) {
        return;
    }
    d.target->FillEllipse(D2D1::Ellipse(c, radius, radius), brush.get());
}

void Renderer::drawRectOutlineGradient(Rect r, const Color* stops, const float* positions,
                                       int count, Point from, Point to, float stroke,
                                       float radius) {
    auto& d = *impl_;
    if (!stops || !positions || count < 2) return;

    std::vector<D2D1_GRADIENT_STOP> gs(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        gs[static_cast<std::size_t>(i)].position = positions[i];
        gs[static_cast<std::size_t>(i)].color    = toD2D(stops[i]);
    }

    // Su macOS il contorno si ottiene ritagliando sulla path ingrossata e poi
    // versandoci dentro la sfumatura. Qui non serve: in Direct2D il pennello
    // vale anche per il TRATTO, quindi si disegna il contorno direttamente col
    // pennello a sfumatura. Il modo CLAMP riproduce
    // kCGGradientDrawsBefore/AfterStartLocation: oltre gli estremi la
    // sfumatura si prolunga col colore di bordo invece di sparire.
    auto brush = linearBrush(d.target.get(), gs.data(), static_cast<UINT32>(count),
                             D2D1::Point2F(from.x, from.y), D2D1::Point2F(to.x, to.y));
    if (!brush) return;

    if (radius > 0.0f) {
        d.target->DrawRoundedRectangle(D2D1::RoundedRect(toD2D(r), radius, radius), brush.get(),
                                       stroke);
    } else {
        d.target->DrawRectangle(toD2D(r), brush.get(), stroke);
    }
}

// ---------------------------------------------------------------------------
// Le tre primitive "sfocate"
//
// Nessuna delle tre usa un blur vero: Direct2D 1.0 non ne ha, e il backend
// macOS non lo usa comunque. Si costruisce il profilo gaussiano a gradini con
// forme concentriche, e per sigma di qualche pixel i gradini non si vedono.
// ---------------------------------------------------------------------------

void Renderer::fillRectBlurred(Rect r, Color color, float radius, float sigma) {
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

void Renderer::fillRectShadow(Rect r, Color color, float radius, Color shadowColor,
                              float shadowBlur, Point shadowOffset) {
    // Core Graphics ha CGContextSetShadowWithColor e il backend macOS lo usa;
    // Direct2D 1.0 non ha niente di equivalente. L'ombra si disegna quindi a
    // mano - la stessa forma, spostata, sfocata col profilo a gradini di
    // fillRectBlurred - e poi la forma nitida sopra.
    //
    // Il parametro `blur` di Core Graphics e' un raggio, non una sigma: il
    // rapporto usuale e' sigma ~ blur/2, ed e' quello adottato qui. E' una
    // corrispondenza NUMERICA, non verificata affiancando i due schermi:
    // e' la prima cosa da guardare se l'ombra su Windows esce piu' dura o piu'
    // molle di quella del Mac.
    if (shadowColor.a > 0.0f && shadowBlur > 0.0f) {
        const Rect shifted = rect(r.left + shadowOffset.x, r.top + shadowOffset.y,
                                  r.right + shadowOffset.x, r.bottom + shadowOffset.y);
        fillRectBlurred(shifted, shadowColor, radius, shadowBlur * 0.5f);
    }
    fillRect(r, color, radius);
}

void Renderer::fillInnerGlow(Rect r, Color color, float radius, float sigma, float spread) {
    auto& d = *impl_;
    if (sigma <= 0.05f) return;

    // Il bagliore va tenuto dentro la forma: le fasce piu' esterne, con il
    // raggio ridotto, sborderebbero agli angoli.
    d.target->PushAxisAlignedClip(toD2D(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

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
        if (band.width() <= 0.0f || band.height() <= 0.0f) break;
        drawRectOutline(band, {color.r, color.g, color.b, color.a * alpha}, w,
                        std::max(0.0f, radius - depth));
    }

    d.target->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// Contorni
// ---------------------------------------------------------------------------

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
    // Giunzioni tonde come su macOS (kCGLineJoinRound): sulle tracce dell'EEG
    // gli spigoli vivi fanno le punte.
    d.target->DrawGeometry(geometry.get(), d.brush.get(), stroke, d.roundStroke.get());
}

void Renderer::drawArc(Point center, float radius, float startDeg, float endDeg, float thickness,
                       Color color) {
    auto& d = *impl_;
    if (radius <= 0.0f || !d.core || !d.core->impl_->d2d) return;

    // Punto per punto invece di AddArc: cosi' l'angolo si legge nella stessa
    // convenzione (y verso il basso, gradi crescenti in senso orario) di tutto
    // il resto, senza doversi fidare di come D2D interpreta il verso di
    // spazzata. E' la stessa scelta del backend macOS, per lo stesso motivo.
    const double startRad = startDeg * 3.14159265358979 / 180.0;
    const double endRad   = endDeg * 3.14159265358979 / 180.0;
    const int    segments = std::max(2, static_cast<int>(std::abs(endDeg - startDeg) / 3.0) + 1);

    winrt::com_ptr<ID2D1PathGeometry> geometry;
    if (FAILED(d.core->impl_->d2d->CreatePathGeometry(geometry.put()))) return;

    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put()))) return;

    for (int i = 0; i <= segments; ++i) {
        const double t  = startRad + (endRad - startRad) * (static_cast<double>(i) / segments);
        const float  px = center.x + radius * static_cast<float>(std::cos(t));
        const float  py = center.y + radius * static_cast<float>(std::sin(t));
        if (i == 0) sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_HOLLOW);
        else        sink->AddLine(D2D1::Point2F(px, py));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;

    d.brush->SetColor(toD2D(color));
    d.target->DrawGeometry(geometry.get(), d.brush.get(), thickness, d.roundStroke.get());
}

// ---------------------------------------------------------------------------
// Ritagli
// ---------------------------------------------------------------------------

void Renderer::pushClip(Rect r) {
    auto& d = *impl_;
    d.target->PushAxisAlignedClip(toD2D(r), D2D1_ANTIALIAS_MODE_ALIASED);
    d.clips.push_back(0);
}

void Renderer::popClip() {
    auto& d = *impl_;
    if (d.clips.empty()) return;   // pop spaiato: meglio ignorarlo che rompere EndDraw
    if (d.clips.back() == 1) d.target->PopLayer();
    else                     d.target->PopAxisAlignedClip();
    d.clips.pop_back();
}

namespace {

/**
 * Ritaglio a forma libera. Il layer senza ID2D1Layer esplicito (nullptr) esiste
 * da Windows 8: e' Direct2D a gestirsi la superficie temporanea, e non c'e'
 * niente da tenere vivo fra un fotogramma e l'altro.
 */
void pushGeometryLayer(ID2D1RenderTarget* target, ID2D1Geometry* geometry,
                       std::vector<unsigned char>& clips) {
    if (!target) return;
    auto params            = D2D1::LayerParameters(D2D1::InfiniteRect(), geometry);
    params.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    target->PushLayer(params, nullptr);
    clips.push_back(1);
}

} // namespace

void Renderer::pushClipCubicPath(const Point* pts, int count) {
    auto& d = *impl_;
    // Sempre in coppia con popClip, anche se la path non e' valida: chi chiama
    // non deve contare i casi. Con geometria nulla il layer non ritaglia
    // niente, che e' il comportamento meno dannoso.
    auto geometry = d.cubicGeometry(pts, count);
    pushGeometryLayer(d.target.get(), geometry.get(), d.clips);
}

void Renderer::pushClipPolygons(const Point* pts, const int* lengths, int shapes) {
    auto& d = *impl_;
    auto geometry = d.polygonGeometry(pts, lengths, shapes);
    pushGeometryLayer(d.target.get(), geometry.get(), d.clips);
}

// ---------------------------------------------------------------------------
// Testo
// ---------------------------------------------------------------------------

namespace {

/**
 * Layout di UNA riga, con la spaziatura fra lettere gia' applicata.
 *
 * Il "tracking" di CoreText (kCTKernAttributeName, in punti) qui e' lo spazio
 * di coda di IDWriteTextLayout1::SetCharacterSpacing: stessa cosa, spazio
 * aggiunto dopo ogni glifo, e accetta valori negativi - che e' quello che
 * serve, visto che il lettering del poster tiene le lettere quasi a toccarsi.
 * minAdvanceWidth resta 0, altrimenti il valore negativo verrebbe limitato.
 */
winrt::com_ptr<IDWriteTextLayout> singleLine(IDWriteFactory* dwrite, const std::wstring& text,
                                             IDWriteTextFormat* format, float tracking) {
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!dwrite || !format || text.empty()) return layout;

    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                        100000.0f, 100000.0f, layout.put()))) {
        return {};
    }
    if (tracking != 0.0f) {
        if (auto layout1 = layout.try_as<IDWriteTextLayout1>()) {
            const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
            layout1->SetCharacterSpacing(0.0f, tracking, 0.0f, all);
        }
    }
    return layout;
}

/**
 * Disegna `layout` col suo centro tipografico - non il riquadro che lo
 * contiene - sul punto dato.
 *
 * E' la stessa geometria di drawCenteredText nel backend macOS, che porta
 * l'origine sulla BASELINE e la sposta di (ascesa - discesa)/2 sotto il
 * centro. Qui DrawTextLayout ancora al vertice in alto a sinistra, quindi si
 * risale: cima = baseline - ascesa, con ascesa e discesa prese dalle metriche
 * della prima riga. Ancorare al riquadro invece che alla baseline sposta il
 * testo di qualche pixel verso il basso, e su una cifra dentro un pallino o
 * sull'etichetta di un pulsante si vede.
 */
void drawLayoutCentered(ID2D1RenderTarget* target, IDWriteTextLayout* layout, Point center,
                        ID2D1SolidColorBrush* brush, Color color) {
    if (!target || !layout || !brush) return;

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;

    DWRITE_LINE_METRICS line{};
    UINT32              lines = 0;
    if (FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines == 0) return;

    const float ascent  = line.baseline;
    const float descent = std::max(0.0f, line.height - line.baseline);
    const float top     = center.y + (ascent - descent) * 0.5f - ascent;
    const float left    = center.x - metrics.width * 0.5f;

    brush->SetColor(toD2D(color));
    target->DrawTextLayout(D2D1::Point2F(left, top), layout, brush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
}

} // namespace

void Renderer::drawText(const std::wstring& text, Rect box, float fontSize, Color color,
                        TextAlign align, bool bold) {
    auto& d = *impl_;
    IDWriteTextFormat* fmt =
        d.core ? d.core->impl_->formatFor(Role::Accent, fontSize, bold, align) : nullptr;
    if (!fmt) return;

    d.brush->SetColor(toD2D(color));
    d.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, toD2D(box),
                       d.brush.get());
}

void Renderer::drawTextBody(const std::wstring& text, Rect box, float fontSize, Color color,
                            TextAlign align, bool bold) {
    auto& d = *impl_;
    IDWriteTextFormat* fmt =
        d.core ? d.core->impl_->formatFor(Role::Body, fontSize, bold, align) : nullptr;
    if (!fmt) return;

    d.brush->SetColor(toD2D(color));
    d.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, toD2D(box),
                       d.brush.get());
}

void Renderer::drawTextCentered(const std::wstring& text, Point center, float fontSize,
                                Color color, bool bold) {
    auto& d = *impl_;
    if (!d.core) return;
    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Accent, fontSize, bold,
                                                      TextAlign::Left);
    auto layout = singleLine(d.core->impl_->dwrite.get(), text, fmt, 0.0f);
    drawLayoutCentered(d.target.get(), layout.get(), center, d.brush.get(), color);
}

void Renderer::drawTextBodyCentered(const std::wstring& text, Point center, float fontSize,
                                    Color color, bool bold) {
    auto& d = *impl_;
    if (!d.core) return;
    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Body, fontSize, bold,
                                                      TextAlign::Left);
    auto layout = singleLine(d.core->impl_->dwrite.get(), text, fmt, 0.0f);
    drawLayoutCentered(d.target.get(), layout.get(), center, d.brush.get(), color);
}

Size Renderer::measureTextBody(const std::wstring& text, float fontSize, bool bold,
                               float maxWidth) const {
    auto& d = *impl_;
    if (text.empty() || !d.core) return {0.0f, 0.0f};

    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Body, fontSize, bold,
                                                      TextAlign::Left);
    if (!fmt) return {0.0f, 0.0f};

    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(d.core->impl_->dwrite->CreateTextLayout(text.c_str(),
                                                       static_cast<UINT32>(text.size()), fmt,
                                                       maxWidth, 100000.0f, layout.put()))) {
        return {0.0f, 0.0f};
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return {0.0f, 0.0f};

    // Arrotondato per eccesso: una misura che sottostima di mezzo pixel fa
    // sparire l'ultimo glifo quando il risultato viene usato come larghezza
    // del riquadro in cui poi si disegna.
    return {std::ceil(metrics.width), std::ceil(metrics.height)};
}

void Renderer::drawParagraph(const std::wstring& text, Rect box, float fontSize, Color color,
                             TextAlign align, float lineHeight) {
    auto& d = *impl_;
    if (text.empty() || !d.core) return;

    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Body, fontSize, false, align);
    if (!fmt) return;

    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(d.core->impl_->dwrite->CreateTextLayout(text.c_str(),
                                                       static_cast<UINT32>(text.size()), fmt,
                                                       box.width(), box.height(),
                                                       layout.put()))) {
        return;
    }

    // Altezza di riga FISSA in punti, non un moltiplicatore del leading
    // naturale del font: "interlinea 1.45" deve voler dire 1.45 volte il
    // corpo, che e' come si ragiona impaginando. E' lo stesso motivo per cui
    // su macOS si fissano minimo e massimo allo stesso valore invece di usare
    // LineHeightMultiple. La baseline a 0.8 dell'altezza di riga e' la
    // proporzione usuale per un sans: sposta il blocco dentro la riga, non la
    // distanza fra le righe.
    const float spacing = fontSize * lineHeight;
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spacing, spacing * 0.8f);

    d.brush->SetColor(toD2D(color));
    d.target->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.get(), d.brush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Renderer::drawTextDisplay(const std::wstring& text, Rect box, float fontSize, Color color,
                               TextAlign align, float tracking) {
    auto& d = *impl_;
    if (text.empty() || !d.core) return;

    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Display, fontSize, false, align);
    if (!fmt) return;

    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(d.core->impl_->dwrite->CreateTextLayout(text.c_str(),
                                                       static_cast<UINT32>(text.size()), fmt,
                                                       box.width(), box.height(),
                                                       layout.put()))) {
        return;
    }
    if (tracking != 0.0f) {
        if (auto layout1 = layout.try_as<IDWriteTextLayout1>()) {
            const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
            layout1->SetCharacterSpacing(0.0f, tracking, 0.0f, all);
        }
    }

    d.brush->SetColor(toD2D(color));
    d.target->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.get(), d.brush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Renderer::drawTextDisplayCentered(const std::wstring& text, Point center, float fontSize,
                                       Color color, float tracking) {
    auto& d = *impl_;
    if (!d.core) return;
    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Display, fontSize, false,
                                                      TextAlign::Left);
    auto layout = singleLine(d.core->impl_->dwrite.get(), text, fmt, tracking);
    drawLayoutCentered(d.target.get(), layout.get(), center, d.brush.get(), color);
}

Size Renderer::measureTextDisplay(const std::wstring& text, float fontSize,
                                  float tracking) const {
    auto& d = *impl_;
    if (text.empty() || !d.core) return {0.0f, 0.0f};

    IDWriteTextFormat* fmt = d.core->impl_->formatFor(Role::Display, fontSize, false,
                                                      TextAlign::Left);
    auto layout = singleLine(d.core->impl_->dwrite.get(), text, fmt, tracking);
    if (!layout) return {0.0f, 0.0f};

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return {0.0f, 0.0f};

    DWRITE_LINE_METRICS line{};
    UINT32              lines = 0;
    const float height = (SUCCEEDED(layout->GetLineMetrics(&line, 1, &lines)) && lines > 0)
                             ? line.height
                             : metrics.height;

    return {std::ceil(metrics.width), std::ceil(height)};
}

} // namespace mz::render
