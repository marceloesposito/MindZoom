#pragma once

// Registrazione e riproduzione dei campioni grezzi della fascia.
//
// A cosa serve: il comportamento del controllo si giudica solo su segnale vero,
// e rimettere la fascia ad ogni tentativo rende il confronto inutile - quando
// riprovi non ricordi piu' com'era. Registrata una sessione, la si riproduce
// quante volte si vuole con tarature diverse, senza fascia.
//
// Il punto di innesto e' il ring SPSC: chi riproduce ci scrive dentro
// esattamente come farebbe il callback GATT, quindi tutto cio' che sta a valle
// (STFT, gating, calibrazione, controllo, render) non sa nemmeno che il segnale
// viene da un file. Nessun percorso alternativo da mantenere.
//
// ATTENZIONE: questo NON e' l'input finto rimosso a suo tempo. Qui si rigioca
// EEG realmente registrato attraverso la pipeline vera; la vecchia simulazione
// fabbricava una velocita' che non era mai passata per il DSP.

#include "ble/muse.hpp"
#include "dsp/decode.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mz::ble {
namespace detail {

#ifndef _WIN32
/**
 * `_wfopen` è un'API CRT di Windows, non esiste altrove. Fuori da Windows i
 * percorsi si convertono a UTF-8 (quello che il filesystem si aspetta) e si
 * passa per `fopen`. Nessuna gestione delle coppie surrogate: quelle esistono
 * solo in UTF-16, cioè solo su Windows, dove questa funzione non viene
 * compilata - lì `wchar_t` è già UTF-16 e si passa diretto a `_wfopen`.
 */
inline std::string toUtf8(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t wc : s) {
        const auto cp = static_cast<std::uint32_t>(wc);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline std::string toNarrow(const wchar_t* s) {
    std::string out;
    for (; *s; ++s) out.push_back(static_cast<char>(*s));
    return out;
}
#endif

/**
 * Il file è un artefatto locale scritto dal programma stesso: le varianti _s
 * non aggiungono sicurezza qui, chiedono solo di controllare un errno che già
 * si ricava dal puntatore nullo.
 */
inline std::FILE* openFile(const std::wstring& path, const wchar_t* mode) {
#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return _wfopen(path.c_str(), mode);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
    return std::fopen(toUtf8(path).c_str(), toNarrow(mode).c_str());
#endif
}

/** Controparte di `openFile`: `_wremove` su Windows, `remove` su UTF-8 altrove. */
inline bool removeFile(const std::wstring& path) {
#ifdef _WIN32
    return _wremove(path.c_str()) == 0;
#else
    return std::remove(toUtf8(path).c_str()) == 0;
#endif
}

} // namespace detail

// I campioni sono banalmente copiabili e il file e' un artefatto locale, quindi
// si scrive lo struct cosi' com'e'. L'intestazione serve a rifiutare un file
// incompatibile invece di interpretarlo a caso: leggere campioni con un layout
// diverso non darebbe un errore, darebbe EEG plausibile ma falso.
inline constexpr char kRecordMagic[8] = {'M', 'Z', 'R', 'E', 'C', '\0', '\0', '\0'};

// Versione 1: solo campioni decodificati, uno dopo l'altro.
// Versione 2: record etichettati, e soprattutto i PACCHETTI GREZZI del
//   Bluetooth accanto ai campioni. Una registrazione che conserva i byte
//   originali si puo' ri-decodificare quando il decodificatore cambia; una che
//   conserva solo l'uscita del decodificatore e' persa insieme a lui. Il primo
//   difetto serio trovato sul campo e' stato proprio nel decodificatore.
inline constexpr std::uint32_t kRecordVersion = 2;

struct RecordHeader {
    char          magic[8];
    std::uint32_t version;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t sampleBytes;
};

/** Etichetta di record, solo dalla versione 2 in poi. */
enum class RecordTag : std::uint8_t { Sample = 1, RawPacket = 2 };

inline RecordHeader makeRecordHeader() {
    RecordHeader h{};
    std::memcpy(h.magic, kRecordMagic, sizeof(kRecordMagic));
    h.version     = kRecordVersion;
    h.sampleRate  = config::kSampleRate;
    h.channels    = config::kChannels;
    h.sampleBytes = static_cast<std::uint32_t>(sizeof(Sample));
    return h;
}

inline bool recordHeaderOk(const RecordHeader& h) {
    return std::memcmp(h.magic, kRecordMagic, sizeof(kRecordMagic)) == 0 &&
           (h.version == 1 || h.version == 2) &&
           h.sampleRate == config::kSampleRate &&
           h.channels == config::kChannels &&
           h.sampleBytes == sizeof(Sample);
}

/**
 * Scrittore. Un solo thread lo usa (quello che consuma il ring): la scrittura
 * passa per il buffer di stdio, quindi nel caso normale e' una memcpy e non
 * tocca il disco.
 *
 * Il file nasce al PRIMO campione, non all'avvio. Una sessione senza fascia
 * collegata altrimenti lascerebbe un file di sole 24 byte, che poi ricompare
 * nell'elenco delle registrazioni e viene rifiutato: un artefatto inutile che
 * si presenta come un guasto.
 */
class Recorder {
public:
    Recorder() = default;
    ~Recorder() { close(); }

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    /** Prepara la registrazione. Il file si crea quando arriva il primo campione. */
    bool arm(const std::wstring& path) {
        close();
        path_  = path;
        armed_ = !path.empty();
        count_ = 0;
        return armed_;
    }

    void write(const Sample& s) {
        if (!armed_) return;
        if (!f_ && !openNow()) return;
        const auto tag = static_cast<std::uint8_t>(RecordTag::Sample);
        if (std::fwrite(&tag, 1, 1, f_) != 1) return;
        if (std::fwrite(&s, sizeof(s), 1, f_) == 1) ++count_;
    }

    /**
     * I byte esatti della notifica Bluetooth, prima di qualunque
     * interpretazione. Sono l'unica cosa che resta utile se il decodificatore
     * si rivela sbagliato: da questi si puo' ricostruire tutto, dai campioni
     * decodificati no.
     */
    void writeRaw(const std::uint8_t* data, std::size_t len) {
        if (!armed_ || !data || len == 0 || len > 0xFFFF) return;
        if (!f_ && !openNow()) return;
        const auto tag = static_cast<std::uint8_t>(RecordTag::RawPacket);
        const auto n16 = static_cast<std::uint16_t>(len);
        if (std::fwrite(&tag, 1, 1, f_) != 1) return;
        if (std::fwrite(&n16, sizeof(n16), 1, f_) != 1) return;
        if (std::fwrite(data, 1, len, f_) == len) ++packets_;
    }

    /**
     * Porta su disco quanto è nel buffer.
     *
     * Va chiamato periodicamente: la chiusura dell'applicazione può richiedere
     * secondi (il thread Bluetooth può essere dentro una scansione lunga), e chi
     * si stanca di aspettare termina il processo. Senza flush periodico si
     * perderebbe l'ultimo pezzo di sessione - proprio quello appena registrato.
     */
    void flush() {
        if (f_) std::fflush(f_);
    }

    void close() {
        if (f_) {
            std::fclose(f_);
            f_ = nullptr;
        }
        armed_ = false;
    }

    /** Registrazione richiesta per questa sessione (anche se ancora senza dati). */
    bool armed() const noexcept { return armed_; }
    /** Almeno un record e' finito su disco: solo allora il file esiste. */
    bool hasData() const noexcept { return count_ > 0 || packets_ > 0; }

    std::uint64_t      count() const noexcept { return count_; }
    std::uint64_t      packets() const noexcept { return packets_; }
    double             seconds() const noexcept {
        return static_cast<double>(count_) / config::kSampleRate;
    }
    const std::wstring& path() const noexcept { return path_; }

private:
    bool openNow() {
        f_ = detail::openFile(path_, L"wb");
        if (!f_) {
            armed_ = false;          // inutile riprovare ad ogni campione
            return false;
        }
        const auto h = makeRecordHeader();
        if (std::fwrite(&h, sizeof(h), 1, f_) != 1) {
            std::fclose(f_);
            f_ = nullptr;
            armed_ = false;
            return false;
        }
        return true;
    }

    std::FILE*    f_ = nullptr;
    std::wstring  path_;
    bool          armed_ = false;
    std::uint64_t count_ = 0;
    std::uint64_t packets_ = 0;
};

/** Esito del caricamento, con il motivo in chiaro quando fallisce. */
struct LoadResult {
    std::vector<Sample>                   samples;
    std::vector<std::vector<std::uint8_t>> packets;   // vuoto per i file v1
    std::uint32_t                         version = 0;
    bool                                  ok = false;
    bool                                  redecoded = false;  // campioni rifatti dai grezzi
    std::string                           error;

    double seconds() const {
        return static_cast<double>(samples.size()) / config::kSampleRate;
    }
};

/**
 * Ricostruisce i campioni dai pacchetti grezzi col decodificatore ATTUALE.
 *
 * È la ragione per cui il formato v2 conserva i byte del Bluetooth. Il primo
 * difetto serio trovato sul campo era nel decodificatore: le sessioni registrate
 * allora contengono zero campioni e settecento notifiche perfettamente valide, e
 * senza questo passaggio resterebbero inutilizzabili pur avendo tutto il
 * necessario dentro.
 *
 * L'offset di partenza si misura sui pacchetti registrati esattamente come dal
 * vivo, con ChainLocator: è lo stesso codice, quindi rigiocare una registrazione
 * mette alla prova anche quello.
 */
inline std::vector<Sample> decodeSamples(
        const std::vector<std::vector<std::uint8_t>>& packets) {
    std::vector<Sample> out;
    if (packets.empty()) return out;

    dsp::ChainLocator locator;
    for (const auto& p : packets) {
        if (locator.offer(p.data(), p.size())) break;
    }
    if (!locator.locked()) return out;

    for (const auto& p : packets) {
        dsp::forEachEegSample(
            p.data(), p.size(), locator.offset(),
            [&out](const std::uint16_t* adc, int numChannels) {
                Sample s;
                const int n = (numChannels < config::kChannels) ? numChannels
                                                                : config::kChannels;
                for (int ch = 0; ch < n; ++ch) {
                    const auto c = static_cast<std::size_t>(ch);
                    s.adc[c] = adc[ch];
                    s.uv[c]  = dsp::toMicrovolts(dsp::centerSample(adc[ch]));
                }
                out.push_back(s);
            });
    }
    return out;
}

inline LoadResult loadRecording(const std::wstring& path) {
    LoadResult r;

    std::FILE* f = detail::openFile(path, L"rb");
    if (!f) {
        r.error = "file non trovato o non leggibile";
        return r;
    }

    RecordHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1 || !recordHeaderOk(h)) {
        r.error = "il file non e' una registrazione di Mind Zoom, "
                  "oppure e' stato scritto da una versione diversa";
        std::fclose(f);
        return r;
    }

    r.version = h.version;

    if (h.version == 1) {
        // Formato originale: campioni uno dopo l'altro, senza etichetta.
        Sample s;
        while (std::fread(&s, sizeof(s), 1, f) == 1) r.samples.push_back(s);
    } else {
        std::uint8_t tag = 0;
        while (std::fread(&tag, 1, 1, f) == 1) {
            if (tag == static_cast<std::uint8_t>(RecordTag::Sample)) {
                Sample s;
                if (std::fread(&s, sizeof(s), 1, f) != 1) break;
                r.samples.push_back(s);
            } else if (tag == static_cast<std::uint8_t>(RecordTag::RawPacket)) {
                std::uint16_t n = 0;
                if (std::fread(&n, sizeof(n), 1, f) != 1) break;
                std::vector<std::uint8_t> buf(n);
                if (n && std::fread(buf.data(), 1, n, f) != n) break;
                r.packets.push_back(std::move(buf));
            } else {
                break;   // etichetta sconosciuta: si smette invece di indovinare
            }
        }
    }
    std::fclose(f);

    // I pacchetti grezzi hanno la precedenza sui campioni salvati: questi ultimi
    // sono l'uscita del decodificatore di ALLORA, quelli sono ciò che la fascia
    // ha davvero mandato. Se il decodificatore è cambiato - ed è cambiato - la
    // ri-decodifica è più fedele della registrazione.
    if (!r.packets.empty()) {
        auto rifatti = decodeSamples(r.packets);
        if (!rifatti.empty()) {
            r.samples   = std::move(rifatti);
            r.redecoded = true;
        }
    }

    if (r.samples.empty()) {
        if (!r.packets.empty()) {
            // Byte presenti ma nessun campione: la fascia trasmetteva e il
            // decodificatore non riesce a leggerla. E' un difetto del programma,
            // non della sessione, e va detto cosi' - il file resta buono e
            // tornera' utilizzabile appena il decodificatore sara' corretto.
            r.error = "la registrazione contiene " + std::to_string(r.packets.size()) +
                      " notifiche dalla fascia, ma il programma non riesce a "
                      "interpretarle: e' un difetto di decodifica, non un file rovinato";
            return r;
        }
        // Capita per le sessioni aperte senza fascia collegata: il file e' bene
        // formato, semplicemente non contiene niente da rigiocare. Dirlo cosi'
        // evita di far cercare un guasto dove non c'e'.
        r.error = "questa registrazione e' vuota: durante quella sessione non e' "
                  "mai arrivato segnale dalla fascia";
        return r;
    }

    r.ok = true;
    return r;
}

} // namespace mz::ble
