cmake_minimum_required(VERSION 3.30)
project(ffAudio LANGUAGES C)

set(CMAKE_C_STANDARD 17)


if(CMAKE_BUILD_TYPE MATCHES "Debug")

set(SAN_FLAGS -fsanitize=address)
set(SAN_COMPILE_FLAGS -g -O1 -fno-omit-frame-pointer ${SAN_FLAGS})
set(SAN_LINK_FLAGS ${SAN_FLAGS})

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

add_library(ffaudio SHARED lib/ffaudio.c lib/ffaudio.h
        lib/cmdutils.c
        lib/cmdutils.h
        lib/packet_queue_utils.c
        lib/packet_queue_utils.h
        lib/clock_utils.c
        lib/clock_utils.h
        lib/frame_queue_utils.c
        lib/frame_queue_utils.h
        lib/decoder_utils.c
        lib/decoder_utils.h
        lib/filtergraph.c
        lib/filtergraph.h
        lib/globals.h
        lib/delagates.h)

if(CMAKE_BUILD_TYPE MATCHES "Debug")

target_compile_options(ffaudio PRIVATE ${SAN_COMPILE_FLAGS})
target_link_options(ffaudio PRIVATE ${SAN_LINK_FLAGS})

endif()

target_link_libraries(ffaudio PUBLIC SDL2 PkgConfig::FFMPEG)

# Set properties for Windows DLL export
set_target_properties(ffaudio PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)

# Build the test executable
add_executable(test_ffaudio tests/test.c)

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

if(CMAKE_BUILD_TYPE MATCHES "Debug")

target_compile_options(test_ffaudio PRIVATE ${SAN_COMPILE_FLAGS})
target_link_options(test_ffaudio PRIVATE ${SAN_LINK_FLAGS})

endif()

# Optional: Set output directory for the test binary
set_target_properties(test_ffaudio PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
#[[        INSTALL_RPATH_USE_LINK_PATH TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib"]]
)
