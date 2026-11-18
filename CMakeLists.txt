cmake_minimum_required(VERSION 3.20)
project(ffAudio VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(FFAUDIO_BUILD_TESTS "Build the headless test suite" ON)
option(FFAUDIO_BUILD_MANUAL_PLAYER "Build the interactive manual test player" ON)
set(FFAUDIO_SANITIZER "" CACHE STRING "Sanitizer to build with: address, thread, undefined or empty")

# Debug builds default to AddressSanitizer, matching the previous behaviour
if(FFAUDIO_SANITIZER STREQUAL "" AND CMAKE_BUILD_TYPE MATCHES "Debug")
    set(FFAUDIO_SANITIZER "address")
endif()

if(NOT FFAUDIO_SANITIZER STREQUAL "" AND NOT FFAUDIO_SANITIZER STREQUAL "none")
    set(SAN_FLAGS -fsanitize=${FFAUDIO_SANITIZER} -fno-omit-frame-pointer -g)
    message(STATUS "ffaudio: building with -fsanitize=${FFAUDIO_SANITIZER}")
endif()

find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavutil
        libavcodec
        libavformat
        libswresample
        libavfilter
)

add_library(ffaudio SHARED
        lib/ffaudio.c
        lib/ffaudio.h
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
        lib/delegates.h)

target_include_directories(ffaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_link_libraries(ffaudio PUBLIC SDL2 PkgConfig::FFMPEG)
target_compile_options(ffaudio PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare ${SAN_FLAGS})
target_link_options(ffaudio PRIVATE ${SAN_FLAGS})

set_target_properties(ffaudio PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

function(ffaudio_test_exe name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ffaudio)
    target_compile_options(${name} PRIVATE ${SAN_FLAGS})
    target_link_options(${name} PRIVATE ${SAN_FLAGS})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

if(FFAUDIO_BUILD_MANUAL_PLAYER)
    ffaudio_test_exe(manual_player tests/manual_player.c)
endif()

if(FFAUDIO_BUILD_TESTS)
    enable_testing()
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    find_program(BASH_EXECUTABLE bash)

    if(NOT FFMPEG_EXECUTABLE OR NOT BASH_EXECUTABLE)
        message(WARNING "ffaudio: ffmpeg or bash not found, tests that need fixtures are disabled")
    else()
        set(FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
        add_custom_command(
                OUTPUT ${FIXTURE_DIR}/.stamp
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/make_fixtures.sh ${FIXTURE_DIR}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/make_fixtures.sh
                COMMENT "Generating audio fixtures")
        add_custom_target(ffaudio_fixtures ALL DEPENDS ${FIXTURE_DIR}/.stamp)

        set(FFAUDIO_TESTS test_basic test_lifecycle test_stress)
        foreach(t ${FFAUDIO_TESTS})
            ffaudio_test_exe(${t} tests/${t}.c)
            add_dependencies(${t} ffaudio_fixtures)
            add_test(NAME ${t} COMMAND ${t} ${FIXTURE_DIR})
            set_tests_properties(${t} PROPERTIES
                    TIMEOUT 180
                    ENVIRONMENT "SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
        endforeach()
    endif()
endif()
