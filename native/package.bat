@echo off
REM Produce la cartella distribuibile: dist\MindZoom\ con l'eseguibile e gli
REM asset. Il CRT e' linkato staticamente, quindi non serve installare il
REM redistributable di Visual C++ sulla macchina di destinazione.

setlocal EnableDelayedExpansion
cd /d "%~dp0"

REM L'ambiente del compilatore si trova da solo, come in build.bat: vswhere sta
REM in un percorso fisso per qualunque Visual Studio dal 2017. Il percorso
REM scritto a mano che c'era qui funzionava su una macchina sola.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    echo [package] Compilatore C++ non trovato. Lancia prima build.bat, che
    echo           spiega cosa installare.
    exit /b 1
)

call "%VCVARS%" >nul 2>&1

where cmake >nul 2>&1
if errorlevel 1 (
    if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "PATH=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
    ) else (
        echo [package] CMake non trovato. Lancia prima build.bat.
        exit /b 1
    )
)

cmake -S . -B build-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if errorlevel 1 (
    echo [package] Ninja non disponibile, ripiego su NMake ^(piu' lento^).
    cmake -S . -B build-release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
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

REM I due font impacchettati vanno accanto all'eseguibile, in fonts\, dove il
REM renderer li registra presso DirectWrite. Senza, l'applicazione parte
REM lo stesso ma il lettering delle intestazioni ripiega sul font di sistema e
REM i titoli escono larghi il doppio: un difetto che si nota solo guardando lo
REM schermo, cioe' quando l'installazione e' gia' montata.
mkdir "%DIST%\fonts" 2>nul
copy /y "assets\fonts\Manrope-Variable.ttf" "%DIST%\fonts\" >nul
if errorlevel 1 (
    echo [package] font Manrope non trovato: pacchetto non prodotto.
    exit /b 1
)
copy /y "assets\fonts\LeagueGothic-Variable.ttf" "%DIST%\fonts\" >nul
if errorlevel 1 (
    echo [package] font League Gothic non trovato: pacchetto non prodotto.
    exit /b 1
)

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
