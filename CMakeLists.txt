cmake_minimum_required(VERSION 3.20)
project(openmix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(openmix
    src/main.cpp
    src/loopback.cpp
    src/output.cpp
    src/mic.cpp
    src/usb_audio.cpp
    src/usbip_server.cpp
)

target_compile_definitions(openmix PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(openmix PRIVATE ole32 oleaut32 uuid ksuser winmm avrt mmdevapi ws2_32)

if (MSVC)
    target_compile_options(openmix PRIVATE /W4 /permissive- /utf-8)
endif()
