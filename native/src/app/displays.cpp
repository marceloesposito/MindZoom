#include "app/displays.hpp"
#include "app/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace mz::app {
namespace {

/**
 * Il file della scelta contiene solo roba generata da noi: nomi di dispositivo e
 * firme di layout, tutti ASCII. Queste due conversioni bastano, e non tirano
 * dentro una libreria di codifica per un problema che non abbiamo.
 */
std::wstring widenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::string narrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t c : s) {
        out.push_back((c < 128) ? static_cast<char>(c) : '?');
    }
    return out;
}

std::string trimEol(const char* s) {
    std::string v = s ? s : "";
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return v;
}

} // namespace

std::wstring Display::describe() const {
    wchar_t buf[128]{};
    std::swprintf(buf, 128, L"%dx%d%s", width(), height(),
                  primary ? L" (principale)" : L"");
    return buf;
}

std::vector<Display> enumerateDisplays() {
    auto out = platform::enumerateDisplaysNative();

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
        std::swprintf(buf, 160, L"|%ls@%d,%d,%dx%d", d.deviceName.c_str(),
                      d.bounds.left, d.bounds.top, d.width(), d.height());
        sig += buf;
    }
    return sig;
}

DisplayChoice loadDisplayChoice() {
    DisplayChoice c;

    std::FILE* f = platform::openChoiceFile(false);
    if (!f) return c;

    char line1[512]{}, line2[512]{};
    const bool ok = std::fgets(line1, 512, f) && std::fgets(line2, 512, f);
    std::fclose(f);
    if (!ok) return c;

    // fgets tiene il newline: va tolto o il confronto fallisce sempre.
    c.deviceName = widenAscii(trimEol(line1));
    c.signature  = widenAscii(trimEol(line2));
    c.valid      = !c.deviceName.empty();
    return c;
}

void saveDisplayChoice(const std::wstring& deviceName, const std::wstring& signature) {
    std::FILE* f = platform::openChoiceFile(true);
    if (!f) return;
    std::fprintf(f, "%s\n%s\n", narrowAscii(deviceName).c_str(),
                 narrowAscii(signature).c_str());
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
