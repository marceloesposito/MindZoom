#pragma once

// Decodifica dei pacchetti EEG del Muse. Port 1:1 di MuseBluetooth.js
// (_get14BitRaw / _centerSample / conversione µV).

#include "config.hpp"

#include <array>
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

// ---------------------------------------------------------------------------
// Formato dei pacchetti Muse S Athena
// ---------------------------------------------------------------------------
//
// Una notifica contiene PIÙ pacchetti concatenati, ciascuno con la propria
// etichetta. L'etichetta è un byte diviso in due nibble: quello alto codifica la
// frequenza, quello basso il tipo di dato. Dopo l'etichetta ci sono 4 byte che
// non servono qui, poi il payload.
//
//   [etichetta][4 byte][payload][etichetta][4 byte][payload]...
//
// Riferimento: AbosaSzakal/MuseAthenaDataformatParser.

/** Tipo di dato: nibble basso dell'etichetta. */
enum class PacketType : int {
    Invalid   = 0,
    Eeg4      = 1,
    Eeg8      = 2,
    DrlRef    = 3,
    Optics4   = 4,
    Optics8   = 5,
    Optics16  = 6,
    Imu       = 7,
    Battery   = 8
};

inline PacketType packetType(std::uint8_t tag) noexcept {
    return static_cast<PacketType>(tag & 0x0F);
}

/**
 * Byte di payload di un pacchetto, oppure -1 se la dimensione non è nota.
 *
 * Queste dimensioni sono MISURATE sui pacchetti veri della fascia, non dedotte
 * da quanti bit servirebbero. Il metodo: il byte dopo l'etichetta è un contatore
 * che avanza di 1 fra pacchetti dello stesso tipo, quindi due pacchetti EEG8
 * consecutivi delimitano una regione di lunghezza nota e ciò che sta in mezzo si
 * ricava per differenza. Verifica finale: la catena deve piastrellare la
 * notifica fino all'ultimo byte, e con questa tabella lo fa su 788 notifiche
 * su 797 (98,9%); le 9 restanti hanno etichette senza senso, cioè notifiche
 * corrotte.
 *
 * Le dimensioni dedotte a tavolino erano sbagliate proprio dove contava: OPT16
 * "3 campioni × 16 canali × 20 bit" darebbe 120, ma il pacchetto vero ne porta
 * 40. Con 120 la catena si spezzava e non usciva NIENTE, perché un tipo di
 * dimensione ignota interrompe anche la lettura dei pacchetti EEG che seguono.
 */
inline int payloadBytes(PacketType t) noexcept {
    switch (t) {
        case PacketType::Eeg4:     return  14;   // 2 campioni × 4 canali × 14 bit
        case PacketType::Eeg8:     return  28;   // 2 campioni × 8 canali × 14 bit
        case PacketType::DrlRef:   return  24;   // misurato, 56 osservazioni su 56
        case PacketType::Optics4:  return  30;
        case PacketType::Optics8:  return  60;
        case PacketType::Optics16: return  40;   // misurato (NON 120)
        case PacketType::Imu:      return  36;   // misurato: 3 campioni × 6 × 16 bit
        case PacketType::Battery:  return  20;   // misurato
        default:                   return -1;
    }
}

inline bool isEeg(PacketType t) noexcept {
    return t == PacketType::Eeg4 || t == PacketType::Eeg8;
}

inline int eegChannels(PacketType t) noexcept {
    return (t == PacketType::Eeg8) ? 8 : 4;
}

/** Ogni pacchetto: 1 byte di etichetta + 4 byte non usati, poi il payload. */
inline constexpr std::size_t kPacketHeaderBytes = 5;

/**
 * Prova a percorrere la catena di pacchetti a partire da `start` e riporta
 * quanti byte si riescono a consumare.
 *
 * Serve a TROVARE dove comincia la catena invece di assumerlo. Il prefisso di
 * trasporto della notifica non è documentato in modo affidabile, e sbagliarlo di
 * pochi byte non produce un errore: produce campioni plausibili e falsi. Il
 * punto di partenza giusto è quello che consuma la notifica fino in fondo; uno
 * sbagliato inciampa quasi subito in un'etichetta senza significato.
 *
 * @param eegSamples se non nullo, riceve quanti campioni EEG conterrebbe
 * @param packets    se non nullo, riceve quanti pacchetti sono stati percorsi
 * @return byte consumati dalla catena (0 se non parte nemmeno)
 */
inline std::size_t walkPackets(const std::uint8_t* data, std::size_t len, std::size_t start,
                               int* eegSamples = nullptr, int* packets = nullptr) noexcept {
    if (eegSamples) *eegSamples = 0;
    if (packets) *packets = 0;
    if (!data || start >= len) return 0;

    std::size_t i = start;
    while (i < len) {
        const auto type = packetType(data[i]);
        const int  size = payloadBytes(type);
        if (size < 0) break;

        const std::size_t next = i + kPacketHeaderBytes + static_cast<std::size_t>(size);
        if (next > len) break;   // pacchetto troncato: la catena finisce qui

        if (eegSamples && isEeg(type)) *eegSamples += 2;
        if (packets) ++(*packets);
        i = next;
    }
    return i - start;
}

/** Esito della lettura di una notifica. */
struct ChainResult {
    int  eegSamples = 0;      // campioni multi-canale emessi
    bool sawEeg     = false;  // c'era almeno un pacchetto EEG
};

/**
 * Percorre la catena da `start` ed emette i campioni EEG che incontra.
 *
 * `fn(const std::uint16_t* adc, int numChannels)` viene chiamata una volta per
 * campione. Sta qui, e non dentro il trasporto Bluetooth, perché serve in due
 * posti: sulle notifiche dal vivo e sui pacchetti grezzi di una registrazione
 * che si vuole ri-decodificare. Due copie della stessa aritmetica sarebbero due
 * cose da tenere allineate, e la seconda non verrebbe mai esercitata.
 */
template <typename Fn>
ChainResult forEachEegSample(const std::uint8_t* data, std::size_t len,
                             std::size_t start, Fn&& fn) {
    ChainResult r;
    if (!data || start >= len) return r;

    std::size_t i = start;
    while (i < len) {
        const auto type = packetType(data[i]);
        const int  size = payloadBytes(type);
        if (size < 0) break;

        const std::size_t payloadAt = i + kPacketHeaderBytes;
        const std::size_t next      = payloadAt + static_cast<std::size_t>(size);
        if (next > len) break;

        if (isEeg(type)) {
            r.sawEeg = true;
            const int          numChannels = eegChannels(type);
            const std::uint8_t* payload    = data + payloadAt;
            const auto          payloadLen = static_cast<std::size_t>(size);
            const std::size_t   numSamples = samplesInPayload(payloadLen, numChannels);

            std::size_t bitOffset = 0;
            for (std::size_t s = 0; s < numSamples; ++s) {
                std::uint16_t adc[16] = {};
                const int n = (numChannels < 16) ? numChannels : 16;
                for (int ch = 0; ch < n; ++ch) {
                    adc[ch] = unpack14(payload, payloadLen, bitOffset);
                    bitOffset += 14;
                }
                fn(static_cast<const std::uint16_t*>(adc), n);
                ++r.eegSamples;
            }
        }
        i = next;
    }
    return r;
}

/**
 * Byte che possono restare non consumati in fondo a una notifica valida.
 *
 * Zero: la catena chiude ESATTAMENTE sull'ultimo byte. Valeva 3 finché le
 * dimensioni dei pacchetti erano dedotte a tavolino e la catena non chiudeva
 * quasi mai; con le dimensioni misurate chiude su 788 notifiche su 797, e la
 * tolleranza serviva solo a nascondere l'errore. Pretendere l'incastro esatto
 * rende il riconoscimento dell'offset molto più selettivo.
 */
inline constexpr std::size_t kChainTailSlack = 0;

/** Oltre questo prefisso non si cerca: ogni formato plausibile è più compatto. */
inline constexpr std::size_t kMaxPrefixSearch = 40;

/**
 * Candidati di partenza per UNA notifica: gli offset da cui i pacchetti si
 * incastrano fino in fondo.
 *
 * Su una notifica sola questo non basta a decidere. Con sei tipi noti su sedici
 * valori di etichetta, dei byte casuali producono catene che arrivano in fondo
 * piuttosto spesso - misurato: più di metà delle volte su buffer da 64 byte.
 * Serve il vincolo del livello superiore, in ChainLocator.
 */
template <typename Fn>
void forEachChainCandidate(const std::uint8_t* data, std::size_t len, Fn&& fn) {
    const std::size_t limit = (len < kMaxPrefixSearch) ? len : kMaxPrefixSearch;
    for (std::size_t s = 0; s < limit; ++s) {
        int packets = 0, eeg = 0;
        const std::size_t consumed = walkPackets(data, len, s, &eeg, &packets);
        // Due pacchetti almeno: uno solo che per caso finisce in fondo non dice
        // niente. E almeno un EEG, che è l'unica cosa che ci interessa leggere.
        if (packets < 2 || eeg < 1) continue;
        if (s + consumed + kChainTailSlack >= len) fn(s);
    }
}

/**
 * Trova l'offset di partenza osservando PIÙ notifiche.
 *
 * È il vincolo che rende la cosa decidibile: l'offset vero è lo stesso in ogni
 * notifica, mentre le coincidenze casuali cadono ogni volta altrove. Si
 * raccolgono voti finché uno degli offset non domina; prima di allora non si
 * emette nulla, che è questione di frazioni di secondo.
 *
 * Il difetto che ha reso inutile un'intera sessione era esattamente questo:
 * un offset sbagliato non fallisce, produce campioni plausibili e falsi. Qui
 * l'offset non si assume, si misura.
 */
class ChainLocator {
public:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    /** Notifiche da osservare prima di decidere. A ~12 Hz sono ~2 secondi. */
    static constexpr int kNeeded = 24;
    /** Frazione di notifiche che deve concordare sullo stesso offset. */
    static constexpr int kAgreePercent = 70;

    /** @return true se dopo questa notifica l'offset è noto. */
    bool offer(const std::uint8_t* data, std::size_t len) {
        if (locked_ != kNone) return true;

        forEachChainCandidate(data, len, [&](std::size_t s) {
            if (s < votes_.size()) ++votes_[s];
        });
        ++seen_;

        if (seen_ < kNeeded) return false;

        std::size_t best = kNone;
        int bestVotes = 0;
        for (std::size_t s = 0; s < votes_.size(); ++s) {
            if (votes_[s] > bestVotes) { bestVotes = votes_[s]; best = s; }
        }

        if (best != kNone && bestVotes * 100 >= seen_ * kAgreePercent) {
            locked_ = best;
            return true;
        }

        // Nessun accordo: si riparte da capo invece di accontentarsi. Se il
        // formato non è questo, è meglio non leggere niente che leggere male.
        votes_.fill(0);
        seen_ = 0;
        return false;
    }

    bool        locked() const noexcept { return locked_ != kNone; }
    std::size_t offset() const noexcept { return locked_; }
    int         observed() const noexcept { return seen_; }
    void        reset() noexcept { votes_.fill(0); seen_ = 0; locked_ = kNone; }

private:
    std::array<int, kMaxPrefixSearch> votes_{};
    int         seen_   = 0;
    std::size_t locked_ = kNone;
};

} // namespace mz::dsp
