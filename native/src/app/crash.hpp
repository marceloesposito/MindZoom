#pragma once

// Ultima riga nel log quando il programma muore male.
//
// Il log di diagnostica e' gia' scritto riga per riga con flush, quindi
// sopravvive a un crash; ma senza questo pezzo finirebbe a meta' di una riga
// qualsiasi, e chi lo apre non saprebbe se il programma e' stato chiuso o e'
// caduto. Qui si intercettano i segnali fatali (SIGSEGV, SIGBUS, SIGABRT,
// SIGFPE, SIGILL) e si appende in coda, con sole chiamate sicure dentro un
// handler (write, backtrace_symbols_fd): il codice MZ-X01 col suo testo,
// l'ultimo stato noto e lo stack. Poi il segnale viene rilanciato, cosi' il
// sistema fa il suo (crash report, uscita).

#include <string>

namespace mz::app::crash {

/**
 * Installa gli handler e apre `logPath` in append su un descrittore
 * dedicato, tenuto aperto per tutta la vita del processo. Va chiamata una
 * volta, dopo che il log e' stato creato. Se il file non si apre gli handler
 * non vengono installati: meglio il comportamento di default che un handler
 * che non puo' scrivere.
 */
void install(const std::string& logPath);

/**
 * Aggiorna l'"ultimo stato noto" (una riga, senza a-capo) che l'handler
 * copiera' nel log. Da chiamare a intervalli dal thread di render: dev'essere
 * economica e non deve allocare. Le stringhe piu' lunghe di ~500 byte vengono
 * troncate.
 */
void updateSnapshot(const char* line);

} // namespace mz::app::crash
