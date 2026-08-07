#pragma once

// Rendering con Direct2D + DirectWrite + WIC: tutto dal Windows SDK, nessuna
// dipendenza esterna. Il carico grafico è due quad texturati in crossfade, per
// cui D2D è la scelta giusta: fa scala e alpha in hardware senza il peso di uno
// stack GL/engine completo.
//
// Due livelli, perché le finestre sono due (operatore e proiezione):
//   GraphicsCore  factory e immagini DECODIFICATE, una volta sola per processo
//   Renderer      risorse legate al device: una per finestra
// La divisione era già preparata: le sorgenti WIC stavano separate dalle bitmap
// D2D perché al device lost le seconde vanno ricreate. Sono le stesse due
// categorie, qui portate a due tipi distinti.

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

/** Punto in coordinate di finestra. Serve alle spezzate dei grafici. */
struct Point {
    float x, y;
};

/**
 * Risorse indipendenti dal device: factory, immagini decodificate e formati di
 * testo. Ne esiste una sola, condivisa da tutte le finestre — decodificare due
 * volte gli stessi dodici JPEG sarebbe puro spreco.
 */
class GraphicsCore {
public:
    bool init();
    void shutdown();

    /** Decodifica <dir>/1.jpg .. <dir>/N.jpg (o .webp/.png). Una volta sola. */
    bool loadSprites(const std::wstring& dir, int count);

    ID2D1Factory*   d2d() const noexcept { return d2d_.get(); }
    IWICImagingFactory* wic() const noexcept { return wic_.get(); }

    std::size_t spriteCount() const noexcept { return sources_.size(); }
    IWICFormatConverter* source(std::size_t i) const {
        return (i < sources_.size()) ? sources_[i].get() : nullptr;
    }

    /** Formato di testo, creato una volta e riusato. Non dipende dal device. */
    IDWriteTextFormat* formatFor(float size, bool bold, TextAlign align);

private:
    winrt::com_ptr<ID2D1Factory>       d2d_;
    winrt::com_ptr<IDWriteFactory>     dwrite_;
    winrt::com_ptr<IWICImagingFactory> wic_;

    std::vector<winrt::com_ptr<IWICFormatConverter>> sources_;

    struct CachedFormat {
        float                             size;
        bool                              bold;
        TextAlign                         align;
        winrt::com_ptr<IDWriteTextFormat> format;
    };
    std::vector<CachedFormat> formats_;
};

/**
 * Superficie di disegno di UNA finestra. Le bitmap D2D sono risorse di device,
 * quindi ogni finestra ha le sue: è inevitabile, non una duplicazione evitata
 * per distrazione.
 */
class Renderer {
public:
    bool init(GraphicsCore& core, HWND hwnd);
    void shutdown();

    /** Crea le bitmap di questo target dalle sorgenti già decodificate nel core. */
    bool uploadSprites();

    void resize(UINT width, UINT height);

    void begin(Color clear);
    /** @return false se il target va ricreato (device lost). */
    bool end();

    /**
     * Disegna lo sprite scalato attorno al centro della finestra.
     * @param scale moltiplicatore sopra la scala "cover" che riempie la finestra
     */
    void drawSprite(int index, float scale, float opacity);

    /** Come drawSprite ma dentro un rettangolo dato: serve alla miniatura. */
    void drawSpriteIn(int index, D2D1_RECT_F box, float scale, float opacity);

    void fillRect(D2D1_RECT_F rect, Color color, float radius = 0.0f);
    void drawRectOutline(D2D1_RECT_F rect, Color color, float stroke, float radius = 0.0f);
    void drawText(const std::wstring& text, D2D1_RECT_F box, float fontSize,
                  Color color, TextAlign align = TextAlign::Center, bool bold = false);

    void drawLine(Point a, Point b, Color color, float stroke = 1.0f);
    /** Spezzata: una geometria per chiamata, trascurabile a qualche centinaio di punti. */
    void drawPolyline(const Point* pts, std::size_t count, Color color, float stroke = 1.5f);

    /** Ritaglio rettangolare, per tenere le tracce dentro il loro riquadro. */
    void pushClip(D2D1_RECT_F rect);
    void popClip();

    D2D1_SIZE_F size() const;
    bool        ready() const noexcept { return static_cast<bool>(target_); }

private:
    bool createTarget();

    GraphicsCore* core_ = nullptr;
    HWND          hwnd_ = nullptr;

    winrt::com_ptr<ID2D1HwndRenderTarget> target_;
    winrt::com_ptr<ID2D1SolidColorBrush>  brush_;
    std::vector<winrt::com_ptr<ID2D1Bitmap>> sprites_;
};

} // namespace mz::render
