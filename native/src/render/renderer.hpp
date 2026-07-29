#pragma once

// Rendering con Direct2D + DirectWrite + WIC: tutto dal Windows SDK, nessuna
// dipendenza esterna. Il carico grafico è due quad texturati in crossfade, per
// cui D2D è la scelta giusta: fa scala e alpha in hardware senza il peso di uno
// stack GL/engine completo.

// L'ordine conta. windows.h definisce DrawText come macro verso DrawTextW: se
// d2d1.h viene letto dopo, il metodo ID2D1RenderTarget::DrawText finisce
// dichiarato come DrawTextW e non è più chiamabile col suo nome. Quindi prima
// windows.h, poi si toglie la macro, poi i header Direct2D.
#include <windows.h>

#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <string>
#include <vector>

namespace mz::render {

enum class TextAlign { Left, Center };

/** Colore in forma comoda (componenti 0..1). */
struct Color {
    float r, g, b, a;
};

class Renderer {
public:
    bool init(HWND hwnd);
    void shutdown();

    /** Carica <dir>/1.webp .. <dir>/N.webp. WIC decodifica il WebP nativamente. */
    bool loadSprites(const std::wstring& dir, int count);

    void resize(UINT width, UINT height);

    void begin(Color clear);
    /** @return false se il target va ricreato (device lost). */
    bool end();

    /**
     * Disegna lo sprite scalato attorno al centro della finestra.
     * @param scale moltiplicatore sopra la scala "cover" che riempie la finestra
     */
    void drawSprite(int index, float scale, float opacity);

    void fillRect(D2D1_RECT_F rect, Color color, float radius = 0.0f);
    void drawRectOutline(D2D1_RECT_F rect, Color color, float stroke, float radius = 0.0f);
    void drawText(const std::wstring& text, D2D1_RECT_F box, float fontSize,
                  Color color, TextAlign align = TextAlign::Center, bool bold = false);

    D2D1_SIZE_F size() const;
    int         spriteCount() const noexcept { return static_cast<int>(sprites_.size()); }
    bool        ready() const noexcept { return static_cast<bool>(target_); }

private:
    bool createTarget();
    IDWriteTextFormat* formatFor(float size, bool bold, TextAlign align);

    HWND hwnd_ = nullptr;

    winrt::com_ptr<ID2D1Factory>          d2d_;
    winrt::com_ptr<IDWriteFactory>        dwrite_;
    winrt::com_ptr<IWICImagingFactory>    wic_;
    winrt::com_ptr<ID2D1HwndRenderTarget> target_;
    winrt::com_ptr<ID2D1SolidColorBrush>  brush_;

    // Le sorgenti WIC restano vive: al device lost le bitmap D2D vanno ricreate.
    std::vector<winrt::com_ptr<IWICFormatConverter>> sources_;
    std::vector<winrt::com_ptr<ID2D1Bitmap>>         sprites_;

    struct CachedFormat {
        float                            size;
        bool                             bold;
        TextAlign                        align;
        winrt::com_ptr<IDWriteTextFormat> format;
    };
    std::vector<CachedFormat> formats_;
};

} // namespace mz::render
