cmake_minimum_required(VERSION 3.20)
project(ffAudio VERSION 3.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(BUILD_SHARED_LIBS "Build ffaudio as a shared library" ON)
option(FFAUDIO_BUILD_TESTS "Build the headless test suite" ON)
option(FFAUDIO_BUILD_MANUAL_PLAYER "Build the interactive manual test player (and, on POSIX, the soak test)" ON)
option(FFAUDIO_INSTALL "Generate install rules" ON)
option(FFAUDIO_CHAOS "Testing only: random delays at the points where threads hand state over (lib/chaos.h), to find races" OFF)
option(FFAUDIO_RELOCATABLE "Linux/macOS: look for dependencies next to libffaudio first (RUNPATH $ORIGIN), for archives that bundle them" OFF)
set(FFAUDIO_SANITIZER "" CACHE STRING "Sanitizer to build with: address, thread, undefined, none or empty")
set(FFAUDIO_TEST_TIMEOUT 180 CACHE STRING "Seconds each playback test may take (chaos runs need more)")
option(FFAUDIO_REQUIRE_TESTS "Fail the configure step when the playback tests can not be built (no ffmpeg or bash), as CI wants" OFF)

# Debug builds default to AddressSanitizer
if(FFAUDIO_SANITIZER STREQUAL "" AND CMAKE_BUILD_TYPE MATCHES "Debug" AND NOT MSVC)
    set(FFAUDIO_SANITIZER "address")
endif()

if(NOT FFAUDIO_SANITIZER STREQUAL "" AND NOT FFAUDIO_SANITIZER STREQUAL "none")
    set(SAN_FLAGS -fsanitize=${FFAUDIO_SANITIZER} -fno-omit-frame-pointer -g)
    if(FFAUDIO_SANITIZER MATCHES "undefined")
        # UBSan only prints by default; abort so the test fails
        list(APPEND SAN_FLAGS -fno-sanitize-recover=undefined)
    endif()
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
        lib/ffaudio_loudness.h
        lib/ffaudio_playlist.h
        lib/ffaudio_meters.h)

add_library(ffaudio
        ${FFAUDIO_PUBLIC_HEADERS}
        lib/ffaudio.c
        lib/play_options.c
        lib/filters.c
        lib/metadata.c
        lib/loudness.c
        lib/gain_cache.c
        lib/gain_cache.h
        lib/gain_scan.c
        lib/network.c
        lib/input.c
        lib/ogg_chain.c
        lib/playlist.c
        lib/text_list.c
        lib/cue.c
        lib/meters.c
        lib/picture.c
        lib/numbers.c
        lib/events.c
        lib/events.h
        lib/numbers.h
        lib/network.h
        lib/once.h
        lib/metadata.h
        lib/track.c
        lib/track.h
        lib/output.c
        lib/output.h
        lib/filtergraph.c
        lib/filtergraph.h
        lib/packet_queue_utils.c
        lib/packet_queue_utils.h
        lib/sample_ring.c
        lib/sample_ring.h
        lib/atomics.h
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
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(ffaudio PRIVATE Threads::Threads) # pthread_once
endif()

target_compile_definitions(ffaudio PRIVATE FFAUDIO_BUILDING)
if(FFAUDIO_CHAOS)
    target_sources(ffaudio PRIVATE lib/chaos.c)
    target_compile_definitions(ffaudio PRIVATE FFAUDIO_CHAOS)
endif()
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(ffaudio PUBLIC FFAUDIO_STATIC)
endif()
target_compile_options(ffaudio PRIVATE ${FFAUDIO_WARNINGS} ${SAN_FLAGS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Nothing reads errno after a math call; this lets the compiler inline lrintf() and friends in the mixer
    target_compile_options(ffaudio PRIVATE -fno-math-errno)
endif()
target_link_options(ffaudio PRIVATE ${SAN_FLAGS})

if(FFAUDIO_RELOCATABLE AND NOT WIN32)
    if(APPLE)
        set_target_properties(ffaudio PROPERTIES INSTALL_RPATH "@loader_path")
    else()
        set_target_properties(ffaudio PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
endif()

set_target_properties(ffaudio PROPERTIES
        C_VISIBILITY_PRESET hidden
        VERSION ${PROJECT_VERSION}
        # The soname follows the major version only: minor releases add to the ABI and never change it
        SOVERSION ${PROJECT_VERSION_MAJOR}
        PUBLIC_HEADER "${FFAUDIO_PUBLIC_HEADERS}")

# 3.0.0 was released with the soname libffaudio.so.3.0: a link by that name lets programs built against it run on
# any 3.x without relinking. Remove in 4.0.
set(FFAUDIO_COMPAT_SONAME_LINK OFF)
if(BUILD_SHARED_LIBS AND UNIX AND NOT APPLE AND PROJECT_VERSION_MAJOR EQUAL 3)
    set(FFAUDIO_COMPAT_SONAME_LINK ON)
    add_custom_command(TARGET ffaudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE_DIR:ffaudio>/libffaudio.so.3.0
            COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_SONAME_FILE_NAME:ffaudio>
                    $<TARGET_FILE_DIR:ffaudio>/libffaudio.so.3.0
            VERBATIM)
endif()

#+-----------------------------------------------------+
#| Install                                             |
#+-----------------------------------------------------+

if(FFAUDIO_INSTALL)
    # A shared libffaudio brings its dependencies along; only static linking needs them listed
    if(BUILD_SHARED_LIBS)
        set(FFAUDIO_PC_CFLAGS "")
        set(FFAUDIO_PC_REQUIRES_PRIVATE "")
    else()
        set(FFAUDIO_PC_CFLAGS "-DFFAUDIO_STATIC")
        set(FFAUDIO_PC_REQUIRES_PRIVATE "Requires.private: sdl2 libavformat libavcodec libavfilter libswresample libavutil")
    endif()
    configure_file(ffaudio.pc.in ${CMAKE_CURRENT_BINARY_DIR}/ffaudio.pc @ONLY)

    install(TARGETS ffaudio
            EXPORT ffaudioTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ffaudio)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ffaudio.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
    if(FFAUDIO_COMPAT_SONAME_LINK)
        install(FILES $<TARGET_FILE_DIR:ffaudio>/libffaudio.so.3.0 DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    install(EXPORT ffaudioTargets NAMESPACE ffaudio:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ffaudio)

    # find_package(ffaudio) -> ffaudio::ffaudio
    include(CMakePackageConfigHelpers)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ffaudioConfig.cmake
         "include(\"\${CMAKE_CURRENT_LIST_DIR}/ffaudioTargets.cmake\")\n")
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ffaudioConfigVersion.cmake
                                     COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ffaudioConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/ffaudioConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ffaudio)
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
    # Plain main(): on Windows SDL.h would otherwise rename it and expect SDL2main to provide WinMain
    target_compile_definitions(${name} PRIVATE SDL_MAIN_HANDLED)
    target_link_options(${name} PRIVATE ${SAN_FLAGS})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

if(FFAUDIO_BUILD_MANUAL_PLAYER)
    ffaudio_test_exe(manual_player tests/manual_player.c)
endif()
if(FFAUDIO_BUILD_MANUAL_PLAYER AND NOT WIN32)
    # Long-running soak test with memory sampling (Linux /proc); not part of CTest
    ffaudio_test_exe(soak tests/soak.c)
    target_link_libraries(soak PRIVATE Threads::Threads)
endif()
if(FFAUDIO_BUILD_MANUAL_PLAYER)
    ffaudio_test_exe(loudness_scan tests/loudness_scan.c)
endif()

if(FFAUDIO_BUILD_TESTS)
    enable_testing()
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    find_program(BASH_EXECUTABLE bash)

    ffaudio_test_exe(test_filters tests/test_filters.c)
    add_test(NAME test_filters COMMAND test_filters ${CMAKE_BINARY_DIR})
    ffaudio_test_exe(test_ring tests/test_ring.c)
    add_test(NAME test_ring COMMAND test_ring)
    # Renders a fixed playlist for before/after comparisons of the output; not a CTest (see the file)
    ffaudio_test_exe(golden_render tests/golden_render.c)

    if(NOT FFMPEG_EXECUTABLE OR NOT BASH_EXECUTABLE)
        if(FFAUDIO_REQUIRE_TESTS)
            message(FATAL_ERROR "ffaudio: ffmpeg or bash not found, the playback tests can not be built")
        endif()
        message(WARNING "ffaudio: ffmpeg or bash not found, playback tests are disabled")
    else()
        set(FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
        add_custom_command(
                OUTPUT ${FIXTURE_DIR}/.stamp
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/make_fixtures.sh ${FIXTURE_DIR}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/make_fixtures.sh
                COMMENT "Generating audio fixtures")
        add_custom_target(ffaudio_fixtures ALL DEPENDS ${FIXTURE_DIR}/.stamp)

        # The ReplayGain cache's default location, inside the build tree: XDG_DATA_HOME on Linux, LOCALAPPDATA on
        # Windows (the tests never touch the user's real one)
        set(FFAUDIO_TEST_DATA_ENV "XDG_DATA_HOME=${CMAKE_BINARY_DIR}/xdg-data")
        if(WIN32)
            string(APPEND FFAUDIO_TEST_DATA_ENV ";LOCALAPPDATA=${CMAKE_BINARY_DIR}/localappdata")
        endif()
        foreach(t test_basic test_lifecycle test_stress test_output test_metadata test_loudness test_gain_cache test_gain_scan test_network test_playlist test_cue)
            ffaudio_test_exe(${t} tests/${t}.c)
            add_dependencies(${t} ffaudio_fixtures)
            add_test(NAME ${t} COMMAND ${t} ${FIXTURE_DIR})
            if(t STREQUAL "test_network" AND WIN32)
                target_link_libraries(${t} PRIVATE ws2_32)   # the test's HTTP server
            endif()
            if(t STREQUAL "test_metadata")
                # Watches FFmpeg's own log messages (av_log_set_callback)
                target_link_libraries(${t} PRIVATE PkgConfig::FFMPEG)
            endif()
            # test_output renders about three minutes of audio in real time: twice the limit
            set(timeout ${FFAUDIO_TEST_TIMEOUT})
            if(t STREQUAL "test_output")
                math(EXPR timeout "${FFAUDIO_TEST_TIMEOUT} * 2")
            endif()
            set_tests_properties(${t} PROPERTIES
                    TIMEOUT ${timeout}
                    ENVIRONMENT "SDL_AUDIODRIVER=dummy;${FFAUDIO_TEST_DATA_ENV};ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
        endforeach()

        if(FFAUDIO_CHAOS)
            # Races of docs/threading.md pinned down: a delay forced at the chaos point that opens each one's window
            ffaudio_test_exe(test_races tests/test_races.c)
            add_dependencies(test_races ffaudio_fixtures)
            set(FFAUDIO_RACES
                    "registration@registration@create_track: worker started=100000"
                    "failed_track_mixer_first@failed_track@eos stored, failure not posted=200000"
                    "failed_track_failure_first@failed_track@sdl_audio_callback=20000"
                    "pause_after_queue@pause_after_queue@cmd_set_next=100000"
                    "play_then_queue@play_then_queue@au_queue=20000|ensure_device=200000")
            foreach(race ${FFAUDIO_RACES})
                string(REPLACE "@" ";" parts "${race}")
                list(GET parts 0 name)
                list(GET parts 1 case)
                list(GET parts 2 point)
                add_test(NAME race_${name} COMMAND test_races ${FIXTURE_DIR} ${case})
                set_tests_properties(race_${name} PROPERTIES
                        TIMEOUT ${FFAUDIO_TEST_TIMEOUT}
                        ENVIRONMENT "SDL_AUDIODRIVER=dummy;${FFAUDIO_TEST_DATA_ENV};FFAUDIO_CHAOS_AT=${point};TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
            endforeach()
        endif()
    endif()
endif()
