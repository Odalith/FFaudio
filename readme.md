### ⚠️ This project is in alpha, use at your own risk. Contributions/suggestions welcome

### FFaudio is a high-level audio player library using FFmpeg & SDL2. 
It is a heavily modified version of FFplay, so all credit for its general design goes to the FFmpeg team and contributors.

## Features
- Cross-platform support for Windows, Linux, macOS, Android, and iOS
- Thanks to FFmpeg it can decode and play basically any file with an audio component
- Support for 10 band Equalizer, Crossfeed, and EBU R128 audio normalization
- Easy-to-use API for integrating audio playback into applications
- Performant and memory efficient thanks again to FFmpeg
- Supports playing audio through a custom audio device
- Licensed under the LGPLv2.1 and free to use

## Who It's For
- Anyone making audio/music applications; It is primarily made for use with music players.
- Anyone who wants to integrate audio playback into their applications without having to worry about file formats

## Planned Features
- Fully support playback of rtp, rtsp, udp, and sdp (non-realtime) audio streams
- Gapless playback for non-realtime streams via 'soon to be done callback'
- Realtime updates to Equalizer
- Crossfade with custom crossfade time (Note that the currently supported Crossfeed is different from Crossfade)

## Possible Features
- Audio mixing of multiple streams
- Audio file conversion
- OS integration. Linux MPRIS support, for example.
- Custom channel layouts
- Support for audio formats with more than two channels
- Assuming compatible hardware, support playing DSD without conversion to PCM
- Generation of EBU R128 audio normalization data (Currently, you have to do this yourself. FFaudio only does the adjustment part)
- Multiple simultaneous audio devices


Note; this project is not affiliated with FFmpeg, FFplay, or their Authors.

## Usage
(1) After compiling and linking to your project, you will need to initialize the audio system before playing anything:
```C
#include "ffaudio.h"

static void error_callback(const char* message, int request) {
   printf(message);
}

static void eof_callback(bool is_eof_from_skip) {
    printf("EOF\n");
}

static void restart_callback(void) { 
    printf("Restart\n");
}

// In your main()..

const InitializeConfig config = {
    .app_name = "Test App",
    .initial_volume = 50,
    .initial_loop_count = 0,
    .on_error = error_callback,
    .on_eof = eof_callback,
    .on_restart = restart_callback
};

initialize(config);
```
The config parameters are as follows:
1. App name. This will be what shows up in your system
2. Initial volume. 0-100
3. Initial loop count. -1 is infinite looping, 0 is no looping.
4. Error callback. Called when an error occurs.
5. EOF callback. Called when the end of the file is reached. This can used to play the next file.
6. Restart callback. Called when the file is restarted after seeking. Useful to update UI when a file is looping.

You can pass null into any of the callbacks if you don't want to use them.

(2) Next, you will need to set up your audio device. This can be as simple as calling:
```C
configure_audio_device(NULL);
```
This will create an audio device with the default settings.

__Setup Done!__

(3) Now you can play your audio with:
```C
play_audio("/path/song.mp3", NULL);
```

__For a more complete and functional example, look at tests/test.c__

---

Some other useful functions include:
```C
stop_audio();

pause_audio();

seek_percent();

seek_time();

set_audio_volume();

get_audio_volume();

mute_audio();

set_loop_count();

get_loop_count();

get_audio_play_time();

get_audio_duration();
```

---

## Todo 🚧
- [ ] Write docs for setting up with a custom audio device
- [ ] Create a test suite
- [X] Setup audio device reconfigure for users and for when a device is lost during playback/idle (use system default)
  - Does not apply when audio device is configured with system default, SDL follows it but not custom set ones.
  - This will probably involve aborting playback, recreating the audio device, then playing the same stream with a seek to last pos
  - [X] Test with non audio file
  - [ ] Try to resolve "Could not open input stream." error when playing a new song directly after reconfiguring. avformat_open_input failing is most likely because is->abort is set to true. Would be best to try and not play the original file if the user is waiting for the next one to play
  - [X] Calling configure_audio_device(&AudConfig) then configure_audio_device(NULL) directly after causes segfault in strcmp().
- [X] Write additional public api functions
- [ ] Setup Github Actions for release
- [ ] Release C# P/Invoke and create nuget packages
- [ ] Test on more platforms
  - [ ] Windows
  - [X] Linux
  - [ ] macOS
  - [ ] Android
  - [ ] iOS
- [X] Setup Equalizer with filterchain
- [ ] Setup updates for Equalizer values during playback with `change` command: https://ffmpeg.org/ffmpeg-filters.html#anequalizer
- [ ] Setup and test with different formats (Currently fixed to S16 regardless of what the device wants)
- [ ] Create gapless audio playback for non-realtime streams
  - [X] 'Soon to be done' callback
  - [ ] Set up a second TrackState and swap in audio callback when the first stream is finished
  - [ ] AV network init/de-init
  - [ ] Cancel TrackState when the user changes stream after callback is called
- [ ] Regulate audio spec samples?
- [ ] Ensure proper deallocation when stream_open() fails before SDL_CreateThread(read_thread, ..) is called. (read_thread is what sends the cleanup message)
- [ ] Test with valgrind
- [ ] Send audio device updates to the user through callback
- [X] Cleanup ffaudio.h, use different header for private structs, defines
- [X] Create public config struct for initialize(), play_audio(), and configure_audio_device()
- [X] Put source files into a folder structure
- [X] Callback to update duration (when known) for files that estimate it
- [ ] Make sure `get_clock(&audio_player->current_track->audclk)` is accurate through pauses and seeks
- [ ] Upgrade to SDL3
- [ ] Implement initial seek and play time in play_audio()
- [X] Setup logging with call instead of av_log()
- [X] Add is_from_error flag to eof_callback
- [ ] Fix error flag == true when no errors
- [ ] Remove CrossFeed from 'track_filters' and add them in a similar fashion to Equalizer
- [ ] Add/test support for audio formats with more than two channels

