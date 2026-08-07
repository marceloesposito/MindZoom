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

# BLE via C++/WinRT. Nessuna dipendenza esterna: le proiezioni WinRT sono
# nel Windows SDK. windowsapp.lib è l'import library del runtime WinRT.
add_library(mz_ble STATIC src/ble/muse.cpp)
target_link_libraries(mz_ble PUBLIC mz_core windowsapp)

enable_testing()

# Test numerici: port dei .test.js, stesse attese sugli stessi valori.
add_executable(mz_tests tests/test_main.cpp)
target_link_libraries(mz_tests PRIVATE mz_core mz_platform)
add_test(NAME core COMMAND mz_tests)

# Sonda da console: connette il Muse e stampa la diagnostica del DSP nativo,
# nello stesso formato dei log JS. Serve a confrontare i due porti.
add_executable(mz_probe tools/muse_probe.cpp)
target_link_libraries(mz_probe PRIVATE mz_ble)

# Conversione degli asset in JPEG per la distribuzione: toglie la dipendenza dal
# codec WebP, che su Windows 10 può mancare.
add_executable(mz_convert tools/convert_assets.cpp)
target_link_libraries(mz_convert PRIVATE windowscodecs ole32)

# Rendering: Direct2D + DirectWrite + WIC, tutto dal Windows SDK.
add_library(mz_render STATIC src/render/renderer.cpp)
target_link_libraries(mz_render PUBLIC mz_core d2d1 dwrite windowscodecs)

# L'applicazione. WIN32 = sottosistema grafico, niente finestra di console.
# Enumerazione dei monitor e memoria della scelta. In libreria a sé perché i
# test devono poterla esercitare senza tirarsi dietro finestre e Direct2D.
add_library(mz_platform STATIC src/app/displays.cpp)
target_link_libraries(mz_platform PUBLIC mz_core shell32 ole32)

# shell32: CommandLineToArgvW e SHGetKnownFolderPath (opzioni, scelta schermo).
add_executable(mz_app WIN32
    src/app/main.cpp
    src/app/telemetry.cpp
)
target_link_libraries(mz_app PRIVATE mz_ble mz_render mz_platform shlwapi)
set_target_properties(mz_app PROPERTIES OUTPUT_NAME "MindZoom")

# Gli asset viaggiano accanto all'eseguibile: l'app li cerca in <exe>\assets.
add_custom_command(TARGET mz_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../images"
            "$<TARGET_FILE_DIR:mz_app>/assets"
    COMMENT "Copia delle immagini in assets/")
