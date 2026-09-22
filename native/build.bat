@echo off
REM Configura e compila il core nativo, poi lancia i test.
REM
REM Non serve il Developer Command Prompt, non serve sapere dove sta Visual
REM Studio, non serve modificare niente: si apre la cartella e si fa doppio
REM clic (o si lancia da cmd). L'ambiente del compilatore lo troviamo qui.
REM
REM Prima questo file conteneva il percorso di vcvars64.bat SCRITTO A MANO, con
REM tanto di numero di versione ("...\Microsoft Visual Studio\18\BuildTools\..."):
REM funzionava sulla macchina di chi l'aveva scritto e su nessun'altra. Basta
REM una versione diversa, l'edizione Community invece dei Build Tools, o
REM l'installazione su un altro disco, e si fermava alla prima riga. Ora si
REM interroga vswhere, che Microsoft installa in un percorso FISSO insieme a
REM qualunque Visual Studio dal 2017 in poi: e' il modo previsto per non dover
REM indovinare.

setlocal EnableDelayedExpansion
cd /d "%~dp0"

REM --- l'ambiente del compilatore ---------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VCVARS="
if exist "%VSWHERE%" (
    REM -latest: la piu' recente. -products *: anche i Build Tools, che senza
    REM questo flag vswhere non elenca. -requires: solo installazioni che hanno
    REM davvero il compilatore C++, non quelle con il solo C#.
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    echo.
    echo [build] Compilatore C++ non trovato.
    echo.
    echo   Serve Visual Studio 2019 o piu' recente ^(anche la versione gratuita^)
    echo   CON il carico di lavoro "Sviluppo di applicazioni desktop con C++".
    echo.
    echo   Da scaricare qui, gratis:
    echo     https://visualstudio.microsoft.com/it/downloads/
    echo   In fondo alla pagina, "Strumenti per Visual Studio" ^> "Build Tools".
    echo.
    echo   In fase di installazione spunta "Sviluppo di applicazioni desktop
    echo   con C++": senza quel carico di lavoro il compilatore non c'e'.
    echo.
    exit /b 1
)

echo [build] compilatore: %VCVARS%
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [build] vcvars64.bat ha fallito: installazione di Visual Studio danneggiata?
    exit /b 1
)

REM --- CMake -------------------------------------------------------------
REM Visual Studio se lo porta dietro, ma non sempre finisce nel PATH: se non
REM c'e', si cerca dove vcvars l'ha messo prima di arrendersi.
where cmake >nul 2>&1
if errorlevel 1 (
    if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "PATH=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
    ) else (
        echo.
        echo [build] CMake non trovato.
        echo   Di solito arriva con Visual Studio: reinstalla spuntando anche
        echo   "Strumenti CMake di C++ per Windows", oppure scaricalo da
        echo   https://cmake.org/download/ e riavvia il prompt.
        echo.
        exit /b 1
    )
)

REM --- configurazione e compilazione -------------------------------------
REM Ninja se c'e' (compila in parallelo, ~3 volte piu' svelto), altrimenti
REM NMake, che con vcvars c'e' sempre.
echo.
echo [build] configurazione...
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=RelWithDebInfo >nul 2>&1
if errorlevel 1 (
    echo [build] Ninja non disponibile, ripiego su NMake ^(piu' lento^).
    cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if errorlevel 1 (
        echo [build] configurazione FALLITA.
        exit /b 1
    )
)

echo.
echo [build] compilazione...
cmake --build build
if errorlevel 1 (
    echo.
    echo [build] compilazione FALLITA.
    exit /b 1
)

REM --- verifiche ---------------------------------------------------------
echo.
echo [build] test numerici...
ctest --test-dir build --output-on-failure
if errorlevel 1 (
    echo.
    echo [build] test FALLITI.
    exit /b 1
)

REM Il selftest disegna un fotogramma completo su una finestra mai mostrata:
REM e' l'unico controllo automatico che tocca Direct2D, DirectWrite e WIC, cioe'
REM proprio la parte che i test numerici non possono vedere.
echo.
echo [build] verifica della catena grafica...
build\MindZoom.exe --selftest
if errorlevel 1 (
    echo.
    echo [build] selftest della grafica FALLITO.
    exit /b 1
)

echo.
echo [build] fatto. L'eseguibile e' in build\MindZoom.exe
echo         Per provarlo senza fascia:  build\MindZoom.exe --pannello
echo         Per il pacchetto da mostra: package.bat
