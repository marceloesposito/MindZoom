@echo off
REM Produce la cartella distribuibile: dist\MindZoom\ con l'eseguibile e gli
REM asset. Il CRT e' linkato staticamente, quindi non serve installare il
REM redistributable di Visual C++ sulla macchina di destinazione.

setlocal
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo [package] vcvars64.bat non trovato: controlla il percorso dei Build Tools.
    exit /b 1
)

call %VCVARS% >nul 2>&1
cd /d "%~dp0"

cmake -S . -B build-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build-release || exit /b 1

echo.
echo [package] verifica headless della catena grafica...
build-release\MindZoom.exe --selftest
if errorlevel 1 (
    echo [package] self-test FALLITO: pacchetto non prodotto.
    exit /b 1
)

set DIST=dist\MindZoom

REM Le registrazioni delle sessioni vivono dentro la cartella distribuita, ma
REM sono dati dell'utente e non un prodotto della compilazione: vanno messe da
REM parte prima di ricostruire, o il reimpacchettamento le distrugge.
REM E' successo: due sessioni registrate sul campo sono andate perse cosi'.
if exist "%DIST%\registrazioni" (
    if exist dist\_registrazioni_tmp rmdir /s /q dist\_registrazioni_tmp
    move "%DIST%\registrazioni" dist\_registrazioni_tmp >nul
)

if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" 2>nul

if exist dist\_registrazioni_tmp (
    move dist\_registrazioni_tmp "%DIST%\registrazioni" >nul
    echo [package] registrazioni preesistenti conservate.
)

copy /y build-release\MindZoom.exe "%DIST%\" >nul

echo.
echo [package] conversione degli asset in JPEG (niente codec WebP richiesto)...
build-release\mz_convert.exe "..\images" "%DIST%\assets"
if errorlevel 1 (
    echo [package] conversione FALLITA: pacchetto non prodotto.
    exit /b 1
)

copy /y dist-template\LEGGIMI.txt "%DIST%\" >nul

echo.
echo [package] creazione dell'archivio...
REM Le sessioni registrate restano nella cartella di lavoro ma NON entrano
REM nell'archivio: sono letture EEG di una persona, e un archivio si manda in
REM giro. Ci finisce solo la registrazione di esempio. Senza questo filtro
REM l'archivio era passato da 3,7 a 12,8 MB portandosi dietro due sessioni vere.
if exist dist\MindZoom.zip del /q dist\MindZoom.zip
powershell -NoProfile -Command ^
    "$root = 'dist\_zip'; if (Test-Path $root) { Remove-Item $root -Recurse -Force };" ^
    "$stage = Join-Path $root 'MindZoom';" ^
    "New-Item -ItemType Directory -Path $root -Force | Out-Null;" ^
    "Copy-Item '%DIST%' $stage -Recurse;" ^
    "Get-ChildItem (Join-Path $stage 'registrazioni') -Filter 'sessione-*.mzr' -ErrorAction SilentlyContinue | Remove-Item -Force;" ^
    "Compress-Archive -Path $stage -DestinationPath 'dist\MindZoom.zip' -Force;" ^
    "Remove-Item $root -Recurse -Force" || exit /b 1

echo.
echo [package] pronto:
echo   cartella: %CD%\%DIST%
echo   archivio: %CD%\dist\MindZoom.zip
dir /b "%DIST%"
