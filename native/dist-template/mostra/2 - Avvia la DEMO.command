#!/bin/bash
# Mind Zoom - modalità DEMO: NON serve la fascia.
#
# Rigioca una sessione registrata davvero con la fascia (sessione-demo.mzr, in
# questa cartella) al posto del segnale dal vivo. Il percorso è identico a
# quello vero - pagina d'ingresso, accoglienza, esperienza, congedo - ma il
# segnale che guida lo zoom è quello di quella sessione, così si vede come si
# comporta l'installazione senza doverla indossare.
#
#   --riproduci        legge il segnale dal file invece che dalla fascia
#   --schermo-singolo  una finestra sola, anche con un monitor esterno attaccato
#   --senza-log        non registra: la sessione riprodotta non è di chi guarda
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/MindZoom.app"
REC="$SCRIPT_DIR/sessione-demo.mzr"
cd "$SCRIPT_DIR"

clear
echo "Mind Zoom — DEMO (senza fascia)"
echo "==============================="
echo

for f in "$APP" "$REC"; do
    if [ ! -e "$f" ]; then
        echo "Manca: $f"
        echo "MindZoom.app e sessione-demo.mzr devono stare in questa cartella."
        read -n 1 -s -r -p "Premi un tasto per chiudere..."
        exit 1
    fi
done

# Vedi lo script della mostra: senza questo macOS può rifiutarsi di aprire
# un'app arrivata da un altro Mac.
xattr -cr "$APP" 2>/dev/null || true
xattr -c "$REC" 2>/dev/null || true

echo "Cosa aspettarsi:"
echo "  INVIO sulla pagina d'ingresso, poi la schermata di istruzioni."
echo "  Il pulsante «Entra nell'esperienza» si accende dopo qualche secondo:"
echo "  è voluto, serve a dare il tempo di leggere e di respirare."
echo
echo "  Da lì la fotografia si ingrandisce e torna indietro seguendo la"
echo "  concentrazione registrata in quella sessione."
echo
echo "  H apre il pannello con i dati, se interessa vedere cosa legge."
echo "  Tieni premuto R per due secondi per chiudere il giro e ricominciare."
echo

open "$APP" --args --riproduci "$REC" --schermo-singolo --senza-log

echo "Avviata. Puoi chiudere questa finestra."
sleep 3
