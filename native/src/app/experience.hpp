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

#include "app/displays.hpp"
#include "app/shared_state.hpp"
#include "app/telemetry.hpp"
#include "control/tunables.hpp"
#include "control/zoom.hpp"
#include "render/renderer.hpp"

#include <string>
#include <vector>

namespace mz::app::experience {

enum class Key { Escape, Enter, Up, Down, Left, Right, S, ShiftS, L, R, H, Q, D, B, C, X, V, M, K, E, ShiftE, RHold };

struct StartOptions {
    std::wstring assetsDir;      // cartella con 1..N.webp/.jpg/.png
    std::wstring recordingsDir;  // dove scrivere le sessioni registrate
    bool         record = true;
    std::wstring replayPath;     // se non vuota, riproduce invece di usare la fascia
    // Cartella per il log di diagnostica BLE (un file per sessione, chiuso a
    // ogni stop()). Vuota per disattivarlo - lo scrive solo lo shell che lo
    // valorizza.
    std::wstring debugDir;

    // Banda adattiva al posto della calibrazione a due fasi: si entra subito
    // nell'esperienza e gli estremi inseguono i percentili correnti
    // dell'indice (control/adaptive_band.hpp). E' il motivo per cui esiste
    // questo ramo; false ripristina la calibrazione classica, per confronto.
    bool adaptiveBand = true;

    // Livello del pannello operatore all'avvio: 0 nascosto (default: in mostra
    // il pubblico vede solo l'immagine), 1 base (stato, diagnosi, FPS), 2
    // esperto (tutte le letture numeriche e la taratura). Il tasto H cicla.
    int hudLevel = 0;
};

/** Percorso del log di diagnostica della sessione corrente (vuoto se non attivo). */
std::string debugLogPath();

/**
 * Avvia BLE (o la riproduzione da file) e il thread DSP.
 * @return vuoto se e' andata, altrimenti un messaggio d'errore.
 */
std::string start(const StartOptions& opt);

/** Ferma i thread e chiude la registrazione. Sicuro da chiamare piu' volte. */
void stop();

/** Aggiorna lo stato che dipende dai tasti (taratura dal vivo, HUD, calibrazione). */
void handleKey(Key key);

/**
 * Clic del mouse, in coordinate della finestra che disegna l'esperienza
 * (origine in alto a sinistra, punti non pixel: le stesse in cui disegna il
 * Renderer). Serve solo ai due pulsanti che chiedono di proseguire - pagina
 * d'ingresso e accoglienza - perche' chi non conosce l'installazione prova a
 * cliccare prima di cercare INVIO.
 *
 * Ignorato quando l'esperienza sta su un secondo schermo: li' la finestra che
 * riceve il clic e' la sala di controllo, che quei pulsanti non li disegna.
 */
void handleClick(float x, float y);

/**
 * Avanzamento [0,1] della pressione prolungata di R, misurata dallo shell
 * (e' lui che vede i tasti giu' e su) e pubblicata qui per disegnare l'anello
 * attorno al promemoria. 0 = tasto non premuto, l'anello sparisce.
 */
void setRestartHold(double progress);

/** true se l'utente ha chiesto di uscire (ESC o Q). */
bool wantsQuit();

/**
 * Schermo di proiezione (il partecipante), quando lo shell ne ha creato uno
 * secondo. Lo stato di scelta (`displays`/`choosing`/`candidate`) resta di
 * proprieta' dello shell - e' gestione di finestre, non logica dell'esperienza
 * - esattamente come in main.cpp (Windows); qui arriva solo per disegnare.
 */
struct ProjectionState {
    render::Renderer*          renderer  = nullptr;   // nullptr = schermo singolo
    bool                       choosing  = false;      // schermata di scelta in corso
    int                        candidate = -1;         // schermo evidenziato durante la scelta
    const std::vector<Display>* displays = nullptr;    // valido solo se choosing
};

/**
 * Un giro di render: avanza lo zoom/la telemetria di `dt` secondi e disegna
 * nella finestra dell'operatore, e in quella di proiezione se `proj.renderer`
 * non e' nullptr. Un solo avanzamento di stato per entrambe: chiamarla due
 * volte farebbe avanzare lo zoom due volte in un frame.
 */
void frame(render::Renderer& r, double dt, const ProjectionState& proj = {});

} // namespace mz::app::experience
