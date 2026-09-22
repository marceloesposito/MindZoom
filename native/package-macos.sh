#!/bin/bash
# Produce dist/MindZoom-macOS/: MindZoom.app pronto per essere copiato su una
# chiavetta USB (o su Applicazioni), piu' il launcher "Avvia MindZoom.command"
# e il LEGGIMI. Equivalente macOS di package.bat (stessa logica: build Release,
# verifica, conserva le registrazioni gia' esistenti, impacchetta).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD_DIR="build-release-macos"
echo "[package] configurazione..."
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo
echo "[package] compilazione..."
cmake --build "$BUILD_DIR" --target mz_app mz_tests -j "$(sysctl -n hw.ncpu)"

echo
echo "[package] test..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

APP_SRC="$BUILD_DIR/MindZoom.app"
if [ ! -d "$APP_SRC" ]; then
    echo "[package] MindZoom.app non trovato in $BUILD_DIR: pacchetto non prodotto."
    exit 1
fi

DIST="dist/MindZoom-macOS"
APP_DEST="$DIST/MindZoom.app"

# Le registrazioni e il log di debug vivono dentro il bundle gia' distribuito
# (stessa convenzione "accanto all'eseguibile" del ramo Windows, vedi
# platform_macos.mm::exeDirectory): sono dati dell'utente, non un prodotto
# della compilazione, e un reimpacchettamento non deve distruggerli. Vedi
# package.bat per il precedente sul ramo Windows: due sessioni perse cosi'.
STASH="$(mktemp -d)"
if [ -d "$APP_DEST/Contents/MacOS/registrazioni" ]; then
    mv "$APP_DEST/Contents/MacOS/registrazioni" "$STASH/registrazioni"
fi
if [ -d "$APP_DEST/Contents/MacOS/debug" ]; then
    mv "$APP_DEST/Contents/MacOS/debug" "$STASH/debug"
fi

mkdir -p "$DIST"
rm -rf "$APP_DEST"
# ditto invece di cp -R: preserva xattr e resource fork, l'unica copia che non
# rischia di rompere la firma del bundle (vedi `man ditto`).
ditto "$APP_SRC" "$APP_DEST"

if [ -d "$STASH/registrazioni" ]; then
    mv "$STASH/registrazioni" "$APP_DEST/Contents/MacOS/registrazioni"
    echo "[package] registrazioni preesistenti conservate."
fi
if [ -d "$STASH/debug" ]; then
    mv "$STASH/debug" "$APP_DEST/Contents/MacOS/debug"
fi
rm -rf "$STASH"

cp "dist-template/Avvia MindZoom.command" "$DIST/"
chmod +x "$DIST/Avvia MindZoom.command"
cp "dist-template/LEGGIMI-macos.txt" "$DIST/"

echo
echo "[package] rifirma e verifica..."
# Le registrazioni e il debug rimessi qui sopra stanno DENTRO il bundle, e il
# bundle era stato firmato senza di loro: qualunque file aggiunto dopo la firma
# rompe il sigillo, e `codesign --verify` fallisce con "a sealed resource is
# missing or invalid". Non e' un caso raro - succede a ogni reimpacchettamento
# fatto dopo che qualcuno ha usato l'app almeno una volta. Si rifirma quindi il
# bundle come viene spedito, dati compresi, e poi si verifica.
#
# Resta aperta la questione di fondo: tenere i dati dell'utente dentro il .app
# e' comodo (stessa convenzione del ramo Windows) ma litiga con la firma, perche'
# l'app invalida il proprio sigillo ogni volta che scrive una registrazione. La
# via d'uscita e' spostarli in ~/Library/Application Support/MindZoom.
codesign --force --sign - --deep "$APP_DEST"
codesign --verify --deep --strict "$APP_DEST"

echo
echo "[package] creazione dell'archivio (le registrazioni vere non entrano:"
echo "          sono letture EEG di una persona, un archivio si manda in giro;"
echo "          resta solo l'eventuale registrazione di esempio)..."
ZIP_ROOT="$(mktemp -d)"
ZIP_STAGE="$ZIP_ROOT/MindZoom-macOS"
ditto "$DIST" "$ZIP_STAGE"
find "$ZIP_STAGE/MindZoom.app/Contents/MacOS/registrazioni" \
     -name "sessione-*.mzr" -delete 2>/dev/null || true
rm -f dist/MindZoom-macOS.zip
ditto -c -k --sequesterRsrc --keepParent "$ZIP_STAGE" dist/MindZoom-macOS.zip
rm -rf "$ZIP_ROOT"

echo
echo "[package] pronto:"
echo "  cartella:  $(pwd)/$DIST"
echo "  archivio:  $(pwd)/dist/MindZoom-macOS.zip"
ls "$DIST"
