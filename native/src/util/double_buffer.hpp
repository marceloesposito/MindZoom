#pragma once

// Double buffer atomico per il percorso DSP -> render.
// Il DSP pubblica ~5.3 volte al secondo, il render legge a vsync (60+ Hz):
// il lettore deve sempre ottenere l'ultimo stato COMPLETO, mai un mezzo
// aggiornamento, e senza mai bloccare lo scrittore.

#include <atomic>
#include <array>

namespace mz::util {

/**
 * Sequence lock a due slot: lo scrittore riempie lo slot inattivo e poi lo
 * pubblica con uno store release. Il lettore legge l'indice pubblicato e copia.
 *
 * Presuppone un solo scrittore. Il lettore può perdere aggiornamenti intermedi
 * (è voluto: al render interessa solo lo stato più recente).
 */
template <typename T>
class DoubleBuffer {
public:
    void publish(const T& value) noexcept {
        const int next = 1 - live_.load(std::memory_order_relaxed);
        slots_[static_cast<std::size_t>(next)] = value;
        // release: il contenuto dello slot è visibile prima dell'indice.
        live_.store(next, std::memory_order_release);
    }

    T read() const noexcept {
        const int idx = live_.load(std::memory_order_acquire);
        return slots_[static_cast<std::size_t>(idx)];
    }

private:
    std::array<T, 2> slots_{};
    std::atomic<int> live_{0};
};

} // namespace mz::util
