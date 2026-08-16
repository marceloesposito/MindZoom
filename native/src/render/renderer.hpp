#pragma once

// Superficie di disegno, in tipi che NON appartengono a nessuna piattaforma.
//
// Il carico grafico è due quad texturati in crossfade più qualche primitiva 2D:
// abbastanza poco da poter essere espresso in un'interfaccia piccola, e questa è
// quell'interfaccia. Dietro ci sta Direct2D su Windows e Core Graphics su macOS;
// chi disegna non lo sa e non deve saperlo.
//
// Prima i tipi Direct2D (D2D1_RECT_F, HWND) stavano nelle firme pubbliche, e
// quindi in ogni singolo punto di disegno dell'applicazione: sessanta chiamate
// a D2D1::RectF sparse fra HUD, telemetria e schede. Non era una scelta, era
// il risultato di non aver mai avuto una seconda piattaforma.
//
// Due livelli, perché le finestre sono due (operatore e proiezione):
//   GraphicsCore  factory e immagini DECODIFICATE, una volta sola per processo
//   Renderer      risorse legate al device: una per finestra

#include <cstddef>
#include <memory>
#include <string>

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

/** Rettangolo in coordinate di finestra, y crescente verso il basso. */
struct Rect {
    float left, top, right, bottom;

    float width() const noexcept { return right - left; }
    float height() const noexcept { return bottom - top; }
};

/** Dimensioni in pixel logici. */
struct Size {
    float width, height;
};

/** Costruttore breve: si scrive quanto D2D1::RectF, e vale ovunque. */
inline constexpr Rect rect(float left, float top, float right, float bottom) noexcept {
    return Rect{left, top, right, bottom};
}

/**
 * Risorse indipendenti dal device: factory, immagini decodificate e formati di
 * testo. Ne esiste una sola, condivisa da tutte le finestre — decodificare due
 * volte gli stessi dodici JPEG sarebbe puro spreco.
 */
class GraphicsCore {
public:
    GraphicsCore();
    ~GraphicsCore();
    GraphicsCore(const GraphicsCore&)            = delete;
    GraphicsCore& operator=(const GraphicsCore&) = delete;

    bool init();
    void shutdown();

    /** Decodifica <dir>/1.jpg .. <dir>/N.jpg (o .webp/.png). Una volta sola. */
    bool loadSprites(const std::wstring& dir, int count);

    std::size_t spriteCount() const noexcept;

private:
    friend class Renderer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Superficie di disegno di UNA finestra. Le texture sono risorse di device,
 * quindi ogni finestra ha le sue: è inevitabile, non una duplicazione sfuggita.
 */
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    /**
     * @param nativeWindow handle nativo della finestra: HWND su Windows,
     *        NSView* su macOS. È l'unico punto in cui la piattaforma entra, ed è
     *        opaco di proposito — chi chiama lo ha già in mano dal suo shell.
     */
    bool init(GraphicsCore& core, void* nativeWindow);
    void shutdown();

    /** Crea le texture di questo target dalle sorgenti già decodificate nel core. */
    bool uploadSprites();

    void resize(unsigned width, unsigned height);

    void begin(Color clear);
    /** @return false se il target va ricreato (device lost). */
    bool end();

    /**
     * Disegna lo sprite scalato attorno al centro della finestra.
     * @param scale moltiplicatore sopra la scala "cover" che riempie la finestra
     */
    void drawSprite(int index, float scale, float opacity);

    /** Come drawSprite ma dentro un rettangolo dato: serve alla miniatura. */
    void drawSpriteIn(int index, Rect box, float scale, float opacity);

    void fillRect(Rect r, Color color, float radius = 0.0f);
    void drawRectOutline(Rect r, Color color, float stroke, float radius = 0.0f);
    void drawText(const std::wstring& text, Rect box, float fontSize,
                  Color color, TextAlign align = TextAlign::Center, bool bold = false);

    void drawLine(Point a, Point b, Color color, float stroke = 1.0f);
    /** Spezzata: una geometria per chiamata, trascurabile a qualche centinaio di punti. */
    void drawPolyline(const Point* pts, std::size_t count, Color color, float stroke = 1.5f);

    /** Ritaglio rettangolare, per tenere le tracce dentro il loro riquadro. */
    void pushClip(Rect r);
    void popClip();

    Size size() const;
    bool ready() const noexcept;
    /** Texture effettivamente caricate SU QUESTO target, non quelle decodificate. */
    int  spriteCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mz::render
