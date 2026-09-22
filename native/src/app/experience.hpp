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
// Doppio schermo: SCRITTO, MAI ESEGUITO con due monitor veri (stato all'08
// settembre 2026). Il ramo macOS ha tutto - finestra di proiezione senza bordo
// sopra ogni cosa, schermata di scelta del monitor, placeProjection, memoria
// della scelta in schermo.txt invalidata se il layout cambia - e con due
// schermi la finestra principale diventa la sala di controllo (drawControlRoom
// in experience.cpp), rimpicciolita apposta perche' i pixel si pagano a ogni
// fotogramma. Quello che manca e' la PROVA: finora e' stato esercitato solo
// con --proiezione-finestra, cioe' la proiezione simulata in una finestra sullo
// stesso schermo, che non tocca l'enumerazione dei monitor, la scelta, il
// posizionamento ne' il comportamento della finestra senza bordo su un secondo
// pannello. Fino a quella prova, qui non c'e' niente su cui contare.
//
// (Questo commento diceva fino a ieri che la proiezione a due schermi "resta
// solo nel ramo Windows". Era scaduto: il codice c'era gia'. Un commento che
// mente su una parte che sta per andare in mostra costa piu' di uno assente.)

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
 * Scrive una riga nel log di diagnostica della sessione.
 *
 * Serve allo shell di piattaforma per annotare cose che solo lui sa - la
 * geometria della finestra, il refresh dello schermo - accanto agli fps, che
 * senza quei numeri non si sanno interpretare: 23 fps su una finestra piccola
 * e 23 fps su un pannello Retina a tutto schermo sono due diagnosi diverse.
 */
void logLine(const std::string& msg);

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

/**
 * Un fotogramma che attraversa OGNI percorso di disegno, su una finestra mai
 * mostrata: valida la catena grafica senza fascia, senza schermo e senza
 * qualcuno che guardi.
 *
 * Non richiede start(): non tocca il DSP ne' i thread, costruisce da se' lo
 * stato finto che gli serve. E' il controllo che l'impacchettamento esegue
 * prima di produrre l'archivio, e il suo valore e' che tocca TUTTE le
 * primitive del Renderer - lettering, sfumature, sfocature, ritagli a forma
 * libera, archi. Un backend a cui ne manca una qui si vede subito.
 *
 * @return true se il fotogramma e' stato disegnato per intero.
 */
bool selfTest(render::Renderer& r);

} // namespace mz::app::experience
