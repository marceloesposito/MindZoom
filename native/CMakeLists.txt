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
    message(STATUS
        "macOS: core, rendering, schermi e BLE hanno il loro backend. "
        "L'eseguibile no: manca la divisione di src/app/main.cpp fra logica e "
        "shell. Si compilano mz_core, mz_render, mz_platform, mz_ble e i test.")
endif()
