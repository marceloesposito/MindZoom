#include "app/crash.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace mz::app::crash {
namespace {

int g_fd = -1;

// Due copie dello snapshot e un indice atomico: il thread di render scrive
// quella non in uso e poi pubblica l'indice, cosi' l'handler legge sempre una
// riga intera e mai una meta' vecchia e meta' nuova.
constexpr int kSnapshotSize = 512;
char             g_snapshot[2][kSnapshotSize]{};
std::atomic<int> g_snapshotIndex{0};

void writeAll(int fd, const char* s, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0) return;
        s += static_cast<std::size_t>(w);
        n -= static_cast<std::size_t>(w);
    }
}

void writeStr(int fd, const char* s) { writeAll(fd, s, std::strlen(s)); }

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV - accesso a memoria non valida";
        case SIGBUS:  return "SIGBUS - accesso a memoria non allineato o non mappato";
        case SIGABRT: return "SIGABRT - interruzione (assert o eccezione non gestita)";
        case SIGFPE:  return "SIGFPE - errore aritmetico";
        case SIGILL:  return "SIGILL - istruzione non valida";
        case SIGTRAP: return "SIGTRAP - trap di debug";
        default:      return "segnale fatale";
    }
}

void handler(int sig) {
    if (g_fd >= 0) {
        writeStr(g_fd, "\n=== [MZ-X01] CRASH: il programma si e' chiuso per un errore interno (");
        writeStr(g_fd, signalName(sig));
        writeStr(g_fd, ") ===\n");
        writeStr(g_fd, "Cosa fare: riavvia il programma dal launcher. Se si ripete, conserva "
                       "questo file e le registrazioni della sessione.\n");
        writeStr(g_fd, "Ultimo stato noto: ");
        const char* snap = g_snapshot[g_snapshotIndex.load(std::memory_order_acquire)];
        writeStr(g_fd, snap[0] ? snap : "(nessuno)");
        writeStr(g_fd, "\nStack (per chi sviluppa):\n");
        void* frames[64];
        const int n = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, n, g_fd);
        writeStr(g_fd, "=== fine ===\n");
        ::fsync(g_fd);
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

void install(const std::string& logPath) {
    if (logPath.empty()) return;
    g_fd = ::open(logPath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (g_fd < 0) return;
    for (const int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGTRAP}) {
        struct sigaction sa{};
        sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND | SA_NODEFER;
        ::sigaction(sig, &sa, nullptr);
    }
}

void updateSnapshot(const char* line) {
    const int next = 1 - g_snapshotIndex.load(std::memory_order_relaxed);
    std::strncpy(g_snapshot[next], line, kSnapshotSize - 1);
    g_snapshot[next][kSnapshotSize - 1] = '\0';
    g_snapshotIndex.store(next, std::memory_order_release);
}

} // namespace mz::app::crash
