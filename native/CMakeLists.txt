cmake_minimum_required(VERSION 3.25)
project(MindZoomNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # /utf-8 serve: i commenti e i prompt sono in italiano con accenti.
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Core: DSP + legge di controllo. Nessuna dipendenza esterna, così è compilabile
# e testabile prima di introdurre BLE (WinRT) e rendering (SDL2/vcpkg).
add_library(mz_core STATIC
    src/dsp/stft.cpp
    src/dsp/gating.cpp
    src/control/calibration.cpp
    src/control/zoom.cpp
)
target_include_directories(mz_core PUBLIC src)

enable_testing()

# Test numerici: port dei .test.js, stesse attese sugli stessi valori.
add_executable(mz_tests tests/test_main.cpp)
target_link_libraries(mz_tests PRIVATE mz_core)
add_test(NAME core COMMAND mz_tests)
