@echo off
REM Configura e compila il core nativo, poi lancia i test.
REM Non serve il Developer Command Prompt: vcvars viene chiamato qui.

setlocal
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo [build] vcvars64.bat non trovato: controlla il percorso dei Build Tools.
    exit /b 1
)

call %VCVARS% >nul 2>&1
cd /d "%~dp0"

cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 (
    echo [build] Ninja non disponibile, ripiego su NMake.
    cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)

cmake --build build || exit /b 1
ctest --test-dir build --output-on-failure
