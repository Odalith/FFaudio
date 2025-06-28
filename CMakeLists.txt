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

add_executable(ffAudio main.c)

target_link_libraries(ffAudio PRIVATE SDL2 PkgConfig::FFMPEG)