#include "app/telemetry.hpp"

#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mz::app {
namespace {

render::Color fade(render::Color c, float alpha) {
    return {c.r, c.g, c.b, c.a * alpha};
}

std::wstring fixed(double v, int decimals) {
    wchar_t buf[64]{};
    swprintf(buf, 64, L"%.*f", decimals, v);
    return buf;
}

/** Riquadro di un grafico, con la conversione valore -> pixel già risolta. */
struct Frame {
    render::Rect box{};
    double      lo = 0.0, hi = 1.0;   // dominio verticale
    std::size_t first = 0;            // primo campione visibile
    std::size_t count = 0;

    float y(double v) const {
        if (hi <= lo) return box.bottom;
        const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
        return static_cast<float>(box.bottom - t * (box.bottom - box.top));
    }
    float x(std::size_t i) const {
        if (count <= 1) return box.right;
        const double t = static_cast<double>(i) / static_cast<double>(count - 1);
        return static_cast<float>(box.left + t * (box.right - box.left));
    }
};

void drawFrame(render::Renderer& r, const Frame& f, const PlotTheme& th,
               const std::wstring& title, double secondsShown) {
    r.fillRect(f.box, th.panel, 6.0f);
    r.drawRectOutline(f.box, fade(th.grid, 0.8f), 1.0f, 6.0f);

    // Griglia temporale ogni 10 s: senza riferimenti una traccia non dice se una
    // cosa e' durata mezzo secondo o dieci.
    const double step = 10.0;
    for (double t = step; t < secondsShown; t += step) {
        const float gx = static_cast<float>(f.box.right -
                                            (t / secondsShown) * (f.box.right - f.box.left));
        r.drawLine({gx, f.box.top}, {gx, f.box.bottom}, fade(th.grid, 0.5f), 1.0f);
    }

    r.drawText(title, render::rect(f.box.left + 8, f.box.top + 4, f.box.right - 8, f.box.top + 22),
               12.0f, th.muted, render::TextAlign::Left);
}

/** Traccia di una grandezza estratta dai campioni. */
template <typename Get>
void plot(render::Renderer& r, const Frame& f, const TelemetryHistory& h, Get get,
          render::Color color, float stroke = 1.6f) {
    if (f.count < 2) return;

    std::vector<render::Point> pts;
    pts.reserve(f.count);
    for (std::size_t i = 0; i < f.count; ++i) {
        pts.push_back({f.x(i), f.y(get(h.at(f.first + i)))});
    }
    r.drawPolyline(pts.data(), pts.size(), color, stroke);
}

/** Fascia verticale continua: usata per la banda locale, che si muove. */
template <typename GetLo, typename GetHi>
void plotBand(render::Renderer& r, const Frame& f, const TelemetryHistory& h,
              GetLo getLo, GetHi getHi, render::Color color) {
    // Disegnata a colonne di un pixel: una geometria chiusa sarebbe piu'
    // elegante ma questa banda ha due bordi indipendenti che possono
    // incrociarsi, e le colonne non se ne accorgono nemmeno.
    for (std::size_t i = 0; i < f.count; ++i) {
        const auto& s = h.at(f.first + i);
        if (!s.calibValid) continue;
        const float x  = f.x(i);
        const float y0 = f.y(getHi(s));
        const float y1 = f.y(getLo(s));
        if (y1 <= y0) continue;
        r.fillRect(render::rect(x, y0, x + 2.0f, y1), color);
    }
}

/** Fondo colorato dove una condizione era vera: gating, lock. */
template <typename Pred>
void plotFlag(render::Renderer& r, const Frame& f, const TelemetryHistory& h, Pred pred,
              render::Color color) {
    for (std::size_t i = 0; i < f.count; ++i) {
        if (!pred(h.at(f.first + i))) continue;
        const float x = f.x(i);
        r.fillRect(render::rect(x, f.box.top, x + 2.0f, f.box.bottom), color);
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool TelemetryHistory::append(const ControlState& st, double targetFocus, double currentFocus,
                              bool locked) {
    if (st.frames == lastFrames_) return false;
    lastFrames_ = st.frames;

    TelemetrySample s;
    s.rawIndex      = st.rawIndex;
    s.smoothedIndex = st.smoothedIndex;
    s.absMin        = st.absMin;
    s.absMax        = st.absMax;
    s.neutral       = st.neutral;
    s.localMin      = st.localMin;
    s.localMax      = st.localMax;
    s.velocity      = st.velocity;
    s.gate          = st.gate;
    s.targetFocus   = targetFocus;
    s.currentFocus  = currentFocus;
    s.maxAbsRaw     = st.maxAbsRaw;
    s.contactOk     = st.contactOk;
    s.artifact      = st.artifact;
    s.calibValid    = st.calibValid;
    s.locked        = locked;

    buf_[write_] = s;
    write_ = (write_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    return true;
}

const TelemetrySample& TelemetryHistory::at(std::size_t i) const {
    // Con il ring pieno il piu' vecchio sta in write_, altrimenti in 0.
    const std::size_t base = (count_ == kCapacity) ? write_ : 0;
    return buf_[(base + i) % kCapacity];
}

void drawTelemetry(render::Renderer& r, render::Rect area, const TelemetryHistory& history,
                   const ControlState& st, const control::Tunables& tune,
                   const PlotTheme& theme, double secondsShown) {
    const auto visible = static_cast<std::size_t>(secondsShown * config::kControlHz);
    const std::size_t count = std::min(history.size(), visible);
    const std::size_t first = history.size() - count;

    const float gap = 10.0f;
    const float h   = (area.bottom - area.top - gap * 3.0f) / 4.0f;
    const auto  row = [&](int i) {
        const float top = area.top + static_cast<float>(i) * (h + gap);
        return render::rect(area.left, top, area.right, top + h);
    };

    // --- 1. Indice e banda di controllo -------------------------------------
    // E' il grafico che spiega tutto: si vede se `c` sta sbattendo contro il
    // bordo della banda o se ci scorre dentro.
    {
        Frame f;
        f.box = row(0);
        f.first = first;
        f.count = count;

        // Dominio: la banda calibrata se c'è, altrimenti l'escursione vista.
        if (st.calibValid && st.absMax > st.absMin) {
            const double pad = (st.absMax - st.absMin) * 0.15;
            f.lo = st.absMin - pad;
            f.hi = st.absMax + pad;
        } else {
            f.lo = 1e30; f.hi = -1e30;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& s = history.at(first + i);
                f.lo = std::min({f.lo, s.rawIndex, s.smoothedIndex});
                f.hi = std::max({f.hi, s.rawIndex, s.smoothedIndex});
            }
            if (f.hi <= f.lo) { f.lo = 0.0; f.hi = 1.0; }
        }

        drawFrame(r, f, theme, L"Indice di concentrazione", secondsShown);
        r.pushClip(f.box);

        plotFlag(r, f, history, [](const TelemetrySample& s) { return !s.contactOk; },
                 fade(theme.bad, 0.18f));
        plotFlag(r, f, history, [](const TelemetrySample& s) { return s.artifact; },
                 fade(theme.warn, 0.15f));

        plotBand(r, f, history,
                 [](const TelemetrySample& s) { return s.localMin; },
                 [](const TelemetrySample& s) { return s.localMax; },
                 {1.0f, 1.0f, 1.0f, 0.07f});

        if (st.calibValid) {
            for (const double level : {st.absMin, st.neutral, st.absMax}) {
                const float y = f.y(level);
                r.drawLine({f.box.left, y}, {f.box.right, y}, fade(theme.muted, 0.45f), 1.0f);
            }
        }

        plot(r, f, history, [](const TelemetrySample& s) { return s.rawIndex; },
             fade(theme.muted, 0.55f), 1.0f);
        plot(r, f, history, [](const TelemetrySample& s) { return s.smoothedIndex; },
             theme.accent, 1.8f);

        r.popClip();
        r.drawText(L"c " + fixed(st.smoothedIndex, 2),
                   render::rect(f.box.left, f.box.bottom - 20, f.box.right - 8, f.box.bottom - 4),
                   12.0f, theme.accent, render::TextAlign::Left);
    }

    // --- 2. Velocità e soglie del detent -------------------------------------
    // Qui la salute del controllo si giudica in un secondo: una linea continua
    // invece di un pettine.
    {
        Frame f;
        f.box = row(1);
        f.first = first;
        f.count = count;
        const double span = std::max(tune.gain() * 1.15, 1e-6);
        f.lo = -span;
        f.hi =  span;

        drawFrame(r, f, theme, L"Velocita' di zoom", secondsShown);
        r.pushClip(f.box);

        plotFlag(r, f, history, [](const TelemetrySample& s) { return s.locked; },
                 {1.0f, 1.0f, 1.0f, 0.06f});

        const float zero = f.y(0.0);
        r.drawLine({f.box.left, zero}, {f.box.right, zero}, fade(theme.muted, 0.5f), 1.0f);

        for (const double frac : {config::kEnterHoldFrac, config::kBreakHoldFrac}) {
            const double v = frac * tune.gain();
            const auto c = (frac == config::kEnterHoldFrac) ? fade(theme.muted, 0.35f)
                                                            : fade(theme.warn, 0.40f);
            r.drawLine({f.box.left, f.y(v)},  {f.box.right, f.y(v)},  c, 1.0f);
            r.drawLine({f.box.left, f.y(-v)}, {f.box.right, f.y(-v)}, c, 1.0f);
        }

        plot(r, f, history, [](const TelemetrySample& s) { return s.velocity; }, theme.ok, 1.8f);

        r.popClip();
        r.drawText(L"v " + fixed(st.velocity, 3) + L"   soglie: aggancio / sgancio",
                   render::rect(f.box.left, f.box.bottom - 20, f.box.right - 8, f.box.bottom - 4),
                   12.0f, theme.muted, render::TextAlign::Left);
    }

    // --- 3. Focus -------------------------------------------------------------
    {
        Frame f;
        f.box = row(2);
        f.first = first;
        f.count = count;
        f.lo = 0.0;
        f.hi = 1.0;

        drawFrame(r, f, theme, L"Profondita' di zoom", secondsShown);
        r.pushClip(f.box);

        // I dodici livelli: dicono quanto si e' vicini a un detent.
        for (int i = 0; i < config::kTotalImages; ++i) {
            const float y = f.y(static_cast<double>(i) / (config::kTotalImages - 1));
            r.drawLine({f.box.left, y}, {f.box.right, y}, fade(theme.grid, 0.35f), 1.0f);
        }

        plot(r, f, history, [](const TelemetrySample& s) { return s.targetFocus; },
             fade(theme.muted, 0.7f), 1.0f);
        plot(r, f, history, [](const TelemetrySample& s) { return s.currentFocus; },
             theme.ink, 1.8f);

        r.popClip();
    }

    // --- 4. Qualità del segnale ----------------------------------------------
    {
        Frame f;
        f.box = row(3);
        f.first = first;
        f.count = count;
        f.lo = 0.0;
        f.hi = 1e-6;
        for (std::size_t i = 0; i < count; ++i) {
            f.hi = std::max(f.hi, history.at(first + i).maxAbsRaw);
        }
        f.hi = std::max(f.hi * 1.15, config::kArtifactUvFloor * 1.2);

        drawFrame(r, f, theme, L"Ampiezza del segnale (uV)", secondsShown);
        r.pushClip(f.box);

        plotFlag(r, f, history, [](const TelemetrySample& s) { return !s.contactOk; },
                 fade(theme.bad, 0.18f));
        plotFlag(r, f, history, [](const TelemetrySample& s) { return s.artifact; },
                 fade(theme.warn, 0.15f));

        const float floorY = f.y(config::kArtifactUvFloor);
        r.drawLine({f.box.left, floorY}, {f.box.right, floorY}, fade(theme.warn, 0.4f), 1.0f);

        plot(r, f, history, [](const TelemetrySample& s) { return s.maxAbsRaw; },
             fade(theme.ink, 0.8f), 1.4f);

        r.popClip();
        r.drawText(L"rosso: contatto assente   giallo: artefatto",
                   render::rect(f.box.left, f.box.bottom - 20, f.box.right - 8, f.box.bottom - 4),
                   12.0f, fade(theme.muted, 0.8f), render::TextAlign::Left);
    }
}

} // namespace mz::app
