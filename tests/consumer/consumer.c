// Includes every public header and calls a function of each, without a player: the installed package is usable
#include <ffaudio.h>
#include <ffaudio_filters.h>
#include <ffaudio_loudness.h>
#include <ffaudio_meters.h>
#include <ffaudio_playlist.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("ffaudio %s\n", au_get_version());
    AuFilterBuilder *fb = au_fb_create();
    if (!fb || au_fb_add_volume_db(fb, NULL, -3.0) != AU_OK) return 1;
    const int filters_ok = strlen(au_fb_peek(fb)) > 0;
    au_fb_free(fb);
    const char *text = "#EXTM3U\n/music/a.flac\n";
    const AuPlaylistFile *pl = au_playlist_parse(text, strlen(text), NULL, NULL);
    const int playlist_ok = pl && pl->count == 1;
    au_playlist_file_release(pl);
    AuSpectrum *sp = au_spectrum_create(1024);
    au_spectrum_free(sp);
    AuLevels lv;
    const int levels = au_get_levels(0.05, &lv); // no player: AU_ERR_NOT_INITIALIZED
    AuLoudness l;
    const int loudness = au_loudness_result(NULL, &l); // no meter: AU_ERR_INVALID_ARGUMENT
    return filters_ok && playlist_ok && sp && levels == AU_ERR_NOT_INITIALIZED && loudness == AU_ERR_INVALID_ARGUMENT
           ? 0 : 1;
}
