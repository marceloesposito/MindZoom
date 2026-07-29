#pragma once

// Client BLE per l'headband Muse via WinRT GATT.
// Port di MuseBluetooth.js: stessi UUID, stessa sequenza di avvio, stesso
// formato di pacchetto, stessa politica di riaggancio (compreso il fix del
// tentativo singolo: due connect() concorrenti si sabotano a vicenda).

#include "config.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace mz::ble {

/** Un campione multi-canale già decodificato. */
struct Sample {
    std::array<double, config::kChannels>        uv{};   // microvolt
    std::array<std::uint16_t, config::kChannels> adc{};  // ADC unsigned grezzo
};

enum class State { Disconnected, Scanning, Connecting, Streaming };

const char* toString(State s) noexcept;

/**
 * Connessione al Muse e streaming EEG.
 *
 * Il callback dei campioni viene invocato da un thread del pool WinRT: deve
 * essere non bloccante (tipicamente una push su un ring lock-free).
 */
class MuseClient {
public:
    using SampleCallback = std::function<void(const Sample&)>;
    using LogCallback    = std::function<void(const std::string&)>;

    MuseClient();
    ~MuseClient();

    MuseClient(const MuseClient&)            = delete;
    MuseClient& operator=(const MuseClient&) = delete;

    void onSample(SampleCallback cb) { onSample_ = std::move(cb); }
    void onLog(LogCallback cb) { onLog_ = std::move(cb); }

    /**
     * Avvia scansione e connessione in background. Ritorna subito.
     * Lo stato osservabile è in state().
     */
    void start();

    /** Disconnessione voluta: niente riaggancio automatico. */
    void stop();

    /** Comando di resume dello streaming ('d'), usato dal watchdog. */
    void resumeStreaming();

    State       state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool        streaming() const noexcept { return state() == State::Streaming; }
    std::string deviceName() const;

    /** Millisecondi dall'ultimo pacchetto ricevuto; 0 se non è mai arrivato nulla. */
    std::int64_t millisSinceLastPacket() const;

    /** Diagnostica dell'ultimo pacchetto: lunghezza e campioni estratti. */
    int lastPacketLen() const noexcept { return packetLen_.load(std::memory_order_relaxed); }
    int lastPacketSamples() const noexcept { return packetSamples_.load(std::memory_order_relaxed); }

    /**
     * Notifiche ricevute sulla characteristic EEG, prima di qualunque filtro, e
     * quelle effettivamente riconosciute come pacchetti EEG. Distinguono due
     * guasti che si assomigliano: zero grezzi = le notifiche non arrivano
     * (sottoscrizione o comandi di avvio); grezzi ma zero valide = i pacchetti
     * arrivano ma il formato non è quello atteso.
     */
    std::uint64_t rawPackets() const noexcept { return rawPackets_.load(std::memory_order_relaxed); }
    std::uint64_t validPackets() const noexcept { return validPackets_.load(std::memory_order_relaxed); }

private:
    struct Impl;                 // isola winrt/*.h dal resto del progetto
    Impl* impl_ = nullptr;

    SampleCallback onSample_;
    LogCallback    onLog_;

    std::atomic<State>        state_{State::Disconnected};
    std::atomic<std::int64_t> lastPacketAt_{0};
    std::atomic<int>          packetLen_{0};
    std::atomic<int>          packetSamples_{0};
    std::atomic<std::uint64_t> rawPackets_{0};
    std::atomic<std::uint64_t> validPackets_{0};

    mutable std::mutex nameMutex_;
    std::string        deviceName_;

    friend struct Impl;
};

} // namespace mz::ble
