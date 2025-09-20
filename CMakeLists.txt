cmake_minimum_required(VERSION 3.30)
project(ffAudio LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_BUILD_TYPE MATCHES "Debug")
    # Common sanitizer flags (address, undefined behavior, and leak)
    set(SANITIZE_FLAGS "-fsanitize=address,undefined,leak")

    # Compile options for all languages (C and C++)
    add_compile_options(
            -g
            -O1
            -fno-omit-frame-pointer
            ${SANITIZE_FLAGS}
    )

    # Linker options
    add_link_options(${SANITIZE_FLAGS})
endif()



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
        cmdutils.h
        packet_queue_utils.c
        packet_queue_utils.h
        clock_utils.c
        clock_utils.h
        frame_queue_utils.c
        frame_queue_utils.h
        decoder_utils.c
        decoder_utils.h
        filtergraph.c
        filtergraph.h)

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
