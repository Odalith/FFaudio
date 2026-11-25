// The public headers from C++: extern "C", and every macro and inline usable
#include <ffaudio.h>
#include <ffaudio_filters.h>
#include <ffaudio_loudness.h>
#include <ffaudio_meters.h>
#include <ffaudio_playlist.h>
#include <cmath>
#include <cstdio>

int main() {
    std::printf("ffaudio %s (C++)\n", au_get_version());
    const AuReplayGain none = AU_REPLAYGAIN_NONE; // a compound literal in C: not valid C++ before 4.0
    if (!std::isnan(none.track_gain_db)) return 1;
    std::printf("%s\n", au_result_string(AU_ERR_NOT_SAVED));
    AuStatus status;
    au_get_status(&status); // no player: nothing playing
    return status.handle == 0 ? 0 : 1;
}
