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

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mz::ble {
namespace detail {

/**
 * Il file è un artefatto locale scritto dal programma stesso: le varianti _s
 * non aggiungono sicurezza qui, chiedono solo di controllare un errno che già
 * si ricava dal puntatore nullo.
 */
inline std::FILE* openFile(const std::wstring& path, const wchar_t* mode) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return _wfopen(path.c_str(), mode);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

} // namespace detail

// I campioni sono banalmente copiabili e il file e' un artefatto locale, quindi
// si scrive lo struct cosi' com'e'. L'intestazione serve a rifiutare un file
// incompatibile invece di interpretarlo a caso: leggere campioni con un layout
// diverso non darebbe un errore, darebbe EEG plausibile ma falso.
inline constexpr char kRecordMagic[8] = {'M', 'Z', 'R', 'E', 'C', '\0', '\0', '\0'};

struct RecordHeader {
    char          magic[8];
    std::uint32_t version;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t sampleBytes;
};

inline RecordHeader makeRecordHeader() {
    RecordHeader h{};
    std::memcpy(h.magic, kRecordMagic, sizeof(kRecordMagic));
    h.version     = 1;
    h.sampleRate  = config::kSampleRate;
    h.channels    = config::kChannels;
    h.sampleBytes = static_cast<std::uint32_t>(sizeof(Sample));
    return h;
}

inline bool recordHeaderOk(const RecordHeader& h) {
    return std::memcmp(h.magic, kRecordMagic, sizeof(kRecordMagic)) == 0 &&
           h.version == 1 &&
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
        if (std::fwrite(&s, sizeof(s), 1, f_) == 1) ++count_;
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
    /** Almeno un campione e' finito su disco: solo allora il file esiste. */
    bool hasData() const noexcept { return count_ > 0; }

    std::uint64_t      count() const noexcept { return count_; }
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
};

/** Esito del caricamento, con il motivo in chiaro quando fallisce. */
struct LoadResult {
    std::vector<Sample> samples;
    bool                ok = false;
    std::string         error;

    double seconds() const {
        return static_cast<double>(samples.size()) / config::kSampleRate;
    }
};

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

    Sample s;
    while (std::fread(&s, sizeof(s), 1, f) == 1) r.samples.push_back(s);
    std::fclose(f);

    if (r.samples.empty()) {
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
