cmake_minimum_required(VERSION 3.20)
project(ffAudio VERSION 4.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# Tests, tools and install rules default to on only when ffaudio is the project being built, not when another project
# includes it with add_subdirectory() or FetchContent
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FFAUDIO_TOP_LEVEL ON)
else()
    set(FFAUDIO_TOP_LEVEL OFF)
endif()
option(FFAUDIO_BUILD_TESTS "Build the headless test suite" ${FFAUDIO_TOP_LEVEL})
option(FFAUDIO_BUILD_TOOLS "Build the manual player, loudness_scan and (on Linux) the soak test" ${FFAUDIO_TOP_LEVEL})
option(FFAUDIO_INSTALL "Generate install rules" ${FFAUDIO_TOP_LEVEL})
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

find_package(SDL2 2.24 REQUIRED)   # SDL_GetDefaultAudioInfo()
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

# Shared only (static builds were dropped in 4.0): one copy of the player's process-wide state, and FFmpeg and SDL
# come along as the library's own dependencies
add_library(ffaudio SHARED
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
# The same name as find_package(ffaudio) gives, for projects that build ffaudio along with themselves
add_library(ffaudio::ffaudio ALIAS ffaudio)

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

#+-----------------------------------------------------+
#| Install                                             |
#+-----------------------------------------------------+

if(FFAUDIO_INSTALL)
    configure_file(ffaudio.pc.in ${CMAKE_CURRENT_BINARY_DIR}/ffaudio.pc @ONLY)

    install(TARGETS ffaudio
            EXPORT ffaudioTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ffaudio)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ffaudio.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
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

if(FFAUDIO_BUILD_TOOLS)
    ffaudio_test_exe(manual_player tests/manual_player.c)
endif()
if(FFAUDIO_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Long-running soak test with memory sampling (Linux /proc and glibc's malloc); not part of CTest
    ffaudio_test_exe(soak tests/soak.c)
    target_link_libraries(soak PRIVATE Threads::Threads)
endif()
if(FFAUDIO_BUILD_TOOLS)
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
        # Made by a test that every playback test requires (a CTest fixture), not by the build: an FFmpeg without an
        # encoder the fixtures need fails the tests, not the build. Building the target again is a no-op.
        add_custom_target(ffaudio_fixtures DEPENDS ${FIXTURE_DIR}/.stamp)
        add_test(NAME ffaudio_fixtures
                 COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ffaudio_fixtures --config $<CONFIG>)
        set_tests_properties(ffaudio_fixtures PROPERTIES FIXTURES_SETUP fixtures TIMEOUT 600)

        # The ReplayGain cache's default location, inside the build tree: XDG_DATA_HOME on Linux, LOCALAPPDATA on
        # Windows, HOME on macOS (the tests never touch the user's real one)
        set(FFAUDIO_TEST_DATA_ENV "XDG_DATA_HOME=${CMAKE_BINARY_DIR}/xdg-data")
        if(WIN32)
            string(APPEND FFAUDIO_TEST_DATA_ENV ";LOCALAPPDATA=${CMAKE_BINARY_DIR}/localappdata")
        elseif(APPLE)
            # ~/Library/Application Support/ffaudio
            string(APPEND FFAUDIO_TEST_DATA_ENV ";HOME=${CMAKE_BINARY_DIR}/home")
        endif()
        foreach(t test_basic test_lifecycle test_stress test_output test_metadata test_loudness test_gain_cache test_gain_scan test_network test_playlist test_cue)
            ffaudio_test_exe(${t} tests/${t}.c)
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
                    FIXTURES_REQUIRED fixtures
                    TIMEOUT ${timeout}
                    ENVIRONMENT "SDL_AUDIODRIVER=dummy;${FFAUDIO_TEST_DATA_ENV};ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
        endforeach()

        # The bindings (Python: bindings/python and the player in player/; C#: bindings/csharp), with the library just
        # built. Not with a sanitizer: an instrumented library can not be loaded into a runtime that is not.
        find_package(Python3 COMPONENTS Interpreter)
        find_program(FFAUDIO_DOTNET dotnet)
        set(FFAUDIO_DOTNET_TFM "")
        if(FFAUDIO_DOTNET)
            # Needs an SDK (bindings/csharp/global.json: 8.0.100 or newer) and a runtime of a framework the tests target
            execute_process(COMMAND ${FFAUDIO_DOTNET} --version WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp
                            RESULT_VARIABLE FFAUDIO_DOTNET_NO_SDK OUTPUT_QUIET ERROR_QUIET)
            execute_process(COMMAND ${FFAUDIO_DOTNET} --list-runtimes OUTPUT_VARIABLE FFAUDIO_DOTNET_RUNTIMES ERROR_QUIET)
            if(NOT FFAUDIO_DOTNET_NO_SDK EQUAL 0)
                message(STATUS "ffaudio: no usable .NET SDK (8.0.100 or newer), the C# bindings' tests are left out")
            elseif(FFAUDIO_DOTNET_RUNTIMES MATCHES "Microsoft.NETCore.App 8\\.")
                set(FFAUDIO_DOTNET_TFM net8.0)
            elseif(FFAUDIO_DOTNET_RUNTIMES MATCHES "Microsoft.NETCore.App 10\\.")
                set(FFAUDIO_DOTNET_TFM net10.0)
            else()
                message(STATUS "ffaudio: no .NET 8 or 10 runtime, the C# bindings' tests are left out")
            endif()
        endif()
        if((Python3_FOUND OR FFAUDIO_DOTNET_TFM) AND (FFAUDIO_SANITIZER STREQUAL "" OR FFAUDIO_SANITIZER STREQUAL "none"))
            # Prints every struct's layout as the C compiler sees it, for the bindings' layout tests
            add_executable(ffaudio_layout bindings/python/tests/layout.c)
            target_include_directories(ffaudio_layout PRIVATE lib)
            set(FFAUDIO_BINDINGS_ENV "FFAUDIO_LIBRARY=$<TARGET_FILE:ffaudio>;FFAUDIO_FIXTURES=${FIXTURE_DIR};SDL_AUDIODRIVER=dummy;${FFAUDIO_TEST_DATA_ENV}")
            if(FFAUDIO_DOTNET_TFM)
                # The tests build into the build tree (--artifacts-path); the first run restores xunit from nuget.org
                add_test(NAME csharp_bindings
                         COMMAND ${FFAUDIO_DOTNET} test ${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp/tests/FFAudioSharp.Tests/FFAudioSharp.Tests.csproj
                                 -c Release -f ${FFAUDIO_DOTNET_TFM} --artifacts-path ${CMAKE_BINARY_DIR}/csharp)
                set_tests_properties(csharp_bindings PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT ${FFAUDIO_TEST_TIMEOUT}
                        ENVIRONMENT "${FFAUDIO_BINDINGS_ENV};FFAUDIO_LAYOUT=$<TARGET_FILE:ffaudio_layout>;FFAUDIO_SYMBOLS=${CMAKE_CURRENT_SOURCE_DIR}/abi/ffaudio.symbols;DOTNET_NOLOGO=1;DOTNET_CLI_TELEMETRY_OPTOUT=1")
            endif()
        endif()
        if(Python3_FOUND AND (FFAUDIO_SANITIZER STREQUAL "" OR FFAUDIO_SANITIZER STREQUAL "none"))
            set(FFAUDIO_PYTHON_ENV "${FFAUDIO_BINDINGS_ENV};PYTHONDONTWRITEBYTECODE=1")
            if(APPLE)
                # HOME is redirected for the tests (the ReplayGain cache above), and Python finds `pip install --user`
                # packages (Textual) through HOME: give it the real user base
                execute_process(COMMAND ${Python3_EXECUTABLE} -m site --user-base OUTPUT_VARIABLE FFAUDIO_PY_USER_BASE
                                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
                if(FFAUDIO_PY_USER_BASE)
                    string(APPEND FFAUDIO_PYTHON_ENV ";PYTHONUSERBASE=${FFAUDIO_PY_USER_BASE}")
                endif()
            endif()
            add_test(NAME python_bindings
                     COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/tests)
            set_tests_properties(python_bindings PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT ${FFAUDIO_TEST_TIMEOUT}
                    ENVIRONMENT "${FFAUDIO_PYTHON_ENV};FFAUDIO_LAYOUT=$<TARGET_FILE:ffaudio_layout>")
            execute_process(COMMAND ${Python3_EXECUTABLE} -c "import textual" RESULT_VARIABLE FFAUDIO_NO_TEXTUAL
                            OUTPUT_QUIET ERROR_QUIET)
            if(FFAUDIO_NO_TEXTUAL EQUAL 0)
                add_test(NAME player
                         COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/player/tests)
                set_tests_properties(player PROPERTIES FIXTURES_REQUIRED fixtures TIMEOUT ${FFAUDIO_TEST_TIMEOUT}
                        ENVIRONMENT "${FFAUDIO_PYTHON_ENV}")
            else()
                message(STATUS "ffaudio: Textual not found for ${Python3_EXECUTABLE}, the player's tests are left out")
            endif()
        endif()

        if(FFAUDIO_CHAOS)
            # Races of docs/threading.md pinned down: a delay forced at the chaos point that opens each one's window
            ffaudio_test_exe(test_races tests/test_races.c)
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
                        FIXTURES_REQUIRED fixtures
                        TIMEOUT ${FFAUDIO_TEST_TIMEOUT}
                        ENVIRONMENT "SDL_AUDIODRIVER=dummy;${FFAUDIO_TEST_DATA_ENV};FFAUDIO_CHAOS_AT=${point};TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
            endforeach()
        endif()
    endif()
endif()
