### FFaudio is a high-level audio player library using FFmpeg & SDL2. 
It is a heavily modified version of FFplay, so all credit for its general design goes to the FFmpeg team and contributors.

## Features
- Cross-platform support for Windows, Linux, and macOS
- Thanks to FFmpeg it can decode and play basically any file with an audio component
- Support for Crossfeed and EBU128 audio normalization
- Easy-to-use API for integrating audio playback into applications
- Performant and memory efficient thanks again to FFmpeg
- 
- Licensed under the LGPLv2.1 and free to use

## Who It's For
- Anyone making audio/music applications; It is primarily made for use with music players.
- Anyone who wants to integrate audio playback into their applications without having to worry about file formats

## Planned Features
- Fully support playback of rtp, rtsp, udp, and sdp (non-realtime) audio streams
- Gapless playback for non-realtime streams via 'soon to be done callback'
- User-configurable Equalizer, with realtime updates
- Crossfade with custom crossfade time (Note that the currently supported Crossfeed is different from Crossfade)
- Audio mixing of multiple streams
- Generation of EBU128 audio normalization data (Currently, you have to do this yourself. FFaudio only does the adjustment part)


## Possible Features
- Audio file conversion
- OS integration. Linux MPRIS support, for example.
- Custom channel layouts
- Support for audio formats with more than two channels
- Assuming compatible hardware, support playing DSD without conversion to PCM
