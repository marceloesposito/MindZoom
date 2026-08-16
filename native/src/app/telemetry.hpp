#pragma once

// Storia dei segnali di controllo e disegno dei grafici contro il tempo.
//
// Perché serve: le righe numeriche del pannello dicono QUANTO VALE ADESSO, e
// per giudicare un controllo serve sapere CHE FORMA HA NEL TEMPO. Il difetto
// che rendeva lo zoom inutilizzabile - un'uscita che si azzerava a ogni
// oscillazione dell'indice - da un numero che cambia non si vede; da una
// traccia si vede in un secondo.

#include "app/shared_state.hpp"
#include "control/tunables.hpp"
#include "render/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mz::app {

/** Un istante di storia. Solo ciò che serve alle tracce: si scrive a 5.3 Hz. */
struct TelemetrySample {
    double rawIndex      = 0.0;
    double smoothedIndex = 0.0;
    double absMin = 0.0, absMax = 0.0, neutral = 0.0;
    double localMin = 0.0, localMax = 0.0;
    double velocity     = 0.0;
    double gate         = 0.0;
    double targetFocus  = 0.0;
    double currentFocus = 0.0;
    double maxAbsRaw    = 0.0;
    bool   contactOk    = true;
    bool   artifact     = false;
    bool   calibValid   = false;
    bool   locked       = false;
};

/**
 * Ring della storia recente.
 *
 * Si appende SOLO quando il contatore di frame del DSP cambia: campionare a
 * 60 Hz un segnale che si aggiorna a 5.3 duplicherebbe ogni valore undici
 * volte, e la traccia mostrerebbe scalini che nel controllo non esistono.
 * Così non serve nemmeno un secondo canale fra i thread.
 */
class TelemetryHistory {
public:
    static constexpr std::size_t kCapacity = 512;   // ~96 s a 5.3 Hz

    /** @return true se il campione è stato aggiunto (frame nuovo). */
    bool append(const ControlState& st, double targetFocus, double currentFocus, bool locked);

    void clear() noexcept { count_ = 0; write_ = 0; lastFrames_ = 0; }

    std::size_t size() const noexcept { return count_; }
    /** 0 = il più vecchio disponibile. */
    const TelemetrySample& at(std::size_t i) const;

private:
    std::array<TelemetrySample, kCapacity> buf_{};
    std::size_t   write_ = 0;
    std::size_t   count_ = 0;
    std::uint64_t lastFrames_ = 0;
};

/** Tavolozza dei grafici, tenuta accanto a quella del resto dell'interfaccia. */
struct PlotTheme {
    render::Color ink, muted, accent, ok, warn, bad, grid, panel;
};

/**
 * Disegna il pannello di telemetria completo dentro `area`.
 * @param secondsShown ampiezza della finestra temporale sull'asse x
 */
void drawTelemetry(render::Renderer& r, render::Rect area, const TelemetryHistory& history,
                   const ControlState& st, const control::Tunables& tune,
                   const PlotTheme& theme, double secondsShown = 60.0);

} // namespace mz::app
