// Le cose legate a Windows che restano fuori dal codice portabile: enumerare i
// monitor, trovare dove si scrive la scelta, e le tre cartelle di cui il
// programma ha bisogno (eseguibile, dati dell'utente, creazione).

#include "app/platform.hpp"

#include <windows.h>
#include <shlobj.h>

#include <vector>

namespace mz::app::platform {
namespace {

BOOL CALLBACK collect(HMONITOR handle, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<Display>*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return TRUE;   // salta, non interrompe

    Display d;
    d.handle     = handle;
    d.bounds     = ScreenRect{info.rcMonitor.left, info.rcMonitor.top,
                              info.rcMonitor.right, info.rcMonitor.bottom};
    d.deviceName = info.szDevice;
    d.primary    = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    out->push_back(std::move(d));
    return TRUE;
}

/** %LOCALAPPDATA%\MindZoom\schermo.txt, con la cartella creata se manca. */
std::wstring choicePath(bool createDir) {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return {};

    std::wstring dir = base;
    CoTaskMemFree(base);
    dir += L"\\MindZoom";

    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\schermo.txt";
}

} // namespace

std::vector<Display> enumerateDisplaysNative() {
    std::vector<Display> out;
    EnumDisplayMonitors(nullptr, nullptr, collect, reinterpret_cast<LPARAM>(&out));
    return out;
}

std::FILE* openChoiceFile(bool forWrite) {
    const auto path = choicePath(forWrite);
    if (path.empty()) return nullptr;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)   // il file e' un artefatto locale nostro
#endif
    return _wfopen(path.c_str(), forWrite ? L"wt" : L"rt");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

std::wstring exeDirectory() {
    // GetModuleFileNameW tronca in silenzio se il buffer e' corto e su Windows
    // non c'e' garanzia che MAX_PATH basti (percorsi lunghi abilitati, cartelle
    // di rete): si raddoppia finche' non ci sta. Il caso in cui serve davvero e'
    // raro, ma il fallimento sarebbe "l'app non trova i suoi assets" con un
    // percorso tagliato a meta', che e' fra i sintomi meno leggibili possibili.
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buf.data(),
                                                 static_cast<DWORD>(buf.size()));
        if (written == 0) return {};
        if (written < buf.size()) {
            std::wstring path(buf.data(), written);
            const auto slash = path.find_last_of(L"\\/");
            return (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash);
        }
        if (buf.size() > 32768) return {};   // oltre il limite di Windows: si rinuncia
        buf.resize(buf.size() * 2);
    }
}

std::wstring dataDirectory() {
    // %LOCALAPPDATA%\MindZoom: la stessa cartella in cui vive schermo.txt, ed e'
    // qui apposta - su Windows il problema che su macOS risolve Application
    // Support (scrivere nel bundle ne rompe la firma) non esiste, ma la
    // distinzione fra "cio' che l'app si porta dietro" e "cio' che scrive" vale
    // lo stesso: l'eseguibile puo' stare in Programmi, dove un utente non
    // amministratore non scrive.
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return {};

    std::wstring dir = base;
    CoTaskMemFree(base);
    dir += L"\\MindZoom";

    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void ensureDirectory(const std::wstring& path) {
    // Una sola CreateDirectoryW non crea i genitori mancanti: si risale finche'
    // serve. SHCreateDirectoryExW farebbe lo stesso in una riga, ma vuole un
    // percorso assoluto e non accetta i percorsi lunghi, quindi tanto vale.
    if (path.empty()) return;
    if (CreateDirectoryW(path.c_str(), nullptr)) return;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) return;   // esiste gia', o errore vero

    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return;
    ensureDirectory(path.substr(0, slash));
    CreateDirectoryW(path.c_str(), nullptr);
}

} // namespace mz::app::platform
