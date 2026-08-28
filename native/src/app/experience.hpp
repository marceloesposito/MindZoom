#pragma once

// L'esperienza Mind Zoom, senza niente che appartenga a una piattaforma.
//
// Estratta da app/main.cpp (Windows) per poter avere anche uno shell macOS
// senza duplicare DSP, calibrazione e disegno. ATTENZIONE: main.cpp resta la
// versione usata dal build Windows e non passa da qui - i due sono
// temporaneamente duplicati sulla logica portabile finche' anche il ramo
// Win32 non viene fatto passare da questa classe (non fatto in questa
// sessione: nessuna macchina Windows a disposizione per verificarlo).
//
// Scope di questa prima versione macOS: una sola finestra (operatore e
// partecipante coincidono). La proiezione a due schermi - drawScreenPicker,
// placeProjection - resta solo nel ramo Windows.

#include "app/shared_state.hpp"
#include "app/telemetry.hpp"
#include "control/tunables.hpp"
#include "control/zoom.hpp"
#include "render/renderer.hpp"

#include <string>

namespace mz::app::experience {

enum class Key { Escape, Enter, Up, Down, Left, Right, S, ShiftS, L, R, H, Q, D, B, C, X, V, M, K };

struct StartOptions {
    std::wstring assetsDir;      // cartella con 1..N.webp/.jpg/.png
    std::wstring recordingsDir;  // dove scrivere le sessioni registrate
    bool         record = true;
    std::wstring replayPath;     // se non vuota, riproduce invece di usare la fascia
    // Cartella per il log di diagnostica BLE (un file per sessione, chiuso a
    // ogni stop()). Vuota per disattivarlo - lo scrive solo lo shell che lo
    // valorizza.
    std::wstring debugDir;
};

/**
 * Avvia BLE (o la riproduzione da file) e il thread DSP.
 * @return vuoto se e' andata, altrimenti un messaggio d'errore.
 */
std::string start(const StartOptions& opt);

/** Ferma i thread e chiude la registrazione. Sicuro da chiamare piu' volte. */
void stop();

/** Aggiorna lo stato che dipende dai tasti (taratura dal vivo, HUD, calibrazione). */
void handleKey(Key key);

/** true se l'utente ha chiesto di uscire (ESC o Q). */
bool wantsQuit();

/**
 * Un giro di render: avanza lo zoom/la telemetria di `dt` secondi e disegna
 * tutto nella finestra unica.
 */
void frame(render::Renderer& r, double dt);

} // namespace mz::app::experience
