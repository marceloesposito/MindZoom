#!/bin/bash
# Avvia la demo di Mind Zoom: nessuna fascia EEG necessaria. Rilegge una
# sessione registrata (registrazione-demo.mzr, qui accanto) al posto del
# segnale dal vivo, e parte senza il pannello diagnostico.
#
# Il percorso si ricava da questo script, cosi' funziona da qualunque cartella
# (Scrivania, chiavetta, Download...). MindZoom.app, registrazione-demo.mzr e
# questo file devono restare nella stessa cartella.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/MindZoom.app"
REC="$SCRIPT_DIR/registrazione-demo.mzr"
cd "$SCRIPT_DIR"

clear
echo "Mind Zoom - demo"
echo "================"
echo

for f in "$APP" "$REC"; do
    if [ ! -e "$f" ]; then
        echo "Manca: $f"
        echo "Controlla che questo file sia rimasto nella stessa cartella dell'app."
        read -n 1 -s -r -p "Premi un tasto per chiudere..."
        exit 1
    fi
done

# L'app non e' notarizzata: copiata da un'altra macchina, macOS la marca in
# quarantena e la rifiuta con "sviluppatore non identificato" o "danneggiata".
# Togliere il flag qui evita di doverlo fare a mano.
xattr -cr "$APP" 2>/dev/null || true
xattr -c "$REC" 2>/dev/null || true

echo "Avvio con la sessione registrata..."
echo
echo "  INVIO   per iniziare l'esperienza"
echo "  H       mostra/nasconde il pannello diagnostico"
echo "  ESC/Q   esce"
echo
open "$APP" --args --riproduci "$REC" --senza-pannello --schermo-singolo

echo "Puoi chiudere questa finestra."
sleep 2
