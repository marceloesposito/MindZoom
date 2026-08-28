cmake_minimum_required(VERSION 3.25)
project(MindZoomNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Runtime CRT statico: l'obiettivo è un .exe che giri su un laptop Windows
# qualsiasi senza installarci sopra il redistributable di Visual C++.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    # /utf-8 serve: i commenti e i prompt sono in italiano con accenti.
    # C4324: il padding da alignas(64) nel ring lock-free è voluto (false sharing).
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /wd4324)
endif()

# UNICODE: le API Win32 devono risolvere alle varianti W (altrimenti IDC_ARROW
# arriva come LPSTR). NOMINMAX: senza, le macro min/max di windows.h rompono
# std::min/std::max.
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra)
endif()

if(APPLE)
    # Senza ARC un CBPeripheral/CBCentralManager assegnato a un campo di una
    # struct C++ (mz::ble::MuseClient::Impl e affini) non viene mai trattenuto:
    # resta vivo per puro caso finche' CoreBluetooth lo tiene nella sua cache
    # interna, e puo' sparire sotto i piedi. Innocuo sui .cpp puri (nessun
    # effetto senza sintassi Objective-C), quindi si applica a tutto il target
    # invece di isolarlo ai soli .mm.
    add_compile_options(-fobjc-arc)
endif()

# Core: DSP + legge di controllo. Deterministico e senza dipendenze, quindi
# testabile per intero senza hardware né finestra.
add_library(mz_core STATIC
    src/dsp/stft.cpp
    src/dsp/gating.cpp
    src/control/calibration.cpp
    src/control/zoom.cpp
)
target_include_directories(mz_core PUBLIC src)

# BLE. L'interfaccia (ble/muse.hpp) e' gia' isolata dietro un PIMPL, quindi il
# backend si sceglie qui senza toccare nient'altro.
# Windows: C++/WinRT, le proiezioni sono nel Windows SDK e windowsapp.lib e'
# l'import library del runtime. macOS: CoreBluetooth.
if(WIN32)
    add_library(mz_ble STATIC src/ble/muse.cpp)
    target_link_libraries(mz_ble PUBLIC mz_core windowsapp)
elseif(APPLE)
    add_library(mz_ble STATIC src/ble/muse_macos.mm)
    target_link_libraries(mz_ble PUBLIC mz_core
        "-framework CoreBluetooth" "-framework Foundation")
endif()

enable_testing()

# Test numerici: port dei .test.js, stesse attese sugli stessi valori.
add_executable(mz_tests tests/test_main.cpp)
target_link_libraries(mz_tests PRIVATE mz_core mz_platform)
add_test(NAME core COMMAND mz_tests)

# Genera una registrazione .mzr sintetica (segnale plausibile, non EEG vero):
# serve a esercitare l'esperienza (--riproduci) senza fascia collegata.
add_executable(mz_gen_test_recording tools/gen_test_recording.cpp)
target_link_libraries(mz_gen_test_recording PRIVATE mz_core)

# La sonda da console e' stata rimossa: chiedeva di imparare un secondo
# programma per ottenere quello che l'applicazione fa gia' da sola. Registra,
# riproduce e mostra la diagnostica MindZoom.exe; le registrazioni si analizzano
# direttamente, il formato e' documentato in src/ble/recording.hpp.

# Conversione degli asset in JPEG per la distribuzione: toglie la dipendenza dal
# codec WebP, che su Windows 10 può mancare. Serve solo a chi impacchetta, e
# solo su Windows: su macOS Image I/O legge il WebP direttamente.
if(WIN32)
    add_executable(mz_convert tools/convert_assets.cpp)
    target_link_libraries(mz_convert PRIVATE windowscodecs ole32)
endif()

# Rendering. L'interfaccia (render/renderer.hpp) non nomina nessuna API di
# sistema: il backend si sceglie qui. Su Windows Direct2D + DirectWrite + WIC,
# su macOS Core Graphics + Core Text + Image I/O.
if(WIN32)
    add_library(mz_render STATIC src/render/renderer_win32.cpp)
    target_link_libraries(mz_render PUBLIC mz_core d2d1 dwrite windowscodecs)
elseif(APPLE)
    add_library(mz_render STATIC src/render/renderer_macos.mm)
    target_link_libraries(mz_render PUBLIC mz_core
        "-framework Cocoa" "-framework CoreGraphics"
        "-framework CoreText" "-framework ImageIO")
endif()

# L'applicazione. WIN32 = sottosistema grafico, niente finestra di console.
# Enumerazione dei monitor e memoria della scelta. In libreria a sé perché i
# test devono poterla esercitare senza tirarsi dietro finestre e Direct2D.
if(WIN32)
    add_library(mz_platform STATIC src/app/displays.cpp src/app/platform_win32.cpp)
    target_link_libraries(mz_platform PUBLIC mz_core shell32 ole32)
elseif(APPLE)
    add_library(mz_platform STATIC src/app/displays.cpp src/app/platform_macos.mm)
    target_link_libraries(mz_platform PUBLIC mz_core "-framework Cocoa")
endif()

# L'applicazione.
#
# ATTENZIONE, unico pezzo ancora non portato: src/app/main.cpp contiene sia la
# logica dell'esperienza (stato, disegno, tasti) sia lo shell Win32 (finestre,
# message loop, dialoghi). Va diviso in app/experience.cpp portabile e
# app/shell_win32.cpp / app/shell_macos.mm prima che il target macOS possa
# esistere. Tutto il resto - core, DSP, controllo, rendering, schermi, BLE - e'
# gia' diviso e ha entrambi i backend.
if(WIN32)
    # WIN32 = sottosistema grafico, niente finestra di console.
    # shell32: CommandLineToArgvW e SHGetKnownFolderPath.
    add_executable(mz_app WIN32
        src/app/main.cpp
        src/app/telemetry.cpp
    )
    # comdlg32: la finestra di scelta della registrazione (tasto P).
    target_link_libraries(mz_app PRIVATE mz_ble mz_render mz_platform shlwapi comdlg32)
    set_target_properties(mz_app PROPERTIES OUTPUT_NAME "MindZoom")

    # Gli asset viaggiano accanto all'eseguibile: l'app li cerca in <exe>\assets.
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/../images"
                "$<TARGET_FILE_DIR:mz_app>/assets"
        COMMENT "Copia delle immagini in assets/")
elseif(APPLE)
    # app/experience.cpp e' l'estrazione portabile della logica di main.cpp
    # (DSP, calibrazione, disegno); vedi il commento in app/experience.hpp per
    # cosa manca ancora rispetto al ramo Windows (proiezione a due schermi).
    add_executable(mz_app MACOSX_BUNDLE
        src/app/experience.cpp
        src/app/telemetry.cpp
        src/app/shell_macos.mm
    )
    target_link_libraries(mz_app PRIVATE mz_ble mz_render mz_platform
        "-framework Cocoa")
    set_target_properties(mz_app PROPERTIES
        OUTPUT_NAME "MindZoom"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")

    # Gli asset viaggiano dentro il bundle, accanto all'eseguibile: lo shell li
    # cerca in <bundle>/Contents/MacOS/assets (stessa convenzione "accanto
    # all'eseguibile" del ramo Windows).
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/../images"
                "$<TARGET_FILE_DIR:mz_app>/assets"
        COMMENT "Copia delle immagini in assets/")

    # Nessun font da impacchettare: l'accento e' Iowan Old Style e il corpo
    # Helvetica Neue, entrambi di sistema su macOS. La copia della cartella
    # assets/fonts serviva a Space Grotesk (e poi a Newsreader), che non si
    # usano piu'; GraphicsCore::loadFonts resta al suo posto per quando
    # servisse di nuovo un carattere non presente su ogni Mac.

    # La firma che il linker mette da solo (adhoc,linker-signed) copre solo
    # l'eseguibile nudo, PRIMA che gli step qui sopra riempiano il bundle: non
    # sigilla l'Info.plist ne' le risorse ("Info.plist=not bound" a
    # `codesign -dv`). Senza un Info.plist sigillato TCC non riesce ad
    # attribuire NSBluetoothAlwaysUsageDescription al processo e la richiesta
    # di autorizzazione Bluetooth resta bloccata a meta' invece di comparire -
    # osservato sul campo: CoreBluetooth logga "TCC ... req 1 complete 0" e
    # li' si ferma. Risigillare l'intero bundle ad-hoc dopo che e' completo
    # risolve: e' lo stesso comando che servira' per l'impacchettamento verso
    # un secondo Mac, solo eseguito ad ogni build invece che una volta sola.
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:mz_app>"
        COMMENT "Firma ad-hoc del bundle (Info.plist e risorse sigillati)")
endif()
