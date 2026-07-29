#pragma once

// Ring lock-free a singolo produttore / singolo consumatore.
// Percorso BLE -> DSP: il thread BLE scrive i campioni decodificati, il thread
// DSP li consuma. Nessun lock, nessuna allocazione: un callback GATT non deve
// mai poter bloccare, altrimenti si perdono pacchetti.

#include <atomic>
#include <array>
#include <cstddef>

namespace mz::util {

/**
 * Capacità arrotondata a potenza di due: l'indice si avvolge con una maschera
 * invece che con un modulo.
 *
 * Il ring tiene Capacity-1 elementi utili: la distinzione fra pieno e vuoto ha
 * bisogno di una posizione libera.
 */
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity deve essere potenza di due");
    static_assert(Capacity >= 2, "Capacity troppo piccola");

public:
    /** @return false se il ring è pieno (il consumatore è in ritardo). */
    bool push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        // acquire: non deve vedere una coda più vecchia di quella reale.
        if (next == tail_.load(std::memory_order_acquire)) return false;

        buffer_[head] = value;
        // release: la scrittura del dato deve essere visibile prima dell'indice.
        head_.store(next, std::memory_order_release);
        return true;
    }

    /** @return false se il ring è vuoto. */
    bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;

        out = buffer_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }

    void clear() noexcept {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};
    // Su cache line separate: senza, i due thread si contendono la stessa riga
    // e il ring va più lento di un mutex (false sharing).
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace mz::util
