// The public headers from C++: extern "C", and every macro and inline usable
#include <ffaudio.h>
#include <ffaudio_filters.h>
#include <ffaudio_loudness.h>
#include <ffaudio_meters.h>
#include <ffaudio_playlist.h>
#include <cstdio>

int main() {
    std::printf("ffaudio %s (C++)\n", au_get_version());
    AuStatus status;
    au_get_status(&status); // no player: nothing playing
    return status.handle == 0 ? 0 : 1;
}
