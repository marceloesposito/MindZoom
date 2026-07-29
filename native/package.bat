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
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" 2>nul

copy /y build-release\MindZoom.exe "%DIST%\" >nul
xcopy /e /i /y /q build-release\assets "%DIST%\assets" >nul

echo.
echo [package] pronto in %CD%\%DIST%
dir /b "%DIST%"
