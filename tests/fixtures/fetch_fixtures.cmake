# Downloads the pinned test-fixture archive and extracts it into FIXTURE_DIR, then writes the .stamp the
# ffaudio_fixtures target is keyed on. Run by that target at test time (see FFAUDIO_FIXTURES_MODE in
# CMakeLists.txt). The archive is the output of make_fixtures.sh, published as a GitHub release asset
# (tag fixtures-v<N>); to cut a new one, run make_fixtures.sh on a machine whose FFmpeg has the
# libvorbis/libopus/libmp3lame encoders, tar.xz the fixture directory's contents, upload it as the new
# release's asset, and bump FFAUDIO_FIXTURES_VERSION and FFAUDIO_FIXTURES_SHA256 in CMakeLists.txt.
# Inputs (cmake -D): FIXTURE_DIR, FIXTURES_URL, FIXTURES_SHA256.
if(NOT FIXTURE_DIR OR NOT FIXTURES_URL OR NOT FIXTURES_SHA256)
    message(FATAL_ERROR "fetch_fixtures: FIXTURE_DIR, FIXTURES_URL and FIXTURES_SHA256 must all be set")
endif()

set(archive "${FIXTURE_DIR}/.fixtures-download.tar.xz")
file(DOWNLOAD "${FIXTURES_URL}" "${archive}"
     EXPECTED_HASH "SHA256=${FIXTURES_SHA256}"
     STATUS status
     TIMEOUT 300
     TLS_VERIFY ON)
list(GET status 0 status_code)
if(NOT status_code EQUAL 0)
    list(GET status 1 status_message)
    message(FATAL_ERROR "fetch_fixtures: download failed (${status_message}): ${FIXTURES_URL}")
endif()

file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${FIXTURE_DIR}")
file(REMOVE "${archive}")
file(TOUCH "${FIXTURE_DIR}/.stamp")
