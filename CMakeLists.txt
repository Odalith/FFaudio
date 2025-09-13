cmake_minimum_required(VERSION 3.30)
project(ffAudio LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#[[set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize=leak -g -O1")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fsanitize=leak")]]


find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
        libavfilter
)

add_library(ffaudio SHARED ffaudio.c ffaudio.h
        cmdutils.c
        cmdutils.h)

target_link_libraries(ffaudio PUBLIC SDL2 PkgConfig::FFMPEG)

# Set properties for Windows DLL export
set_target_properties(ffaudio PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)

# Build the test executable
add_executable(test_ffaudio test.cpp)

find_library(AVFORMAT_LIB avformat)
find_library(AVCODEC_LIB avcodec)
find_library(AVFILTER_LIB avfilter)
find_library(AVUTIL_LIB avutil)
find_library(SWRESAMPLE_LIB swresample)


# Link the test executable to the library
target_link_libraries(test_ffaudio
        ffaudio
        ${AVFORMAT_LIB}
        ${AVCODEC_LIB}
        ${AVFILTER_LIB}
        ${AVUTIL_LIB}
        ${SWRESAMPLE_LIB}
)

# Optional: Set output directory for the test binary
set_target_properties(test_ffaudio PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
#[[        INSTALL_RPATH_USE_LINK_PATH TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib"]]
)
