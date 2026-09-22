// Controparte Windows di crash_posix.cpp: l'ultima riga nel log quando il
// programma muore male.
//
// Su POSIX si intercettano i segnali; qui il meccanismo e' un filtro sulle
// eccezioni strutturate (SEH), che e' cosa diversa ma serve allo stesso scopo -
// l'access violation di Windows e' il SIGSEGV di POSIX.
//
// Due differenze rispetto alla versione POSIX, entrambe volute:
//
//  1. Niente stack simbolico. Su POSIX bastano backtrace/backtrace_symbols_fd,
//     che sono async-signal-safe. Qui l'equivalente sarebbe DbgHelp
//     (StackWalk64 + SymFromAddr), che NON e' rientrante, prende un lock suo e
//     va in stallo proprio nel caso in cui serve - un crash dentro l'allocatore.
//     Si scrivono invece gli indirizzi di ritorno grezzi, che con la mappa del
//     linker (.pdb accanto all'exe) si risolvono a posteriori senza rischiare
//     di perdere il log.
//  2. Si aggiunge l'indirizzo che ha fatto saltare tutto: su Windows e' gia' in
//     mano nel record dell'eccezione, ed e' l'informazione piu' utile del lotto.
//
// La regola resta quella di crash_posix.cpp: dentro l'handler solo chiamate
// sicure. WriteFile lo e'; printf, new e i lock no.

#include "app/crash.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace mz::app::crash {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;

// Due copie dello snapshot e un indice atomico: il thread di render scrive
// quella non in uso e poi pubblica l'indice, cosi' l'handler legge sempre una
// riga intera e mai una meta' vecchia e meta' nuova.
constexpr int kSnapshotSize = 512;
char             g_snapshot[2][kSnapshotSize]{};
std::atomic<int> g_snapshotIndex{0};

void writeStr(const char* s) {
    if (g_file == INVALID_HANDLE_VALUE || !s) return;
    DWORD written = 0;
    WriteFile(g_file, s, static_cast<DWORD>(std::strlen(s)), &written, nullptr);
}

/** Un intero in esadecimale, senza toccare la libreria standard. */
void writeHex(ULONG64 value) {
    char buf[19] = "0x0000000000000000";
    for (int i = 0; i < 16; ++i) {
        const int nibble = static_cast<int>((value >> ((15 - i) * 4)) & 0xF);
        buf[2 + i]       = static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
    }
    writeStr(buf);
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION - accesso a memoria non valida";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT - accesso non allineato";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW - ricorsione senza fondo";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO - divisione intera per zero";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO - divisione in virgola mobile per zero";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION - istruzione non valida";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION - istruzione privilegiata";
        case EXCEPTION_BREAKPOINT:            return "BREAKPOINT - trap di debug";
        default:                              return "eccezione fatale";
    }
}

LONG WINAPI handler(EXCEPTION_POINTERS* info) {
    if (g_file != INVALID_HANDLE_VALUE && info && info->ExceptionRecord) {
        const auto* rec = info->ExceptionRecord;

        writeStr("\n=== [MZ-X01] CRASH: il programma si e' chiuso per un errore interno (");
        writeStr(exceptionName(rec->ExceptionCode));
        writeStr(") ===\n");
        writeStr("Cosa fare: riavvia il programma dal launcher. Se si ripete, conserva "
                 "questo file e le registrazioni della sessione.\n");

        writeStr("Indirizzo: ");
        writeHex(reinterpret_cast<ULONG64>(rec->ExceptionAddress));
        writeStr("\nUltimo stato noto: ");
        const char* snap = g_snapshot[g_snapshotIndex.load(std::memory_order_acquire)];
        writeStr(snap[0] ? snap : "(nessuno)");

        // Gli indirizzi di ritorno, grezzi. Si risolvono dopo, con il .pdb
        // accanto all'eseguibile: vedi il commento in testa al file per il
        // perche' non si usi DbgHelp qui dentro.
        writeStr("\nStack, indirizzi grezzi (per chi sviluppa; risolvere col .pdb):\n");
        void* frames[64];
        const USHORT n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            writeStr("  ");
            writeHex(reinterpret_cast<ULONG64>(frames[i]));
            writeStr("\n");
        }
        writeStr("=== fine ===\n");
        FlushFileBuffers(g_file);
    }
    // Come il raise() della versione POSIX: si lascia che il sistema faccia il
    // suo (report di errore, uscita) invece di inghiottire il crash.
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install(const std::string& logPath) {
    if (logPath.empty()) return;

    // Il log e' gia' stato creato da chi chiama: si apre in append, con
    // condivisione piena perche' il file resta aperto anche dall'altro lato.
    // Il percorso e' UTF-8 e va allargato: contiene il nome utente, che puo'
    // avere accenti.
    const int wide = MultiByteToWideChar(CP_UTF8, 0, logPath.c_str(), -1, nullptr, 0);
    if (wide <= 0) return;
    std::wstring wpath(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, logPath.c_str(), -1, wpath.data(), wide);
    wpath.resize(static_cast<std::size_t>(wide) - 1);   // via il terminatore

    g_file = CreateFileW(wpath.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;   // meglio il default che un handler muto

    SetUnhandledExceptionFilter(handler);
}

void updateSnapshot(const char* line) {
    if (!line) return;
    const int next = 1 - g_snapshotIndex.load(std::memory_order_relaxed);
    std::strncpy(g_snapshot[next], line, kSnapshotSize - 1);
    g_snapshot[next][kSnapshotSize - 1] = '\0';
    g_snapshotIndex.store(next, std::memory_order_release);
}

} // namespace mz::app::crash
