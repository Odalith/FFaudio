cmake_minimum_required(VERSION 3.20)
project(ffAudio VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(BUILD_SHARED_LIBS "Build ffaudio as a shared library" ON)
option(FFAUDIO_BUILD_TESTS "Build the headless test suite" ON)
option(FFAUDIO_BUILD_MANUAL_PLAYER "Build the interactive manual test player (POSIX only)" ON)
option(FFAUDIO_INSTALL "Generate install rules" ON)
set(FFAUDIO_SANITIZER "" CACHE STRING "Sanitizer to build with: address, thread, undefined, none or empty")

# Debug builds default to AddressSanitizer
if(FFAUDIO_SANITIZER STREQUAL "" AND CMAKE_BUILD_TYPE MATCHES "Debug" AND NOT MSVC)
    set(FFAUDIO_SANITIZER "address")
endif()

if(NOT FFAUDIO_SANITIZER STREQUAL "" AND NOT FFAUDIO_SANITIZER STREQUAL "none")
    set(SAN_FLAGS -fsanitize=${FFAUDIO_SANITIZER} -fno-omit-frame-pointer -g)
    message(STATUS "ffaudio: building with -fsanitize=${FFAUDIO_SANITIZER}")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(FFAUDIO_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
endif()

#+-----------------------------------------------------+
#| Dependencies                                        |
#+-----------------------------------------------------+

find_package(SDL2 REQUIRED)
if(TARGET SDL2::SDL2)
    set(FFAUDIO_SDL2 SDL2::SDL2)
else()
    set(FFAUDIO_SDL2 SDL2)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavutil>=58
        libavcodec>=60
        libavformat>=60
        libswresample>=4
        libavfilter>=9
)

#+-----------------------------------------------------+
#| Library                                             |
#+-----------------------------------------------------+

set(FFAUDIO_PUBLIC_HEADERS
        lib/ffaudio.h
        lib/ffaudio_filters.h
        lib/delegates.h)

add_library(ffaudio
        ${FFAUDIO_PUBLIC_HEADERS}
        lib/ffaudio.c
        lib/filters.c
        lib/metadata.c
        lib/metadata.h
        lib/track.c
        lib/track.h
        lib/output.c
        lib/output.h
        lib/filtergraph.c
        lib/filtergraph.h
        lib/packet_queue_utils.c
        lib/packet_queue_utils.h
        lib/frame_queue_utils.c
        lib/frame_queue_utils.h
        lib/decoder_utils.c
        lib/decoder_utils.h
        lib/globals.h)

target_include_directories(ffaudio PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ffaudio>)

# The public headers include neither SDL nor FFmpeg, so consumers do not need their headers
target_link_libraries(ffaudio PRIVATE ${FFAUDIO_SDL2} PkgConfig::FFMPEG)
if(UNIX)
    target_link_libraries(ffaudio PRIVATE m)
endif()

target_compile_definitions(ffaudio PRIVATE FFAUDIO_BUILDING)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(ffaudio PUBLIC FFAUDIO_STATIC)
endif()
target_compile_options(ffaudio PRIVATE ${FFAUDIO_WARNINGS} ${SAN_FLAGS})
target_link_options(ffaudio PRIVATE ${SAN_FLAGS})

set_target_properties(ffaudio PROPERTIES
        C_VISIBILITY_PRESET hidden
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        PUBLIC_HEADER "${FFAUDIO_PUBLIC_HEADERS}")

#+-----------------------------------------------------+
#| Install                                             |
#+-----------------------------------------------------+

if(FFAUDIO_INSTALL)
    if(BUILD_SHARED_LIBS)
        set(FFAUDIO_PC_CFLAGS "")
    else()
        set(FFAUDIO_PC_CFLAGS "-DFFAUDIO_STATIC")
    endif()
    configure_file(ffaudio.pc.in ${CMAKE_CURRENT_BINARY_DIR}/ffaudio.pc @ONLY)

    install(TARGETS ffaudio
            EXPORT ffaudioTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ffaudio)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ffaudio.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
    install(EXPORT ffaudioTargets NAMESPACE ffaudio:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ffaudio)
endif()

#+-----------------------------------------------------+
#| Tests                                               |
#+-----------------------------------------------------+

function(ffaudio_test_exe name src)
    add_executable(${name} ${src})
    # Tests use SDL directly for timing and synchronisation
    target_link_libraries(${name} PRIVATE ffaudio ${FFAUDIO_SDL2})
    if(UNIX)
        target_link_libraries(${name} PRIVATE m)
    endif()
    target_compile_options(${name} PRIVATE ${SAN_FLAGS})
    target_link_options(${name} PRIVATE ${SAN_FLAGS})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

if(FFAUDIO_BUILD_MANUAL_PLAYER AND NOT WIN32)
    ffaudio_test_exe(manual_player tests/manual_player.c)
endif()

if(FFAUDIO_BUILD_TESTS)
    enable_testing()
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    find_program(BASH_EXECUTABLE bash)

    ffaudio_test_exe(test_filters tests/test_filters.c)
    add_test(NAME test_filters COMMAND test_filters ${CMAKE_BINARY_DIR})

    if(NOT FFMPEG_EXECUTABLE OR NOT BASH_EXECUTABLE)
        message(WARNING "ffaudio: ffmpeg or bash not found, playback tests are disabled")
    else()
        set(FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
        add_custom_command(
                OUTPUT ${FIXTURE_DIR}/.stamp
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/make_fixtures.sh ${FIXTURE_DIR}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/make_fixtures.sh
                COMMENT "Generating audio fixtures")
        add_custom_target(ffaudio_fixtures ALL DEPENDS ${FIXTURE_DIR}/.stamp)

        foreach(t test_basic test_lifecycle test_stress test_output test_metadata)
            ffaudio_test_exe(${t} tests/${t}.c)
            add_dependencies(${t} ffaudio_fixtures)
            add_test(NAME ${t} COMMAND ${t} ${FIXTURE_DIR})
            set_tests_properties(${t} PROPERTIES
                    TIMEOUT 180
                    ENVIRONMENT "SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
        endforeach()
    endif()
endif()
