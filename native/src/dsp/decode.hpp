#pragma once

// Decodifica dei pacchetti EEG del Muse. Port 1:1 di MuseBluetooth.js
// (_get14BitRaw / _centerSample / conversione µV).

#include "config.hpp"

#include <cstddef>
#include <cstdint>

namespace mz::dsp {

/**
 * Estrae il campione a 14 bit come intero UNSIGNED (0..16383).
 * I campioni sono packed senza allineamento ai byte: si leggono 3 byte e si
 * shifta del resto del bit offset.
 */
inline std::uint16_t unpack14(const std::uint8_t* payload, std::size_t payloadLen,
                              std::size_t bitOffset) noexcept {
    const std::size_t byteOffset = bitOffset >> 3;
    const unsigned    bitShift   = static_cast<unsigned>(bitOffset & 7u);

    // Come in JS (payload[i] || 0): oltre il buffer si legge 0, niente UB.
    const std::uint32_t b0 = (byteOffset     < payloadLen) ? payload[byteOffset]     : 0u;
    const std::uint32_t b1 = (byteOffset + 1 < payloadLen) ? payload[byteOffset + 1] : 0u;
    const std::uint32_t b2 = (byteOffset + 2 < payloadLen) ? payload[byteOffset + 2] : 0u;

    return static_cast<std::uint16_t>(((b0 | (b1 << 8) | (b2 << 16)) >> bitShift) & 0x3FFFu);
}

/**
 * Converte l'unsigned in valore centrato sullo zero.
 * L'ADC del Muse emette unsigned centrati sul fondo scala / 2 (8192 a 14 bit).
 * Interpretarli in complemento a due produce un'onda quadra da ±725 µV, non EEG.
 */
inline int centerSample(std::uint16_t raw) noexcept {
    return static_cast<int>(raw) - config::kAdcCenter;
}

inline double toMicrovolts(int centered) noexcept {
    return static_cast<double>(centered) * config::kAdcToMicrovolts;
}

/**
 * Numero di campioni contenuti nel payload. NON è fisso: dipende da quanti ce ne
 * stanno. Hardcodarlo (il bug originale lo fissava a 2) scarta tutti i campioni
 * successivi e riduce il rate effettivo a una frazione dei 256 Hz.
 */
inline std::size_t samplesInPayload(std::size_t payloadLen, int numChannels) noexcept {
    if (numChannels <= 0) return 0;
    return (payloadLen * 8u) / (14u * static_cast<std::size_t>(numChannels));
}

} // namespace mz::dsp
