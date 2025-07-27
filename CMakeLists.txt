cmake_minimum_required(VERSION 3.30)
project(ffAudio C)

set(CMAKE_C_STANDARD 11)

find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswresample
)

add_library(ffAudio SHARED ffaudio.c ffaudio.h)

target_link_libraries(ffAudio PRIVATE SDL2 PkgConfig::FFMPEG)

# Set properties for Windows DLL export
set_target_properties(ffAudio PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)