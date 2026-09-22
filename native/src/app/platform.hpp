#pragma once

// Il poco che resta legato al sistema operativo, dichiarato in un posto solo.
//
// Ogni voce qui dentro e' una cosa che non si puo' scrivere in C++ portabile e
// che serve davvero. La lista e' corta di proposito: piu' corta e' questa,
// meno costa la prossima piattaforma.

#include "app/displays.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace mz::app::platform {

/**
 * Monitor attivi, il primario per primo.
 *
 * Win32: EnumDisplayMonitors + GetMonitorInfoW.
 * macOS: [NSScreen screens], con screens[0] che e' quello con la barra dei menu.
 */
std::vector<Display> enumerateDisplaysNative();

/**
 * Il file in cui si ricorda su quale schermo proiettare.
 *
 * Windows: %LOCALAPPDATA%\MindZoom\schermo.txt
 * macOS:   ~/Library/Application Support/MindZoom/schermo.txt
 *
 * Ritorna il file gia' aperto perche' il PERCORSO non e' rappresentabile in modo
 * portabile: contiene il nome utente, che puo' avere accenti, e le due
 * piattaforme lo codificano diversamente. Il CONTENUTO invece e' ASCII puro
 * (nome dispositivo e firma del layout, generati da noi), quindi da qui in poi
 * si puo' lavorare con char e basta.
 *
 * @return nullptr se non si riesce ad aprirlo: e' un caso normale la prima
 *         volta, e chi chiama deve trattarlo come "nessuna scelta memorizzata".
 */
std::FILE* openChoiceFile(bool forWrite);

/**
 * Cartella dell'eseguibile in corso. Assets e registrazioni vivono accanto ad
 * esso. Usata solo dallo shell macOS: main.cpp (Windows) ha la sua versione
 * locale basata su GetModuleFileNameW.
 */
std::wstring exeDirectory();

/**
 * Cartella dei dati dell'utente: su macOS ~/Library/Application Support/MindZoom,
 * creata se manca. Ci va tutto quello che l'applicazione SCRIVE - registrazioni
 * e log - che e' cosa diversa da quello che si porta dietro (assets e font,
 * che stanno accanto all'eseguibile).
 *
 * La distinzione non e' formale, risolve due problemi veri:
 *
 *  1. Scrivere dentro il proprio bundle ne rompe la FIRMA. Il sigillo copre
 *     tutti i file presenti al momento della firma; il primo log aggiunto dopo
 *     lo invalida, e "codesign --verify" comincia a fallire. Senza firma valida
 *     macOS puo' smettere di attribuire al processo il permesso Bluetooth, che
 *     qui vuol dire la fascia.
 *  2. Se l'app sta sulla Scrivania (o in Documenti, o in Download), scrivere li'
 *     dentro passa dal consenso TCC - la finestra "vorrebbe accedere ai file
 *     nella cartella Scrivania". Application Support non e' protetta: nessun
 *     consenso da chiedere.
 *
 * @return stringa vuota se non si riesce a determinarla; chi chiama deve avere
 *         un ripiego (di solito la cartella dell'eseguibile, come prima).
 */
std::wstring dataDirectory();

/** Crea `path` se non esiste gia'. Non fallisce se esiste gia'. */
void ensureDirectory(const std::wstring& path);

} // namespace mz::app::platform
