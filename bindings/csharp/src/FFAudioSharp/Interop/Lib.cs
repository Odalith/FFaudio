// Every exported function of libffaudio (abi/ffaudio.symbols), as the headers declare it. Strings that go in are UTF-8
// (marshalled by the generated code); strings that come out are byte * that the public layer decodes, because the
// library owns them. Opaque handles (AuPlayOptions *, AuFilterBuilder *, ...) are nint. Callbacks are plain function
// pointers (void *): the public layer passes [UnmanagedCallersOnly] methods.
//
// Only 64-bit platforms are supported, where there is a single calling convention.

using System.Runtime.InteropServices;

#pragma warning disable SYSLIB1054 // the signatures are deliberate

namespace FFAudioSharp.Interop;

internal static unsafe partial class Lib
{
    public const string Name = "ffaudio";

    //+-----------------------------------------------------+
    //| ffaudio.h                                           |
    //+-----------------------------------------------------+

    [LibraryImport(Name)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool au_poll_event(AuEvent* @out);
    [LibraryImport(Name)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool au_wait_event(AuEvent* @out, double timeout_seconds);

    [LibraryImport(Name)] public static partial nint au_play_options_create();
    [LibraryImport(Name)] public static partial void au_play_options_free(nint options);
    [LibraryImport(Name)] public static partial int au_play_options_set_range(nint options, double start_seconds, double length_seconds);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_play_options_set_filters(nint options, string? filters);
    [LibraryImport(Name)] public static partial int au_play_options_set_fade_in(nint options, AuFade* fade);
    [LibraryImport(Name)] public static partial int au_play_options_set_replaygain(nint options, AuReplayGain* replaygain);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_play_options_set_http_headers(nint options, string? headers);

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_init(string? app_name, void* on_event, void* user_data);
    [LibraryImport(Name)] public static partial int au_open_device();
    [LibraryImport(Name)] public static partial int au_shutdown();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_set_device(string? name);
    [LibraryImport(Name)] public static partial int au_set_output_format(int channels, int sample_rate);
    [LibraryImport(Name)] public static partial byte** au_list_devices(int* count);
    [LibraryImport(Name)] public static partial void au_free_devices(byte** devices, int count);
    [LibraryImport(Name)] public static partial int au_get_device_name(byte* buf, nuint size);

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_play(string path, nint options);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_queue(string path, nint options);
    [LibraryImport(Name)] public static partial int au_skip();
    [LibraryImport(Name)] public static partial int au_clear_queue();
    [LibraryImport(Name)] public static partial int au_stop();
    [LibraryImport(Name)] public static partial int au_pause([MarshalAs(UnmanagedType.U1)] bool paused);
    [LibraryImport(Name)] public static partial int au_seek(double seconds);
    [LibraryImport(Name)] public static partial int au_seek_fraction(double fraction);
    [LibraryImport(Name)] public static partial int au_set_crossfade(AuCrossfade* config);
    [LibraryImport(Name)] public static partial int au_set_speed(double speed, [MarshalAs(UnmanagedType.U1)] bool keep_pitch);
    [LibraryImport(Name)] public static partial int au_set_volume(float gain);
    [LibraryImport(Name)] public static partial int au_set_muted([MarshalAs(UnmanagedType.U1)] bool muted);
    [LibraryImport(Name)] public static partial int au_set_loop_count(int count);
    [LibraryImport(Name)] public static partial int au_set_prepare_next(double seconds);

    [LibraryImport(Name)] public static partial int au_set_network(AuNetworkSettings* config);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool au_is_network_url(string path);

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_set_filters(string? filters);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_set_track_filters(int handle, string? filters);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_set_source_filters(string? filters);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_send_filter_command(int handle, string target, string command, string arg);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_validate_filters(string? filters);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_set_downmix(int mode, string? custom_filters);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_set_upmix(int mode, string? custom_filters);

    [LibraryImport(Name)] public static partial int au_set_replaygain(AuReplayGainSettings* config);
    [LibraryImport(Name)] public static partial int au_track_gain_db(int handle, double* out_db);

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_gain_cache_set_file(string? path);
    [LibraryImport(Name)] public static partial int au_gain_cache_get_file(byte* buf, nuint size);
    [LibraryImport(Name)] public static partial int au_gain_cache_reload();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_gain_cache_set(string path, AuReplayGain* rg);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool au_gain_cache_get(string path, AuReplayGain* @out);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_gain_cache_remove(string path);
    [LibraryImport(Name)] public static partial int au_gain_cache_clear();
    [LibraryImport(Name)] public static partial int au_gain_cache_count();
    [LibraryImport(Name)] public static partial int au_gain_cache_foreach(void* visitor, void* user);

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial AuMediaInfo* au_probe(string path, int* out_error);
    [LibraryImport(Name)] public static partial AuMediaInfo* au_track_info(int handle);
    [LibraryImport(Name)] public static partial void au_media_info_release(AuMediaInfo* info);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial byte* au_media_tag(AuMediaInfo* info, string key);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial AuPicture* au_media_picture(string path, int type, int* out_error);
    [LibraryImport(Name)] public static partial void au_picture_release(AuPicture* picture);

    [LibraryImport(Name)] public static partial void au_get_status(AuStatus* @out);
    [LibraryImport(Name)] public static partial void au_wait_loop();
    [LibraryImport(Name)] public static partial byte* au_get_version();
    [LibraryImport(Name)] public static partial byte* au_result_string(int result);

    //+-----------------------------------------------------+
    //| ffaudio_filters.h                                   |
    //+-----------------------------------------------------+

    [LibraryImport(Name)] public static partial nint au_fb_create();
    [LibraryImport(Name)] public static partial void au_fb_free(nint fb);
    [LibraryImport(Name)] public static partial void au_fb_clear(nint fb);
    [LibraryImport(Name)] public static partial byte* au_fb_peek(nint fb);
    [LibraryImport(Name)] public static partial byte* au_fb_last_error(nint fb);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add(nint fb, string filter);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] [return: MarshalAs(UnmanagedType.U1)] public static partial bool au_filter_available(string name);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_volume_db(nint fb, string? name, double gain_db);

    [LibraryImport(Name)] public static partial AuEqualizer au_equalizer_defaults();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_equalizer(nint fb, string? name, AuEqualizer* eq);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_parametric_eq(nint fb, string? name, AuParametricEq* eq);
    [LibraryImport(Name)] public static partial int au_parametric_eq_parse(byte* text, AuParametricEq* @out, byte* error, nuint error_size);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_tone(nint fb, string? name, double bass_db, double treble_db);
    [LibraryImport(Name)] public static partial AuStereo au_stereo_defaults();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_stereo(nint fb, string? name, AuStereo* stereo);
    [LibraryImport(Name)] public static partial AuLimiter au_limiter_defaults();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_limiter(nint fb, string? name, AuLimiter* config);
    [LibraryImport(Name)] public static partial AuCompressor au_compressor_defaults();
    [LibraryImport(Name)] public static partial AuCompressor au_compressor_night();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_compressor(nint fb, string? name, AuCompressor* config);
    [LibraryImport(Name)] public static partial AuDeclick au_declick_defaults();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_declick(nint fb, string? name, AuDeclick* config);
    [LibraryImport(Name)] public static partial AuDeclip au_declip_defaults();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_declip(nint fb, string? name, AuDeclip* config);
    [LibraryImport(Name)] public static partial AuCrossfeed au_crossfeed_defaults();
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_crossfeed(nint fb, string? name, AuCrossfeed* config);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_bs2b_profile(nint fb, string? name, int profile);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_fb_add_bs2b(nint fb, string? name, int fcut_hz, int feed_tenth_db);

    //+-----------------------------------------------------+
    //| ffaudio_loudness.h                                  |
    //+-----------------------------------------------------+

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_loudness_measure(string path, AuLoudnessOptions* config, nint* @out);
    [LibraryImport(Name)] public static partial int au_loudness_merge(nint into, nint other);
    [LibraryImport(Name)] public static partial int au_loudness_result(nint meter, AuLoudness* @out);
    [LibraryImport(Name)] public static partial int au_loudness_file_count(nint meter);
    [LibraryImport(Name)] public static partial void au_loudness_meter_free(nint meter);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_loudness_analyze(string path, AuLoudnessOptions* config, AuLoudness* @out);
    [LibraryImport(Name)] public static partial AuReplayGain au_loudness_to_replaygain(AuLoudness* track, AuLoudness* album);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial int au_gain_cache_scan(string path, AuLoudnessOptions* config, AuReplayGain* @out);

    [LibraryImport(Name)] public static partial int au_set_gain_scan(AuGainScanSettings* config);
    [LibraryImport(Name)] public static partial int au_gain_scan_request(byte** paths, int count, int priority);
    [LibraryImport(Name)] public static partial int au_gain_scan_request_album(byte** paths, int count, int priority);
    [LibraryImport(Name)] public static partial void au_gain_scan_cancel();
    [LibraryImport(Name)] public static partial int au_gain_scan_pending();
    [LibraryImport(Name)] public static partial int au_gain_scan_stop();

    //+-----------------------------------------------------+
    //| ffaudio_meters.h                                    |
    //+-----------------------------------------------------+

    [LibraryImport(Name)] public static partial int au_get_levels(double window_seconds, AuLevels* @out);
    [LibraryImport(Name)] public static partial nint au_spectrum_create(int fft_size);
    [LibraryImport(Name)] public static partial int au_spectrum_read(nint spectrum, int channel, float* magnitudes, int capacity, int* sample_rate);
    [LibraryImport(Name)] public static partial int au_spectrum_bands(float* magnitudes, int fft_size, int sample_rate, double f_min, double f_max, float* bands, int n_bands);
    [LibraryImport(Name)] public static partial void au_spectrum_free(nint spectrum);

    //+-----------------------------------------------------+
    //| ffaudio_playlist.h                                  |
    //+-----------------------------------------------------+

    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial AuPlaylistFile* au_playlist_read(string path_or_url, int* out_error);
    [LibraryImport(Name)] public static partial AuPlaylistFile* au_playlist_parse(byte* text, nuint length, byte* @base, int* out_error);
    [LibraryImport(Name)] public static partial void au_playlist_file_release(AuPlaylistFile* playlist);
    [LibraryImport(Name, StringMarshalling = StringMarshalling.Utf8)] public static partial AuCueSheet* au_cue_read(string path_or_url, int* out_error);
    [LibraryImport(Name)] public static partial AuCueSheet* au_cue_parse(byte* text, nuint length, byte* @base, int* out_error);
    [LibraryImport(Name)] public static partial void au_cue_release(AuCueSheet* sheet);
}
