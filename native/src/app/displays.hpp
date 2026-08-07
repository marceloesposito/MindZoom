#pragma once

// Enumerazione dei monitor e memoria della scelta.
//
// La configurazione da installazione vuole due schermi: il partecipante vede
// solo l'immagine, l'operatore vede la telemetria. Con un monitor solo si
// ricade nel comportamento a finestra singola, che e' anche quello con cui si
// sviluppa: e' il percorso che non deve mai rompersi.

#include <windows.h>

#include <string>
#include <vector>

namespace mz::app {

struct Display {
    HMONITOR     handle  = nullptr;
    RECT         bounds{};          // coordinate virtuali del desktop
    std::wstring deviceName;        // \\.\DISPLAY1
    bool         primary = false;

    int width() const noexcept { return bounds.right - bounds.left; }
    int height() const noexcept { return bounds.bottom - bounds.top; }

    /** "1920x1080 (principale)" - quello che serve leggere per riconoscerlo. */
    std::wstring describe() const;
};

/** Tutti i monitor attivi, il primario per primo. */
std::vector<Display> enumerateDisplays();

/**
 * Firma del layout corrente: numero di schermi, risoluzioni e posizioni.
 * Se cambia, la scelta memorizzata non e' piu' affidabile e va richiesta -
 * altrimenti si proietterebbe su uno schermo che non e' piu' quello di prima.
 */
std::wstring layoutSignature(const std::vector<Display>& displays);

/** Scelta memorizzata: nome dispositivo + firma del layout in cui fu fatta. */
struct DisplayChoice {
    std::wstring deviceName;
    std::wstring signature;
    bool         valid = false;
};

DisplayChoice loadDisplayChoice();
void          saveDisplayChoice(const std::wstring& deviceName, const std::wstring& signature);

/**
 * Indice dello schermo memorizzato dentro `displays`, oppure -1 se non c'e'
 * una scelta utilizzabile (mai fatta, layout cambiato, schermo sparito).
 */
int matchStoredChoice(const std::vector<Display>& displays);

} // namespace mz::app
