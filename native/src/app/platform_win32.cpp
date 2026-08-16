// Le due cose legate a Windows che restano fuori dal codice portabile:
// enumerare i monitor e trovare dove si scrive la scelta.

#include "app/platform.hpp"

#include <windows.h>
#include <shlobj.h>

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

} // namespace mz::app::platform
