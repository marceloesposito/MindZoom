cmake_minimum_required(VERSION 3.25)

# macOS: si compila per entrambe le architetture, e per un macOS vecchio.
# Vanno impostate PRIMA di project(), perche' CMake le usa gia' nelle prove del
# compilatore; dopo, varrebbero solo per i target e non per la configurazione.
#
# ARCHITECTURES: senza questa riga clang produce solo la fetta della macchina
# che compila (qui arm64), e un Mac Intel rifiuta il bundle con "the application
# cannot be opened because it has an incorrect executable format" - accaduto sul
# Mac di destinazione. Il binario universale contiene entrambe le fette: circa il
# doppio di peso per un eseguibile da mezzo mega, niente rispetto agli asset.
#
# DEPLOYMENT_TARGET: senza, clang incide come minimo il macOS della macchina che
# compila (qui 26.x, e nel bundle spedito c'era "minos 14.5"), e su un Mac piu'
# vecchio l'app non parte comunque - con un secondo errore diverso, dopo aver
# risolto il primo. 11.0 e' il valore che l'Info.plist gia' dichiara in
# LSMinimumSystemVersion, ed e' il primo macOS su cui NSBluetoothAlwaysUsage-
# Description e' obbligatoria: sotto, il BLE non funzionerebbe comunque.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING
        "Architetture del binario macOS (universale per default)")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
        "macOS minimo supportato, da tenere allineato a macos/Info.plist")
endif()

project(MindZoomNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Senza un tipo di build CMake non passa NESSUN flag di ottimizzazione: si
# compila a -O0 anche facendo "cmake --build" senza dire altro, ed e' esattamente
# quello che stava succedendo. Non e' un dettaglio da poco per questo programma -
# lo sciame della pagina d'ingresso e' decine di migliaia di particelle simulate
# a ogni fotogramma, e fra -O0 e -O3 ci passa un fattore vicino al dieci.
# Chi vuole i simboli di debug puo' sempre chiedere -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo di build" FORCE)
endif()
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
    # _USE_MATH_DEFINES: M_PI non e' standard C++, e' un'estensione POSIX. Sui
    # sistemi Unix arriva comunque da <cmath>; su Windows (MSVC come mingw) c'e'
    # solo se si chiede questa macro PRIMA di includere <cmath>, e siccome
    # CMAKE_CXX_EXTENSIONS e' OFF non arriva per altre vie. Senza, non compilano
    # ble/synthetic.hpp e app/experience.cpp - cioe' il segnale sintetico e la
    # logica condivisa, due file che con macOS non davano problemi.
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
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
        "-framework CoreText" "-framework ImageIO"
        # La foto la compone la GPU su CALayer, vedi renderer_macos.mm.
        "-framework QuartzCore")
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
# Le due piattaforme condividono app/experience.cpp (stato, DSP, disegno) e si
# differenziano nel solo shell: finestre, ciclo di messaggi, tasti. Prima non
# era cosi' - src/app/main.cpp teneva logica e shell Win32 nello stesso file, e
# il ramo macOS ne era la copia divergente. main.cpp resta nel repo come
# riferimento storico ma NON viene piu' compilato: vedi ARCHEOLOGIA.md.
if(WIN32)
    # WIN32 = sottosistema grafico, niente finestra di console.
    # shell32: CommandLineToArgvW e SHGetKnownFolderPath.
    add_executable(mz_app WIN32
        src/app/experience.cpp
        src/app/telemetry.cpp
        src/app/crash_win32.cpp
        src/app/shell_win32.cpp
    )
    # comdlg32: la finestra di scelta della registrazione.
    target_link_libraries(mz_app PRIVATE mz_ble mz_render mz_platform shlwapi comdlg32)
    set_target_properties(mz_app PROPERTIES OUTPUT_NAME "MindZoom")

    # Gli asset viaggiano accanto all'eseguibile: l'app li cerca in <exe>\assets.
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/../images"
                "$<TARGET_FILE_DIR:mz_app>/assets"
        COMMENT "Copia delle immagini in assets/")

    # Gli stessi due font del bundle macOS, in <exe>\fonts, dove il renderer li
    # registra presso DirectWrite con GraphicsCore::loadFonts: Manrope per il
    # corpo del testo e League Gothic per il lettering delle intestazioni
    # (identita' Biodetails, vedi design/biodetails/IDENTITA-VISIVA.md).
    #
    # Il terzo font, quello d'accento, su macOS e' Iowan Old Style ed e' di
    # SISTEMA; su Windows non esiste e non e' ridistribuibile, quindi il
    # renderer sceglie il piu' vicino fra quelli presenti (Constantia, poi
    # Georgia) - vedi pickAccentFamily in renderer_win32.cpp. E' l'unico punto
    # in cui le due piattaforme mostrano per forza qualcosa di diverso.
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mz_app>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Manrope-Variable.ttf"
                "$<TARGET_FILE_DIR:mz_app>/fonts/Manrope-Variable.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LeagueGothic-Variable.ttf"
                "$<TARGET_FILE_DIR:mz_app>/fonts/LeagueGothic-Variable.ttf"
        COMMENT "Copia dei font in fonts/")
elseif(APPLE)
    # app/experience.cpp e' l'estrazione portabile della logica di main.cpp
    # (DSP, calibrazione, disegno); vedi il commento in app/experience.hpp per
    # cosa manca ancora rispetto al ramo Windows (proiezione a due schermi).
    add_executable(mz_app MACOSX_BUNDLE
        src/app/experience.cpp
        src/app/telemetry.cpp
        src/app/crash_posix.cpp
        src/app/shell_macos.mm
    )
    target_link_libraries(mz_app PRIVATE mz_ble mz_render mz_platform
        "-framework Cocoa" "-framework CoreVideo")
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

    # Due font non di sistema viaggiano nel bundle, in
    # <bundle>/Contents/MacOS/fonts, dove lo shell li registra con
    # GraphicsCore::loadFonts: Manrope per il corpo del testo e League Gothic
    # per il lettering delle intestazioni (identita' Biodetails, vedi
    # design/biodetails/IDENTITA-VISIVA.md). L'accento (Iowan Old Style) e' di
    # sistema e non va impacchettato. Si copiano i due file che servono, non la
    # cartella: assets/fonts contiene ancora Space Grotesk e Newsreader, di
    # identita' precedenti.
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mz_app>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Manrope-Variable.ttf"
                "$<TARGET_FILE_DIR:mz_app>/fonts/Manrope-Variable.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LeagueGothic-Variable.ttf"
                "$<TARGET_FILE_DIR:mz_app>/fonts/LeagueGothic-Variable.ttf"
        COMMENT "Copia dei font in fonts/")

    # La firma che il linker mette da solo (adhoc,linker-signed) copre solo
    # l'eseguibile nudo, PRIMA che gli step qui sopra riempiano il bundle: non
    # sigilla l'Info.plist ne' le risorse ("Info.plist=not bound" a
    # `codesign -dv`). Senza un Info.plist sigillato TCC non riesce ad
    # attribuire NSBluetoothAlwaysUsageDescription al processo e la richiesta
    # di autorizzazione Bluetooth resta bloccata a meta' invece di comparire -
    # osservato sul campo: CoreBluetooth logga "TCC ... req 1 complete 0" e
    # li' si ferma. Risigillare l'intero bundle dopo che e' completo risolve:
    # e' lo stesso comando che serve per l'impacchettamento verso un secondo
    # Mac, solo eseguito ad ogni build invece che una volta sola.
    #
    # Identita': ad-hoc (-) di default, cosi' chiunque clona il repo (o
    # package-macos.sh per l'impacchettamento) continua a compilare senza
    # bisogno di certificati locali - ed e' quello che va distribuito, visto
    # che un certificato self-signed non e' fidato su un altro Mac piu' di
    # quanto lo sia l'ad-hoc, con lo svantaggio di richiedere di importarlo
    # li' apposta. Il problema dell'ad-hoc e' solo in sviluppo: firma ogni
    # build con un hash diverso, e macOS tratta ogni hash come un'app diversa
    # ai fini di TCC - il permesso Bluetooth va riconcesso a ogni rebuild.
    # Impostando MZ_CODESIGN_IDENTITY (cache var, es.
    # -DMZ_CODESIGN_IDENTITY="MindZoom Local Dev") su un certificato locale
    # stabile importato nel Keychain (Chiave Accesso -> Assistente Certificati
    # -> Crea un certificato -> tipo "Firma del codice", poi fidati per
    # "Firma del codice" nelle sue proprieta'), il permesso sopravvive ai
    # rebuild su QUESTA macchina. Resta una comodita' locale, non va
    # committata come default.
    set(MZ_CODESIGN_IDENTITY "-" CACHE STRING
        "Identita' di firma del bundle macOS (- = ad-hoc, il default per chiunque)")
    add_custom_command(TARGET mz_app POST_BUILD
        COMMAND codesign --force --deep --sign "${MZ_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:mz_app>"
        COMMENT "Firma del bundle con ${MZ_CODESIGN_IDENTITY} (Info.plist e risorse sigillati)")
endif()
