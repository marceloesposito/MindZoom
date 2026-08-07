#include "app/displays.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>

namespace mz::app {
namespace {

BOOL CALLBACK collect(HMONITOR handle, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<Display>*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return TRUE;   // salta, non interrompe

    Display d;
    d.handle     = handle;
    d.bounds     = info.rcMonitor;
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

std::FILE* openFile(const std::wstring& path, const wchar_t* mode) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return _wfopen(path.c_str(), mode);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

} // namespace

std::wstring Display::describe() const {
    wchar_t buf[128]{};
    swprintf(buf, 128, L"%dx%d%s", width(), height(),
             primary ? L" (principale)" : L"");
    return buf;
}

std::vector<Display> enumerateDisplays() {
    std::vector<Display> out;
    EnumDisplayMonitors(nullptr, nullptr, collect, reinterpret_cast<LPARAM>(&out));

    // Il primario per primo: e' quello su cui l'utente sta guardando adesso, e
    // quindi il candidato naturale per il pannello dell'operatore.
    std::stable_sort(out.begin(), out.end(),
                     [](const Display& a, const Display& b) { return a.primary > b.primary; });
    return out;
}

std::wstring layoutSignature(const std::vector<Display>& displays) {
    std::wstring sig = std::to_wstring(displays.size());
    for (const auto& d : displays) {
        wchar_t buf[160]{};
        swprintf(buf, 160, L"|%s@%d,%d,%dx%d", d.deviceName.c_str(),
                 static_cast<int>(d.bounds.left), static_cast<int>(d.bounds.top),
                 d.width(), d.height());
        sig += buf;
    }
    return sig;
}

DisplayChoice loadDisplayChoice() {
    DisplayChoice c;

    const auto path = choicePath(false);
    if (path.empty()) return c;

    std::FILE* f = openFile(path, L"rt, ccs=UTF-8");
    if (!f) return c;

    wchar_t line1[512]{}, line2[512]{};
    const bool ok = fgetws(line1, 512, f) && fgetws(line2, 512, f);
    std::fclose(f);
    if (!ok) return c;

    // fgetws tiene il newline: va tolto o il confronto fallisce sempre.
    const auto trim = [](wchar_t* s) {
        std::wstring v = s;
        while (!v.empty() && (v.back() == L'\n' || v.back() == L'\r')) v.pop_back();
        return v;
    };

    c.deviceName = trim(line1);
    c.signature  = trim(line2);
    c.valid      = !c.deviceName.empty();
    return c;
}

void saveDisplayChoice(const std::wstring& deviceName, const std::wstring& signature) {
    const auto path = choicePath(true);
    if (path.empty()) return;

    std::FILE* f = openFile(path, L"wt, ccs=UTF-8");
    if (!f) return;
    std::fwprintf(f, L"%s\n%s\n", deviceName.c_str(), signature.c_str());
    std::fclose(f);
}

int matchStoredChoice(const std::vector<Display>& displays) {
    const auto stored = loadDisplayChoice();
    if (!stored.valid) return -1;

    // Layout diverso da quando la scelta fu fatta: uno schermo aggiunto, tolto o
    // spostato puo' aver cambiato quale sia "il secondo". Meglio richiedere che
    // proiettare sullo schermo sbagliato davanti a qualcuno.
    if (stored.signature != layoutSignature(displays)) return -1;

    for (std::size_t i = 0; i < displays.size(); ++i) {
        if (displays[i].deviceName == stored.deviceName) return static_cast<int>(i);
    }
    return -1;
}

} // namespace mz::app
