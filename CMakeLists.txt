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
    src/gpu/d3d_context.cpp
    src/gpu/profiler.cpp
    src/capture/wgc_capture.cpp
    src/capture/test_source.cpp
    src/pipeline/compositor.cpp
    src/pipeline/motion.cpp
    src/pipeline/dedupe.cpp
    src/pipeline/upscaler.cpp
    src/pipeline/pipeline.cpp
    src/present/swapchain.cpp
    src/overlay/imgui_overlay.cpp
    src/stats/frame_stats.cpp
    src/rpenhance.manifest
)

target_include_directories(rpenhance PRIVATE src)
target_compile_options(rpenhance PRIVATE /W4)
target_link_libraries(rpenhance PRIVATE
    imgui_lib
    d3d11 dxgi dxguid d3dcompiler
    dwmapi windowsapp
)

# Tests de la logique CPU (sans GPU) : ctest -C Release
enable_testing()
add_executable(rpenhance_tests tests/dedupe_test.cpp src/pipeline/dedupe.cpp)
target_include_directories(rpenhance_tests PRIVATE src)
add_test(NAME dedupe COMMAND rpenhance_tests)
