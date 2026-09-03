#!/bin/bash
# Avvia (o installa) Mind Zoom da qui. Pensato per girare da una chiavetta
# USB: il punto di mount (/Volumes/NOME) cambia da Mac a Mac, quindi il
# percorso si ricava da questo stesso script invece di essere scritto a mano.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/MindZoom.app"
cd "$SCRIPT_DIR"

clear
echo "Mind Zoom — avvio portable"
echo "=========================="
echo

if [ ! -d "$APP" ]; then
    echo "MindZoom.app non si trova qui accanto (atteso: $APP)."
    echo "Controlla che questo file sia rimasto nella stessa cartella dell'app."
    read -n 1 -s -r -p "Premi un tasto per chiudere..."
    exit 1
fi

# L'app non e' notarizzata (firma ad-hoc, vedi CMakeLists.txt): copiata da
# un'altra macchina o da una chiavetta, macOS puo' rifiutarla con "sviluppatore
# non identificato" o "app danneggiata". Il flag di quarantena e' la causa in
# entrambi i casi; toglierlo qui evita all'utente di doverlo fare a mano da
# Preferenze di Sistema. Se il flag non c'e' xattr non si lamenta.
xattr -cr "$APP" 2>/dev/null || true

echo "Come vuoi usarla?"
echo "  [A] Avvia direttamente da qui - nessuna installazione (predefinito)"
echo "  [I] Installa in /Applications e avvia da li'"
echo
read -r -p "Scelta [A/i]: " CHOICE
echo

case "$CHOICE" in
    [Ii]*)
        DEST="/Applications/MindZoom.app"
        echo "Installazione in /Applications..."
        if ! { rm -rf "$DEST" 2>/dev/null && cp -R "$APP" "$DEST" 2>/dev/null; }; then
            DEST="$HOME/Applications/MindZoom.app"
            echo "Permessi insufficienti su /Applications: installo in $HOME/Applications."
            mkdir -p "$HOME/Applications"
            rm -rf "$DEST"
            cp -R "$APP" "$DEST"
        fi
        xattr -cr "$DEST" 2>/dev/null || true
        echo "Installata in: $DEST"
        echo "La prossima volta si avvia da li' (Launchpad o Applicazioni), senza la chiavetta."
        echo
        echo "Avvio..."
        open "$DEST"
        ;;
    *)
        echo "Avvio da: $APP"
        echo "Le registrazioni e il log di questa sessione restano sulla chiavetta,"
        echo "dentro MindZoom.app - si spostano insieme all'app se la copi altrove."
        open "$APP"
        ;;
esac

echo
echo "Puoi chiudere questa finestra."
sleep 2
