#!/bin/bash
# Mind Zoom - prova del DOPPIO SCHERMO, senza fascia.
#
# Serve a una cosa sola: verificare, prima dell'apertura, che con due schermi
# collegati l'installazione si comporti come deve - la proiezione a tutto
# schermo sul monitor del pubblico, la sala di controllo su quello
# dell'operatore.
#
# Perche' esiste. Il launcher della DEMO passa --schermo-singolo, apposta: e'
# pensato per far vedere l'esperienza su un portatile e basta. Ma quel flag
# spegne la proiezione a monte, quindi con la DEMO il doppio schermo non si puo'
# provare: l'app si comporta come se ci fosse un monitor solo, ed e' un tranello
# in cui si cade una volta ciascuno. Qui il flag NON c'e'.
#
#   --riproduci        segnale da una sessione registrata, niente fascia
#   --senza-log        non registra: la sessione riprodotta non e' di chi guarda
#
# PRIMA DI LANCIARLO, due condizioni che l'app non puo' aggirare:
#   1. il secondo schermo va collegato PRIMA di aprire l'app - la finestra di
#      proiezione si crea solo all'avvio, e collegare un monitor a programma
#      gia' aperto non la fa comparire;
#   2. gli schermi devono essere in "schermo esteso", NON in "duplica": duplicati
#      macOS ne riporta uno solo, e si ricade nel caso a monitor singolo.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/MindZoom.app"
REC="$APP/Contents/Resources/sessione-demo.mzr"
[ -f "$SCRIPT_DIR/sessione-demo.mzr" ] && REC="$SCRIPT_DIR/sessione-demo.mzr"
cd "$SCRIPT_DIR"

clear
echo "Mind Zoom — PROVA DEL DOPPIO SCHERMO (senza fascia)"
echo "==================================================="
echo

for f in "$APP" "$REC"; do
    if [ ! -e "$f" ]; then
        echo "Manca: $f"
        read -n 1 -s -r -p "Premi un tasto per chiudere..."
        exit 1
    fi
done

SCHERMI=$(system_profiler SPDisplaysDataType 2>/dev/null | grep -c "Resolution:" || true)
echo "Schermi visti dal sistema: ${SCHERMI:-?}"
if [ "${SCHERMI:-0}" -lt 2 ]; then
    echo
    echo "Ne serve piu' di uno. Collega il secondo schermo, mettilo in"
    echo "«schermo esteso» (non «duplica») e rilancia questo file."
    echo
    read -n 1 -s -r -p "Premi un tasto per chiudere..."
    exit 1
fi
echo

# Vedi lo script della mostra per il perche' si tolga SOLO la quarantena.
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
xattr -d com.apple.quarantine "$REC" 2>/dev/null || true

echo "Cosa aspettarsi, nell'ordine:"
echo
echo "  1. La PRIMA volta compare «SU QUALE SCHERMO PROIETTARE?», a coprire"
echo "     tutta la finestra dell'operatore. E' un passaggio da fare a mano:"
echo "     frecce per scegliere, INVIO per confermare. La scelta viene"
echo "     ricordata, e la domanda non torna finche' non cambi la"
echo "     disposizione degli schermi."
echo
echo "  2. Confermato: sul monitor del pubblico l'esperienza a tutto schermo,"
echo "     su quello dell'operatore la SALA DI CONTROLLO - segnale elaborato,"
echo "     spettro, indice nel tempo, fps e stato del collegamento."
echo
echo "  Se ti accorgi di aver confermato lo schermo sbagliato (la proiezione"
echo "  copre la sala di controllo), premi P: la domanda torna."
echo
echo "  ESC per uscire."
echo

open "$APP" --args --riproduci "$REC" --senza-log

echo "Avviata. Puoi chiudere questa finestra."
sleep 3
