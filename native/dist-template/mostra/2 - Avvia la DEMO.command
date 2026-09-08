#!/bin/bash
# Mind Zoom - modalità DEMO: NON serve la fascia.
#
# Rigioca una sessione registrata davvero con la fascia (sessione-demo.mzr, che
# viaggia dentro il bundle) al posto del segnale dal vivo. Il percorso è identico a
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

# La registrazione della demo viaggia DENTRO il bundle. Se stesse qui accanto
# sarebbe un file della Scrivania, e leggerlo farebbe comparire la richiesta di
# consenso "MindZoom vorrebbe accedere ai file nella cartella Scrivania": una
# finestra da cliccare a mano proprio all'apertura della mostra. Dentro il
# bundle l'app legge solo roba sua, e non chiede niente.
REC="$APP/Contents/Resources/sessione-demo.mzr"
# Una registrazione messa qui accanto ha comunque la precedenza: serve a provare
# un'altra sessione senza toccare il bundle. In quel caso pero' il consenso alla
# cartella torna a essere necessario, perche' quello si' e' un file della
# Scrivania.
[ -f "$SCRIPT_DIR/sessione-demo.mzr" ] && REC="$SCRIPT_DIR/sessione-demo.mzr"
cd "$SCRIPT_DIR"

clear
echo "Mind Zoom — DEMO (senza fascia)"
echo "==============================="
echo

for f in "$APP" "$REC"; do
    if [ ! -e "$f" ]; then
        echo "Manca: $f"
        echo "Serve MindZoom.app in questa cartella, con dentro"
        echo "Contents/Resources/sessione-demo.mzr (oppure una sessione-demo.mzr"
        echo "qui accanto)."
        read -n 1 -s -r -p "Premi un tasto per chiudere..."
        exit 1
    fi
done

# Vedi lo script della mostra: senza questo macOS può rifiutarsi di aprire
# un'app arrivata da un altro Mac.
# Si toglie SOLO la quarantena, non tutti gli attributi estesi. Gli asset e i
# font stanno in Contents/MacOS (stessa convenzione "accanto all'eseguibile"
# del ramo Windows), e li' codesign li tratta come codice annidato: la loro
# firma vive in un attributo esteso, com.apple.cs.CodeDirectory. Un "xattr -cr"
# lo cancellava, e il bundle risultava non firmato - verificato il 07/09/2026:
# dopo un avvio col launcher, "codesign --verify --deep --strict" falliva su
# assets/111.webp. L'app partiva lo stesso, ma senza firma valida TCC puo'
# smettere di attribuire al processo il permesso Bluetooth, cioe' proprio la
# fascia. Se il flag non c'e', xattr non si lamenta.
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
xattr -d com.apple.quarantine "$REC" 2>/dev/null || true

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
