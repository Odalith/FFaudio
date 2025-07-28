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

# Build the test executable
add_executable(test_ffaudio test.c)

find_library(AVFORMAT_LIB avformat)
find_library(AVCODEC_LIB avcodec)
find_library(AVFILTER_LIB avfilter)
find_library(AVUTIL_LIB avutil)
find_library(SWRESAMPLE_LIB swresample)


# Link the test executable to the library
target_link_libraries(test_ffaudio
        ffAudio
        ${AVFORMAT_LIB}
        ${AVCODEC_LIB}
        ${AVFILTER_LIB}
        ${AVUTIL_LIB}
        ${SWRESAMPLE_LIB}
)


# Optional: Set output directory for the test binary
set_target_properties(test_ffaudio PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testbin
)
