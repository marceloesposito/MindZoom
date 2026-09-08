#!/bin/sh
# Compila in croce il ramo Windows da un Mac (o da Linux), con mingw-w64.
#
# Serve a non dover aspettare la macchina Windows per sapere se il codice
# almeno compila. Non sostituisce MSVC - gli header di mingw non sono quelli
# del Windows SDK - ma prende la stragrande maggioranza degli errori, che
# altrimenti si scoprono uno alla volta la prima mattina davanti al PC.
#
# Ha gia' trovato due errori veri che nessuna rilettura aveva visto:
# <objbase.h> mancante (WIN32_LEAN_AND_MEAN toglie le API COM da windows.h) e
# M_PI non definito (e' POSIX, non C++ standard).
#
#   brew install mingw-w64        # macOS
#   apt install g++-mingw-w64     # Debian/Ubuntu
#
# Uso:  ./tools/verifica-windows.sh
#
# COSA NON COPRE: ble/muse.cpp, che usa C++/WinRT ed e' di MSVC. E' l'unico
# file del ramo Windows su cui questo script non dice niente.

set -u

CXX=${CXX_MINGW:-x86_64-w64-mingw32-g++}
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "Compilatore incrociato non trovato: $CXX"
    echo "  macOS:  brew install mingw-w64"
    echo "  Debian: apt install g++-mingw-w64"
    exit 1
fi

cd "$(dirname "$0")/.." || exit 1
SRC=src
SHIM=tools/verifica-windows
OUT=$(mktemp -d) || exit 1
trap 'rm -rf "$OUT"' EXIT

# Le stesse definizioni del CMakeLists per WIN32, o si verifica una
# configurazione diversa da quella che verra' compilata davvero.
FLAGS="-std=c++20 -I$SRC -I$SHIM -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_USE_MATH_DEFINES -Wall -Wextra"

FILES="render/renderer_win32.cpp
app/shell_win32.cpp
app/crash_win32.cpp
app/platform_win32.cpp
app/experience.cpp
app/telemetry.cpp
app/displays.cpp
dsp/stft.cpp
dsp/gating.cpp
control/calibration.cpp
control/zoom.cpp"

echo "=== compilazione ==="
objs=""
fallito=0
for f in $FILES; do
    obj="$OUT/$(basename "$f" .cpp).o"
    printf '  %-28s ' "$(basename "$f")"
    if $CXX $FLAGS -c "$SRC/$f" -o "$obj" 2> "$OUT/err.txt"; then
        echo "ok"
        objs="$objs $obj"
    else
        echo "ERRORI"
        grep -E 'error:' "$OUT/err.txt" | head -8 | sed 's/^/      /'
        fallito=1
    fi
done

[ "$fallito" -eq 1 ] && { echo; echo "Compilazione fallita."; exit 1; }

# Il BLE finto esiste solo per il link: definisce i simboli di MuseClient e
# basta. Su Windows vero quel posto lo prende src/ble/muse.cpp.
echo
echo "=== link (BLE finto) ==="
$CXX $FLAGS -c "$SHIM/muse_finto.cpp" -o "$OUT/muse_finto.o" || exit 1

# -municode: mingw vuole questo per usare wWinMain al posto di WinMain.
# -luuid: le costanti FOLDERID_* di SHGetKnownFolderPath.
if $CXX -municode -mwindows $objs "$OUT/muse_finto.o" \
    -ld2d1 -ldwrite -lwindowscodecs -lshell32 -lole32 -lshlwapi -lcomdlg32 \
    -lgdi32 -luser32 -luuid \
    -static-libgcc -static-libstdc++ -static \
    -o "$OUT/MindZoom.exe" 2> "$OUT/link.txt"; then
    echo "  ok - nessun simbolo non risolto"
else
    echo "  ERRORI"
    grep -oE "undefined reference to .[^'\`]*" "$OUT/link.txt" | sort -u | sed 's/^/      /'
    exit 1
fi

echo
echo "Tutto compila e linka. Restano fuori:"
echo "  - ble/muse.cpp (C++/WinRT, solo MSVC)"
echo "  - le differenze fra gli header di mingw e il Windows SDK vero"
echo "Un errore qui e' quasi certamente vero; l'assenza di errori non e' una"
echo "garanzia, e' un forte indizio."
