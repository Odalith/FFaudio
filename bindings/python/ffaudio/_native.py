#
# Copyright (c) 2025 Odalith
#
# ffaudio is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either version 2.1 of the License, or (at your option) any
# later version.
#
"""
The C API one to one: the library, every struct of the five public headers as a ctypes Structure, and the argument
and result types of every function. The Pythonic layer (__init__.py) is built on this; use it directly only for
what that layer does not cover.

Struct layouts must match the headers exactly: tests/test_bindings.py compares every size and offset with what a C
compiler says (tests/layout.c).
"""

import ctypes
import os
import sys
from ctypes import (CFUNCTYPE, POINTER, Structure, Union, c_bool, c_char, c_char_p, c_double, c_float, c_int,
                    c_int32, c_int64, c_size_t, c_uint8, c_void_p)

ABI_MAJOR = 4   # the major version these declarations are for (the soname: libffaudio.so.4)


#+-----------------------------------------------------+
#| Finding the library                                 |
#+-----------------------------------------------------+

def _candidates():
    env = os.environ.get("FFAUDIO_LIBRARY")
    if env:
        yield env
        return
    here = os.path.dirname(os.path.abspath(__file__))
    names = (["libffaudio.dll", "libffaudio-4.dll", "ffaudio.dll"] if sys.platform == "win32" else
             ["libffaudio.4.dylib", "libffaudio.dylib"] if sys.platform == "darwin" else
             ["libffaudio.so.4", "libffaudio.so"])
    # A release archive: <root>/python/ffaudio, the library in <root>/lib (or <root>/bin on Windows)
    for rel in ("../../bin", "../../lib", "../../lib64", ".."):
        for n in names:
            yield os.path.normpath(os.path.join(here, rel, n))
    yield from names   # the system's search path (an installed libffaudio)


def _load():
    errors = []
    for path in _candidates():
        if os.path.isabs(path) and not os.path.exists(path):
            continue
        try:
            if sys.platform == "win32" and os.path.isabs(path):
                # The DLLs it needs (FFmpeg, SDL2, the MinGW runtime) are next to it in a release archive
                os.add_dll_directory(os.path.dirname(path))
            return ctypes.CDLL(path)
        except OSError as e:
            if sys.platform == "win32":
                # A build of your own: its DLLs are on PATH (e.g. MSYS2's ucrt64/bin), which Python 3.8+ leaves
                # out of the search for a DLL's dependencies; winmode=0 is Windows' standard search order
                try:
                    return ctypes.CDLL(path, winmode=0)
                except OSError:
                    pass
            errors.append(f"{path}: {e}")
    raise ImportError("libffaudio (version 4) not found; set FFAUDIO_LIBRARY to its path\n  " + "\n  ".join(errors))


lib = _load()


#+-----------------------------------------------------+
#| Structs (ffaudio.h)                                 |
#+-----------------------------------------------------+

class EvLog(Structure):
    _fields_ = [("level", c_int), ("message", c_char_p), ("request", c_int64)]


class EvDevice(Structure):
    _fields_ = [("ok", c_bool), ("fell_back", c_bool), ("name", c_char_p), ("sample_rate", c_int32),
                ("channels", c_int32)]


class EvOpened(Structure):
    _fields_ = [("duration", c_double), ("live", c_bool), ("seekable", c_bool)]


class EvEnded(Structure):
    _fields_ = [("reason", c_int), ("error", c_int32), ("message", c_char_p)]


class EvRestarted(Structure):
    _fields_ = [("position", c_double), ("from_loop", c_bool), ("loops_remaining", c_int32)]


class EvDuration(Structure):
    _fields_ = [("seconds", c_double)]


class EvBuffering(Structure):
    _fields_ = [("active", c_bool), ("buffered_seconds", c_double)]


class EvMetadata(Structure):
    _fields_ = [("title", c_char_p), ("artist", c_char_p)]


class EvOverflow(Structure):
    _fields_ = [("dropped", c_int32)]


class EvData(Union):
    _fields_ = [("log", EvLog), ("device", EvDevice), ("opened", EvOpened), ("ended", EvEnded),
                ("restarted", EvRestarted), ("duration", EvDuration), ("buffering", EvBuffering),
                ("metadata", EvMetadata), ("overflow", EvOverflow), ("padding", c_uint8 * 64)]


class AuEvent(Structure):
    _fields_ = [("type", c_int), ("handle", c_int32), ("data", EvData)]


AuEventFn = CFUNCTYPE(None, POINTER(AuEvent), c_void_p)
AuFadeCurveFn = CFUNCTYPE(c_float, c_float, c_void_p)


class AuFade(Structure):
    _fields_ = [("seconds", c_double), ("curve", c_int), ("custom", AuFadeCurveFn), ("custom_user", c_void_p)]


class AuCrossfade(Structure):
    _fields_ = [("on_track_end", AuFade), ("on_skip", AuFade), ("on_play", AuFade), ("on_stop", AuFade),
                ("on_pause", AuFade), ("min_track_seconds", c_double)]


class AuReplayGain(Structure):
    _fields_ = [("track_gain_db", c_float), ("track_peak", c_float), ("album_gain_db", c_float),
                ("album_peak", c_float), ("from_r128", c_bool)]


class AuReplayGainSettings(Structure):
    _fields_ = [("mode", c_int), ("preamp_db", c_double), ("fallback_gain_db", c_double),
                ("prevent_clipping", c_bool), ("measure_missing", c_bool), ("measure_wait_seconds", c_double)]


class AuNetworkSettings(Structure):
    _fields_ = [("user_agent", c_char_p), ("timeout_seconds", c_double), ("buffer_seconds", c_double),
                ("max_buffer_seconds", c_double), ("reconnect_attempts", c_int)]


class AuTag(Structure):
    _fields_ = [("key", c_char_p), ("value", c_char_p)]


class AuMediaInfo(Structure):
    _fields_ = [("path", c_char_p), ("container", c_char_p), ("codec", c_char_p), ("channel_layout", c_char_p),
                ("duration", c_double), ("bit_rate", c_int64), ("sample_rate", c_int), ("channels", c_int),
                ("bits_per_sample", c_int), ("live", c_bool), ("seekable", c_bool), ("track_number", c_int),
                ("track_total", c_int), ("disc_number", c_int), ("disc_total", c_int),
                ("replaygain", AuReplayGain), ("tag_count", c_int), ("tags", POINTER(AuTag)),
                ("picture_count", c_int)]


class AuPicture(Structure):
    _fields_ = [("data", POINTER(c_uint8)), ("size", c_size_t), ("mime", c_char_p), ("type", c_int),
                ("width", c_int), ("height", c_int), ("description", c_char_p)]


class AuStatus(Structure):
    _fields_ = [("handle", c_int32), ("position", c_double), ("duration", c_double), ("buffering", c_bool),
                ("paused", c_bool), ("muted", c_bool), ("volume", c_float), ("speed", c_double),
                ("keep_pitch", c_bool), ("loop_count", c_int), ("sample_rate", c_int), ("channels", c_int)]


#+-----------------------------------------------------+
#| Structs (ffaudio_filters.h)                         |
#+-----------------------------------------------------+

EQ_BANDS = 10
PEQ_MAX_BANDS = 32


class AuEqualizer(Structure):
    _fields_ = [("gain_db", c_double * EQ_BANDS), ("frequency_hz", c_double * EQ_BANDS),
                ("width_hz", c_double * EQ_BANDS), ("preamp_db", c_double)]


class AuPeqBand(Structure):
    _fields_ = [("type", c_int), ("frequency_hz", c_double), ("q", c_double), ("gain_db", c_double)]


class AuParametricEq(Structure):
    _fields_ = [("preamp_db", c_double), ("band_count", c_int), ("bands", AuPeqBand * PEQ_MAX_BANDS)]


class AuStereo(Structure):
    _fields_ = [("balance", c_double), ("width", c_double), ("swap", c_bool)]


class AuLimiter(Structure):
    _fields_ = [("ceiling_db", c_double), ("attack_ms", c_double), ("release_ms", c_double)]


class AuCompressor(Structure):
    _fields_ = [("threshold_db", c_double), ("ratio", c_double), ("attack_ms", c_double), ("release_ms", c_double),
                ("knee_db", c_double), ("makeup_db", c_double)]


class AuDeclick(Structure):
    _fields_ = [("window_ms", c_double), ("overlap_percent", c_double), ("threshold", c_double), ("burst", c_double)]


class AuDeclip(Structure):
    _fields_ = [("window_ms", c_double), ("overlap_percent", c_double), ("threshold", c_double)]


class AuCrossfeed(Structure):
    _fields_ = [("strength", c_double), ("range", c_double), ("slope", c_double), ("level_in", c_double),
                ("level_out", c_double)]


#+-----------------------------------------------------+
#| Structs (ffaudio_loudness.h, _meters.h, _playlist.h)|
#+-----------------------------------------------------+

class AuLoudness(Structure):
    _fields_ = [("silent", c_bool), ("integrated_lufs", c_double), ("loudness_range_lu", c_double),
                ("true_peak", c_double), ("sample_peak", c_double), ("duration", c_double),
                ("replaygain_gain_db", c_double), ("r128_gain_db", c_double), ("r128_gain_q78", c_int32)]


AuLoudnessProgressFn = CFUNCTYPE(c_bool, c_double, c_void_p)


class AuLoudnessOptions(Structure):
    _fields_ = [("skip_true_peak", c_bool), ("progress", AuLoudnessProgressFn), ("progress_user", c_void_p)]


AuGainCachedFn = CFUNCTYPE(None, c_char_p, c_int, POINTER(AuReplayGain), c_void_p)
AuGainCacheVisitorFn = CFUNCTYPE(c_bool, c_char_p, POINTER(AuReplayGain), c_void_p)


class AuGainScanSettings(Structure):
    _fields_ = [("threads", c_int), ("skip_true_peak", c_bool), ("on_cached", AuGainCachedFn), ("user", c_void_p),
                ("measure_streams", c_bool)]


class AuLevels(Structure):
    _fields_ = [("channels", c_int), ("sample_rate", c_int), ("window_seconds", c_double), ("peak", c_float * 8),
                ("rms", c_float * 8)]


class AuPlaylistEntry(Structure):
    _fields_ = [("path", c_char_p), ("title", c_char_p), ("duration", c_double)]


class AuPlaylistFile(Structure):
    _fields_ = [("count", c_int), ("entries", POINTER(POINTER(AuPlaylistEntry)))]


class AuCueTrack(Structure):
    _fields_ = [("number", c_int), ("path", c_char_p), ("start_seconds", c_double), ("length_seconds", c_double),
                ("title", c_char_p), ("performer", c_char_p), ("songwriter", c_char_p), ("isrc", c_char_p),
                ("replaygain", AuReplayGain)]


class AuCueSheet(Structure):
    _fields_ = [("count", c_int), ("tracks", POINTER(POINTER(AuCueTrack))), ("title", c_char_p),
                ("performer", c_char_p), ("genre", c_char_p), ("date", c_char_p)]


# Every struct, for the layout test
STRUCTS = [AuEvent, AuFade, AuCrossfade, AuReplayGain, AuReplayGainSettings, AuNetworkSettings, AuTag, AuMediaInfo,
           AuPicture, AuStatus, AuEqualizer, AuPeqBand, AuParametricEq, AuStereo, AuLimiter, AuCompressor, AuDeclick,
           AuDeclip, AuCrossfeed, AuLoudness, AuLoudnessOptions, AuGainScanSettings, AuLevels, AuPlaylistEntry,
           AuPlaylistFile, AuCueTrack, AuCueSheet]


#+-----------------------------------------------------+
#| Functions                                           |
#+-----------------------------------------------------+

_p = POINTER
_opaque = c_void_p
_FUNCTIONS = {
    # ffaudio.h
    "au_poll_event": (c_bool, [_p(AuEvent)]),
    "au_wait_event": (c_bool, [_p(AuEvent), c_double]),
    "au_play_options_create": (_opaque, []),
    "au_play_options_free": (None, [_opaque]),
    "au_play_options_set_range": (c_int, [_opaque, c_double, c_double]),
    "au_play_options_set_filters": (c_int, [_opaque, c_char_p]),
    "au_play_options_set_fade_in": (c_int, [_opaque, _p(AuFade)]),
    "au_play_options_set_replaygain": (c_int, [_opaque, _p(AuReplayGain)]),
    "au_play_options_set_http_headers": (c_int, [_opaque, c_char_p]),
    "au_init": (c_int, [c_char_p, AuEventFn, c_void_p]),
    "au_open_device": (c_int, []),
    "au_shutdown": (c_int, []),
    "au_set_device": (c_int, [c_char_p]),
    "au_set_output_format": (c_int, [c_int, c_int]),
    "au_list_devices": (_p(c_char_p), [_p(c_int)]),
    "au_free_devices": (None, [_p(c_char_p), c_int]),
    "au_get_device_name": (c_int, [_p(c_char), c_size_t]),
    "au_play": (c_int32, [c_char_p, _opaque]),
    "au_queue": (c_int32, [c_char_p, _opaque]),
    "au_skip": (c_int, []),
    "au_clear_queue": (c_int, []),
    "au_stop": (c_int, []),
    "au_pause": (c_int, [c_bool]),
    "au_seek": (c_int, [c_double]),
    "au_seek_fraction": (c_int, [c_double]),
    "au_set_crossfade": (c_int, [_p(AuCrossfade)]),
    "au_set_speed": (c_int, [c_double, c_bool]),
    "au_set_volume": (c_int, [c_float]),
    "au_set_muted": (c_int, [c_bool]),
    "au_set_loop_count": (c_int, [c_int]),
    "au_set_prepare_next": (c_int, [c_double]),
    "au_set_network": (c_int, [_p(AuNetworkSettings)]),
    "au_is_network_url": (c_bool, [c_char_p]),
    "au_set_filters": (c_int, [c_char_p]),
    "au_set_track_filters": (c_int, [c_int32, c_char_p]),
    "au_set_source_filters": (c_int, [c_char_p]),
    "au_send_filter_command": (c_int, [c_int32, c_char_p, c_char_p, c_char_p]),
    "au_validate_filters": (c_int, [c_char_p]),
    "au_set_downmix": (c_int, [c_int, c_char_p]),
    "au_set_upmix": (c_int, [c_int, c_char_p]),
    "au_set_replaygain": (c_int, [_p(AuReplayGainSettings)]),
    "au_track_gain_db": (c_int, [c_int32, _p(c_double)]),
    "au_gain_cache_set_file": (c_int, [c_char_p]),
    "au_gain_cache_get_file": (c_int, [_p(c_char), c_size_t]),
    "au_gain_cache_reload": (c_int, []),
    "au_gain_cache_set": (c_int, [c_char_p, _p(AuReplayGain)]),
    "au_gain_cache_get": (c_bool, [c_char_p, _p(AuReplayGain)]),
    "au_gain_cache_remove": (c_int, [c_char_p]),
    "au_gain_cache_clear": (c_int, []),
    "au_gain_cache_count": (c_int, []),
    "au_gain_cache_foreach": (c_int, [AuGainCacheVisitorFn, c_void_p]),
    "au_probe": (_p(AuMediaInfo), [c_char_p, _p(c_int)]),
    "au_track_info": (_p(AuMediaInfo), [c_int32]),
    "au_media_info_release": (None, [_p(AuMediaInfo)]),
    "au_media_tag": (c_char_p, [_p(AuMediaInfo), c_char_p]),
    "au_media_picture": (_p(AuPicture), [c_char_p, c_int, _p(c_int)]),
    "au_picture_release": (None, [_p(AuPicture)]),
    "au_get_status": (None, [_p(AuStatus)]),
    "au_get_version": (c_char_p, []),
    "au_result_string": (c_char_p, [c_int]),
    # ffaudio_filters.h
    "au_fb_create": (_opaque, []),
    "au_fb_free": (None, [_opaque]),
    "au_fb_clear": (None, [_opaque]),
    "au_fb_peek": (c_char_p, [_opaque]),
    "au_fb_last_error": (c_char_p, [_opaque]),
    "au_fb_add": (c_int, [_opaque, c_char_p]),
    "au_filter_available": (c_bool, [c_char_p]),
    "au_fb_add_volume_db": (c_int, [_opaque, c_char_p, c_double]),
    "au_equalizer_defaults": (AuEqualizer, []),
    "au_fb_add_equalizer": (c_int, [_opaque, c_char_p, _p(AuEqualizer)]),
    "au_fb_add_parametric_eq": (c_int, [_opaque, c_char_p, _p(AuParametricEq)]),
    "au_parametric_eq_parse": (c_int, [c_char_p, _p(AuParametricEq), _p(c_char), c_size_t]),
    "au_fb_add_tone": (c_int, [_opaque, c_char_p, c_double, c_double]),
    "au_stereo_defaults": (AuStereo, []),
    "au_fb_add_stereo": (c_int, [_opaque, c_char_p, _p(AuStereo)]),
    "au_limiter_defaults": (AuLimiter, []),
    "au_fb_add_limiter": (c_int, [_opaque, c_char_p, _p(AuLimiter)]),
    "au_compressor_defaults": (AuCompressor, []),
    "au_compressor_night": (AuCompressor, []),
    "au_fb_add_compressor": (c_int, [_opaque, c_char_p, _p(AuCompressor)]),
    "au_declick_defaults": (AuDeclick, []),
    "au_fb_add_declick": (c_int, [_opaque, c_char_p, _p(AuDeclick)]),
    "au_declip_defaults": (AuDeclip, []),
    "au_fb_add_declip": (c_int, [_opaque, c_char_p, _p(AuDeclip)]),
    "au_crossfeed_defaults": (AuCrossfeed, []),
    "au_fb_add_crossfeed": (c_int, [_opaque, c_char_p, _p(AuCrossfeed)]),
    "au_fb_add_bs2b_profile": (c_int, [_opaque, c_char_p, c_int]),
    "au_fb_add_bs2b": (c_int, [_opaque, c_char_p, c_int, c_int]),
    # ffaudio_loudness.h
    "au_loudness_measure": (c_int, [c_char_p, _p(AuLoudnessOptions), _p(_opaque)]),
    "au_loudness_merge": (c_int, [_opaque, _opaque]),
    "au_loudness_result": (c_int, [_opaque, _p(AuLoudness)]),
    "au_loudness_file_count": (c_int, [_opaque]),
    "au_loudness_meter_free": (None, [_opaque]),
    "au_loudness_analyze": (c_int, [c_char_p, _p(AuLoudnessOptions), _p(AuLoudness)]),
    "au_loudness_to_replaygain": (AuReplayGain, [_p(AuLoudness), _p(AuLoudness)]),
    "au_gain_cache_scan": (c_int, [c_char_p, _p(AuLoudnessOptions), _p(AuReplayGain)]),
    "au_set_gain_scan": (c_int, [_p(AuGainScanSettings)]),
    "au_gain_scan_request": (c_int, [_p(c_char_p), c_int, c_int]),
    "au_gain_scan_request_album": (c_int, [_p(c_char_p), c_int, c_int]),
    "au_gain_scan_cancel": (None, []),
    "au_gain_scan_pending": (c_int, []),
    "au_gain_scan_stop": (c_int, []),
    # ffaudio_meters.h
    "au_get_levels": (c_int, [c_double, _p(AuLevels)]),
    "au_spectrum_create": (_opaque, [c_int]),
    "au_spectrum_read": (c_int, [_opaque, c_int, _p(c_float), c_int, _p(c_int)]),
    "au_spectrum_bands": (c_int, [_p(c_float), c_int, c_int, c_double, c_double, _p(c_float), c_int]),
    "au_spectrum_free": (None, [_opaque]),
    # ffaudio_playlist.h
    "au_playlist_read": (_p(AuPlaylistFile), [c_char_p, _p(c_int)]),
    "au_playlist_parse": (_p(AuPlaylistFile), [c_char_p, c_size_t, c_char_p, _p(c_int)]),
    "au_playlist_file_release": (None, [_p(AuPlaylistFile)]),
    "au_cue_read": (_p(AuCueSheet), [c_char_p, _p(c_int)]),
    "au_cue_parse": (_p(AuCueSheet), [c_char_p, c_size_t, c_char_p, _p(c_int)]),
    "au_cue_release": (None, [_p(AuCueSheet)]),
}

for _name, (_res, _args) in _FUNCTIONS.items():
    _f = getattr(lib, _name)
    _f.restype = _res
    _f.argtypes = _args

FUNCTION_NAMES = sorted(_FUNCTIONS)

_major = int(lib.au_get_version().decode().split(".")[0])
if _major != ABI_MAJOR:
    raise ImportError(f"libffaudio {lib.au_get_version().decode()} found, these bindings are for {ABI_MAJOR}.x")
