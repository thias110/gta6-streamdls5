cmake_minimum_required(VERSION 3.25)
project(rpenhance LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "rpenhance se compile avec MSVC (Windows SDK + C++/WinRT).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(/utf-8 /EHsc /permissive- /Zc:__cplusplus /MP)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_executable(rpenhance WIN32
    src/main.cpp
    src/util.cpp
    src/config.cpp
    src/app.cpp
    src/selftest.cpp
    src/selftest_tap.cpp
    src/gpu/d3d_context.cpp
    src/gpu/profiler.cpp
    src/capture/wgc_capture.cpp
    src/capture/test_source.cpp
    src/capture/tap_protocol.cpp
    src/capture/tap_source.cpp
    src/capture/capture_logic.cpp
    src/capture/mf_capture.cpp
    src/capture/audio_delay.cpp
    src/capture/audio_sync.cpp
    src/capture/audio_format.cpp
    src/capture/audio_passthrough.cpp
    src/pipeline/compositor.cpp
    src/pipeline/motion.cpp
    src/pipeline/dedupe.cpp
    src/pipeline/scene_cut.cpp
    src/pipeline/upscaler.cpp
    src/pipeline/enhance_logic.cpp
    src/pipeline/enhancer.cpp
    src/pipeline/pipeline.cpp
    src/present/swapchain.cpp
    src/overlay/imgui_overlay.cpp
    src/stats/frame_stats.cpp
    src/nvvfx/nvvfx_backend.cpp
    src/rpenhance.manifest
)

target_include_directories(rpenhance PRIVATE src)
target_compile_options(rpenhance PRIVATE /W4)
target_link_libraries(rpenhance PRIVATE
    imgui_lib
    d3d11 dxgi dxguid d3dcompiler
    dwmapi windowsapp
    mfplat mf mfreadwrite mfuuid  # carte de capture (Media Foundation)
    ole32                         # COM (MMDevice/WASAPI du relais audio, CoTaskMemFree)
)

# --- Backend optionnel NVIDIA Video Effects SDK (VFG / VSR) -----------------------------------------
# OFF par défaut. Avec ON, seuls les EN-TÊTES du VFX SDK Core installé localement sont lus à la
# compilation (types et chaînes de sélecteurs) ; aucune bibliothèque NVIDIA n'est liée : les DLL sont
# chargées à l'exécution (LoadLibrary), l'exe démarre sans elles et se replie sur le pipeline actuel.
# Rien de NVIDIA n'est copié dans le dépôt ni dans build/ : voir scripts/install-nvvfx.ps1 (NGC).
# Le wrapper MIT NVIDIA-Maxine/Maxine-VFX-SDK (v0.7.6) n'est pas utilisé : ses en-têtes ne connaissent
# ni VFG ni les sélecteurs du SDK 1.3 (et nomment autrement certains effets).
option(RPENHANCE_NVVFX "Backend optionnel NVIDIA Video Effects SDK (--interp nvvfx, --upscale nvvfx)" OFF)
if(RPENHANCE_NVVFX)
    if(DEFINED ENV{NVVFX_SDK_DIR})
        set(_nvvfx_default "$ENV{NVVFX_SDK_DIR}")
    elseif(DEFINED ENV{VFXSDK_ROOT})
        set(_nvvfx_default "$ENV{VFXSDK_ROOT}")
    else()
        set(_nvvfx_default "${CMAKE_SOURCE_DIR}/third_party/nvvfx-sdk")
    endif()
    set(NVVFX_SDK_DIR "${_nvvfx_default}" CACHE PATH "Racine du NVIDIA VFX SDK Core installé (non versionné)")

    unset(NVVFX_INCLUDE_DIR CACHE)  # recherché à chaque configuration (NVVFX_SDK_DIR peut changer)
    find_path(NVVFX_INCLUDE_DIR nvVideoEffects.h
        HINTS "${NVVFX_SDK_DIR}" PATH_SUFFIXES include nvvfx/include NO_DEFAULT_PATH)
    if(NOT NVVFX_INCLUDE_DIR)
        message(FATAL_ERROR
            "RPENHANCE_NVVFX=ON mais nvVideoEffects.h est introuvable sous NVVFX_SDK_DIR=${NVVFX_SDK_DIR}.\n"
            "Installe le SDK (scripts/install-nvvfx.ps1, clé NGC dans la variable d'environnement NGC_API_KEY) "
            "ou passe -DNVVFX_SDK_DIR=<racine du SDK>, ou reconfigure avec -DRPENHANCE_NVVFX=OFF.")
    endif()
    set(_nvvfx_includes "${NVVFX_INCLUDE_DIR}")
    # En-têtes des features (nvVFXVideoFrameGeneration.h, nvVFXVideoSuperRes.h) : à côté de
    # nvVideoEffects.h ou sous features/<feature>/ selon l'installation.
    foreach(_hdr nvVFXVideoFrameGeneration.h nvVFXVideoSuperRes.h)
        file(GLOB_RECURSE _found LIST_DIRECTORIES false "${NVVFX_SDK_DIR}/${_hdr}")
        if(_found)
            list(GET _found 0 _first)
            get_filename_component(_dir "${_first}" DIRECTORY)
            list(APPEND _nvvfx_includes "${_dir}")
        else()
            message(WARNING "${_hdr} introuvable sous ${NVVFX_SDK_DIR} : feature non installée ? "
                            "(la compilation échouera si les sélecteurs manquent)")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _nvvfx_includes)
    message(STATUS "rpenhance : backend NVIDIA VFX activé, en-têtes : ${_nvvfx_includes}")

    set_source_files_properties(src/nvvfx/nvvfx_backend.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${_nvvfx_includes}")
    file(TO_CMAKE_PATH "${NVVFX_SDK_DIR}" _nvvfx_dir_def)
    target_compile_definitions(rpenhance PRIVATE
        RPENHANCE_HAVE_NVVFX=1
        RPENHANCE_NVVFX_DEFAULT_DIR="${_nvvfx_dir_def}")
endif()

# Tests de la logique CPU (sans GPU) : ctest -C Release
enable_testing()
add_executable(rpenhance_tests tests/dedupe_test.cpp src/pipeline/dedupe.cpp)
target_include_directories(rpenhance_tests PRIVATE src)
add_test(NAME dedupe COMMAND rpenhance_tests)
add_executable(rpenhance_cut_tests tests/scene_cut_test.cpp src/pipeline/scene_cut.cpp)
target_include_directories(rpenhance_cut_tests PRIVATE src)
add_test(NAME scene_cut COMMAND rpenhance_cut_tests)
add_executable(rpenhance_tap_tests tests/tap_protocol_test.cpp src/capture/tap_protocol.cpp)
target_include_directories(rpenhance_tap_tests PRIVATE src)
add_test(NAME tap_protocol COMMAND rpenhance_tap_tests)
# Carte de capture (sans carte ni Windows) : horodatage MF, choix du mode/format/périphérique, doublons « câble ».
add_executable(rpenhance_capture_tests tests/capture_logic_test.cpp src/capture/capture_logic.cpp src/pipeline/dedupe.cpp)
target_include_directories(rpenhance_capture_tests PRIVATE src)
add_test(NAME capture_logic COMMAND rpenhance_capture_tests)
# Retard du son relayé (--audio-delay) : analyse, cible, lissage, file de trames, simulation du relais.
add_executable(rpenhance_audio_delay_tests tests/audio_delay_test.cpp src/capture/audio_delay.cpp)
target_include_directories(rpenhance_audio_delay_tests PRIVATE src)
add_test(NAME audio_delay COMMAND rpenhance_audio_delay_tests)
add_executable(rpenhance_audio_sync_tests tests/audio_sync_test.cpp src/capture/audio_sync.cpp src/capture/audio_delay.cpp
               src/capture/audio_format.cpp)
target_include_directories(rpenhance_audio_sync_tests PRIVATE src)
add_test(NAME audio_sync COMMAND rpenhance_audio_sync_tests)
# Amélioration d'image (--enhance) : bornes, courbes, lissage, références CPU des filtres (bilatéral, netteté).
add_executable(rpenhance_enhance_tests tests/enhance_test.cpp src/pipeline/enhance_logic.cpp)
target_include_directories(rpenhance_enhance_tests PRIVATE src)
add_test(NAME enhance COMMAND rpenhance_enhance_tests)

# Émetteur de test du frame tap (copie adaptée de l'émetteur MIT de SmoovPlay) : alimente
# `rpenhance --selftest-tap` dans la CI (voir .github/workflows/windows-build.yml). Programme console.
add_executable(tap_writer tests/tap_writer.cpp src/capture/tap_protocol.cpp)
target_include_directories(tap_writer PRIVATE src)
