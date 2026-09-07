#!/bin/bash
# Mind Zoom - modalità MOSTRA (quella buona, con la fascia vera).
#
# Nessuna opzione: è il comportamento predefinito, ed è quello giusto in mostra.
#   - legge la fascia EEG dal vivo;
#   - nessun pannello diagnostico a schermo: il pubblico vede solo l'immagine
#     (il tasto H lo apre comunque, se serve a chi gestisce la postazione);
#   - registra la sessione da sola, dentro MindZoom.app, senza chiedere niente;
#   - con DUE schermi collegati PRIMA di avviare, il secondo diventa la
#     proiezione a tutto schermo e il primo resta la sala di controllo.
#
# Il percorso si ricava da questo script, così funziona da qualunque cartella.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/MindZoom.app"
cd "$SCRIPT_DIR"

clear
echo "Mind Zoom — MOSTRA"
echo "=================="
echo

if [ ! -d "$APP" ]; then
    echo "MindZoom.app non è in questa cartella (atteso: $APP)."
    read -n 1 -s -r -p "Premi un tasto per chiudere..."
    exit 1
fi

# L'app non è notarizzata: copiata da un'altra macchina macOS la marca in
# quarantena e si rifiuta di aprirla. Toglierlo qui evita di doverlo fare a mano.
xattr -cr "$APP" 2>/dev/null || true

echo "Prima di far entrare la prima persona:"
echo "  1. accendi la fascia Muse e mettila in testa a chi prova;"
echo "  2. aspetta che la pagina d'ingresso dica «Fascia EEG collegata»;"
echo "  3. INVIO per cominciare."
echo
echo "A fine giro: tieni premuto R per due secondi. L'app saluta, ricorda di"
echo "igienizzare la fascia e torna da sola alla pagina d'ingresso, pronta per"
echo "il visitatore dopo."
echo

open "$APP"

echo "Avviata. Puoi chiudere questa finestra."
sleep 3
