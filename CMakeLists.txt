cmake_minimum_required(VERSION 3.20)
project(openmix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dear ImGui: MIT, so the project stays MIT. Pinned so a build is reproducible.
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# Shared engine: everything that makes audio, with no user interface.
add_library(openmix_core STATIC
    src/loopback.cpp
    src/output.cpp
    src/mic.cpp
    src/usb_audio.cpp
    src/usbip_server.cpp
    src/engine.cpp
    src/config.cpp
    src/selftest.cpp
)
target_compile_definitions(openmix_core PUBLIC
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_include_directories(openmix_core PUBLIC src)
target_link_libraries(openmix_core PUBLIC
    ole32 oleaut32 uuid ksuser winmm avrt mmdevapi ws2_32 shell32 advapi32
)

# The application: a windowed mixer that lives in the tray.
add_executable(openmix WIN32 src/gui.cpp res/openmix.rc)
target_link_libraries(openmix PRIVATE openmix_core imgui dwmapi shell32)

# Console build, kept for debugging and headless use.
add_executable(openmix-cli src/main.cpp)
target_link_libraries(openmix-cli PRIVATE openmix_core)

if (MSVC)
    target_compile_options(openmix       PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(openmix-cli   PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(openmix_core  PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(imgui         PRIVATE /utf-8)
endif()
