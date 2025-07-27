#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

// Define PacketQueue structure for audio packets
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


typedef struct AudioState {
    AVFormatContext *ic;              // Input format context
    int audio_stream;                 // Index of audio stream
    AVStream *audio_st;               // Audio stream
    AVCodecContext *audio_ctx;        // Audio codec context
    PacketQueue audioq;               // Queue for audio packets
    int quit;                         // Flag to exit
    int paused;                       // Pause state
    SDL_AudioDeviceID audio_dev;      // SDL audio device ID
    
    // Audio resampling
    struct SwrContext *swr_ctx;       // Resampling context
    uint8_t *audio_buf;               // Audio buffer for converted samples
    unsigned int audio_buf_size;      // Size of audio buffer
    unsigned int audio_buf_index;     // Current position in audio buffer
    AVFrame *audio_frame;             // Temp audio frame
    int audio_hw_buf_size;            // Hardware buffer size
    
    // Audio specs
    int audio_src_freq;               // Source sample rate
    int audio_tgt_freq;               // Target sample rate
    int audio_tgt_channels;           // Target channels
    AVChannelLayout audio_src_ch_layout; // Source channel layout
    AVChannelLayout audio_tgt_ch_layout; // Target channel layout
    enum AVSampleFormat audio_src_fmt;// Source sample format
    enum AVSampleFormat audio_tgt_fmt;// Target sample format
} VideoState;

// Function prototypes
static void packet_queue_init(PacketQueue *q);
static int packet_queue_put(PacketQueue *q, AVPacket *pkt);
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
static int audio_open(VideoState *is);
static void audio_callback(void *userdata, Uint8 *stream, int len);
static int stream_component_open(VideoState *is, int stream_index);
static int read_thread(void *arg);
static void stream_close(VideoState *is);
static void do_exit(VideoState *is);

// Main function
int main(int argc, char **argv) {
    /*if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }*/

    // Initialize FFmpeg libraries
    // This is no longer required in newer FFmpeg versions, but doesn't hurt
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    avformat_network_init();
    
    VideoState *is = av_mallocz(sizeof(VideoState));
    if (!is) {
        fprintf(stderr, "Could not allocate VideoState\n");
        return 1;
    }
    
    // Initialize is structure
    is->audio_stream = -1;
    is->quit = 0;
    is->paused = 0;
    is->audio_buf = NULL;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    is->audio_frame = NULL;
    is->swr_ctx = NULL;
    
    // Initialize channel layouts
    av_channel_layout_uninit(&is->audio_src_ch_layout);
    av_channel_layout_uninit(&is->audio_tgt_ch_layout);
    
    // Pre-initialize channel layouts with default values
    av_channel_layout_default(&is->audio_src_ch_layout, 2); // Default to stereo
    av_channel_layout_default(&is->audio_tgt_ch_layout, 2); // Default to stereo

    // Initialize SDL for audio and timer
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        return 1;
    }

    // Create a minimal window for event handling
    SDL_Window *screen = SDL_CreateWindow("FFplay Audio", SDL_WINDOWPOS_UNDEFINED,
                                         SDL_WINDOWPOS_UNDEFINED, 320, 240, 0);
    if (!screen) {
        fprintf(stderr, "SDL: could not create window - exiting\n");
        return 1;
    }

    // Open input file
    const char* input_file = "/home/malkj/Downloads/Unique Formats/Jhariah.flac";
    if (avformat_open_input(&is->ic, input_file, NULL, NULL) != 0) {
        fprintf(stderr, "Could not open input file '%s'\n", input_file);
        do_exit(is);
    }

    // Find stream info
    if (avformat_find_stream_info(is->ic, NULL) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information\n");
        do_exit(is);
    }

    // Find audio stream
    is->audio_stream = av_find_best_stream(is->ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (is->audio_stream < 0) {
        fprintf(stderr, "Could not find audio stream in input file\n");
        do_exit(is);
    }

    // Open audio stream component
    if (stream_component_open(is, is->audio_stream) < 0) {
        fprintf(stderr, "Could not open audio stream\n");
        do_exit(is);
    }

    // Initialize audio packet queue
    packet_queue_init(&is->audioq);

    // Start reading thread
    SDL_Thread *read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!read_tid) {
        fprintf(stderr, "Could not create read thread\n");
        do_exit(is);
    }

    // Main event loop
    SDL_Event event;
    while (!is->quit) {
        SDL_WaitEvent(&event);
        switch (event.type) {
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                    case SDLK_q:
                    case SDLK_ESCAPE:
                        is->quit = 1;
                        break;
                    case SDLK_SPACE:
                        is->paused = !is->paused;
                        SDL_PauseAudioDevice(is->audio_dev, is->paused);
                        break;
                }
                break;
            case SDL_QUIT:
                is->quit = 1;
                break;
        }
    }

    // Wait for read thread to finish
    SDL_WaitThread(read_tid, NULL);
    stream_close(is);
    SDL_DestroyWindow(screen);
    SDL_Quit();
    return 0;
}

// Packet queue initialization
static void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

// Audio resampling and buffering
static int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {
    AVPacket pkt;
    AVFrame *frame = is->audio_frame;
    int len1, data_size = 0;
    int resampled_data_size = 0;
    int64_t dec_channel_layout;
    
    // Sanity checks
    if (!is || !is->audio_ctx || !frame || !audio_buf) {
        fprintf(stderr, "Invalid parameters in audio_decode_frame\n");
        return -1;
    }
    
    for (;;) {
        // Get a new packet if we've processed all previous ones
        if (packet_queue_get(&is->audioq, &pkt, 1) <= 0) {
            return -1; // No more packets, could be EOF or error
        }
        
        // Ensure the codec context is valid
        if (!is->audio_ctx) {
            fprintf(stderr, "Audio codec context is NULL\n");
            av_packet_unref(&pkt);
            return -1;
        }
        
        // Send packet to decoder
        int ret = avcodec_send_packet(is->audio_ctx, &pkt);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "Error sending packet to decoder: %s\n", errbuf);
            av_packet_unref(&pkt);
            continue;
        }
        
        // Receive decoded frames
        ret = avcodec_receive_frame(is->audio_ctx, frame);
        av_packet_unref(&pkt); // Unref packet as we don't need it anymore
        
        if (ret == AVERROR(EAGAIN)) {
            continue; // Need more packets
        } else if (ret < 0) {
            return -1; // Error or EOF
        }
        
        // We have a valid frame, now convert it to our desired format
        
        // Check if we need to update resampler
        if (frame->format != is->audio_src_fmt ||
            av_channel_layout_compare(&frame->ch_layout, &is->audio_src_ch_layout) != 0 ||
            frame->sample_rate != is->audio_src_freq) {
            
            if (is->swr_ctx) swr_free(&is->swr_ctx);
            
            is->swr_ctx = swr_alloc();
            if (!is->swr_ctx) {
                fprintf(stderr, "Cannot allocate resampler context\n");
                return -1;
            }
            
            // Set source parameters
            av_opt_set_int(is->swr_ctx, "in_sample_rate", frame->sample_rate, 0);
            av_opt_set_sample_fmt(is->swr_ctx, "in_sample_fmt", frame->format, 0);
            av_opt_set_chlayout(is->swr_ctx, "in_chlayout", &frame->ch_layout, 0);
            
            // Set target parameters
            av_opt_set_int(is->swr_ctx, "out_sample_rate", is->audio_tgt_freq, 0);
            av_opt_set_sample_fmt(is->swr_ctx, "out_sample_fmt", is->audio_tgt_fmt, 0);
            av_opt_set_chlayout(is->swr_ctx, "out_chlayout", &is->audio_tgt_ch_layout, 0);
            
            if (swr_init(is->swr_ctx) < 0) {
                fprintf(stderr, "Cannot initialize the resampling context\n");
                return -1;
            }
            
            // Save source parameters for future comparison
            av_channel_layout_copy(&is->audio_src_ch_layout, &frame->ch_layout);
            is->audio_src_freq = frame->sample_rate;
            is->audio_src_fmt = frame->format;
        }
        
        if (is->swr_ctx) {
            // Calculate output samples
            int out_samples = frame->nb_samples * is->audio_tgt_freq / frame->sample_rate + 256;
            int out_bytes = av_samples_get_buffer_size(NULL, is->audio_tgt_channels, out_samples, is->audio_tgt_fmt, 0);
            if (out_bytes < 0) {
                fprintf(stderr, "av_samples_get_buffer_size error\n");
                return -1;
            }
            
            // Make sure our buffer is big enough
            if (out_bytes > buf_size) {
                fprintf(stderr, "Buffer too small for audio conversion\n");
                return -1;
            }
            
            // Convert samples
            uint8_t *out_buf[1] = { audio_buf };
            int converted_samples = swr_convert(is->swr_ctx, 
                                               out_buf, out_samples,
                                               (const uint8_t **)frame->data, frame->nb_samples);
            if (converted_samples < 0) {
                fprintf(stderr, "swr_convert error\n");
                return -1;
            }
            
            resampled_data_size = converted_samples * is->audio_tgt_channels * av_get_bytes_per_sample(is->audio_tgt_fmt);
            return resampled_data_size;
        } else {
            // If no resampling, just copy the data
            data_size = av_samples_get_buffer_size(NULL, frame->ch_layout.nb_channels,
                                                frame->nb_samples, frame->format, 1);
            memcpy(audio_buf, frame->data[0], data_size);
            return data_size;
        }
    }
}

// Put packet into queue
static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1) return -1;
    
    // Create a new packet that will be owned by the queue
    if (av_packet_make_refcounted(pkt) < 0) {
        av_free(pkt1);
        return -1;
    }
    
    // Make a copy by incrementing reference count
    if (av_packet_ref(&pkt1->pkt, pkt) < 0) {
        av_free(pkt1);
        return -1;
    }
    
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);
    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }
    q->last_pkt = pkt1;
    q->nb_packets++;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

// Get packet from queue
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    /*// Make sure the packet is initialized
    av_packet_unref(pkt); // Ensure it's clean first*/

    SDL_LockMutex(q->mutex);
    for (;;) {
        // Check if we should abort the operation (e.g., due to quit flag)
        if (q->nb_packets <= 0) {
            if (!block) {
                ret = 0; // No packets and not blocking
                break;
            } else {
                // Wait for packet or timeout (100ms)
                int wait_result = SDL_CondWaitTimeout(q->cond, q->mutex, 100);
                if (wait_result == SDL_MUTEX_TIMEDOUT) {
                    ret = 0; // Timeout - no packets yet
                    break;
                }
                continue; // Try again
            }
        }
        
        // Got a packet - dequeue it
        pkt1 = q->first_pkt;
        q->first_pkt = pkt1->next;
        if (!q->first_pkt) {
            q->last_pkt = NULL;
        }
        q->nb_packets--;
        
        // Move packet to output (no need to copy)
        *pkt = pkt1->pkt;
        av_free(pkt1); // Free only the list node
        ret = 1;
        break;
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// Open audio stream component
static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;
    int ret;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        fprintf(stderr, "Invalid stream index: %d\n", stream_index);
        return -1;
    }

    // Allocate codec context
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        return AVERROR(ENOMEM);
    }

    // Copy codec parameters to context
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy codec parameters to context\n");
        avcodec_free_context(&avctx);
        return ret;
    }

    // Find decoder
    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        fprintf(stderr, "Failed to find decoder for codec ID: %d\n", avctx->codec_id);
        avcodec_free_context(&avctx);
        return -1;
    }

    // Set timebase
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;
    
    // Explicitly set thread count to 1 to avoid threading issues
    avctx->thread_count = 1;
    
    // Open codec
    ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Failed to open codec: %s\n", errbuf);
        avcodec_free_context(&avctx);
        return ret;
    }
    
    // Print debug info
    fprintf(stderr, "Successfully opened audio codec: %s, sample_rate: %d, channels: %d\n", 
            codec->name, avctx->sample_rate, avctx->ch_layout.nb_channels);
    
    // Set the audio codec context
    is->audio_ctx = avctx;
    is->audio_st = ic->streams[stream_index];

    is->audio_ctx = avctx;
    is->audio_st = ic->streams[stream_index];

    // Allocate audio frame
    is->audio_frame = av_frame_alloc();
    if (!is->audio_frame) {
        avcodec_free_context(&avctx);
        return AVERROR(ENOMEM);
    }
    
    // Initialize audio resampling parameters
    is->audio_src_fmt = avctx->sample_fmt;
    is->audio_src_freq = avctx->sample_rate;
    
    // Copy source channel layout
    av_channel_layout_copy(&is->audio_src_ch_layout, &avctx->ch_layout);
    
    // Set target audio parameters for SDL
    is->audio_tgt_fmt = AV_SAMPLE_FMT_S16;
    is->audio_tgt_freq = avctx->sample_rate;
    is->audio_tgt_channels = avctx->ch_layout.nb_channels;
    
    // Set up target channel layout matching SDL requirements
    av_channel_layout_default(&is->audio_tgt_ch_layout, is->audio_tgt_channels);

    // Set up SDL audio
    wanted_spec.freq = is->audio_tgt_freq;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = is->audio_tgt_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024; // Buffer size
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;

    is->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 0);
    if (is->audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        avcodec_free_context(&avctx);
        av_frame_free(&is->audio_frame);
        return -1;
    }
    
    // Allocate audio buffer
    is->audio_hw_buf_size = spec.size;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    
    // Buffer should be large enough to hold multiple converted audio frames
    is->audio_buf = av_malloc(MAX_AUDIO_FRAME_SIZE * 4);
    if (!is->audio_buf) {
        fprintf(stderr, "Could not allocate audio buffer\n");
        avcodec_free_context(&avctx);
        av_frame_free(&is->audio_frame);
        return -1;
    }

    // Start audio playback
    SDL_PauseAudioDevice(is->audio_dev, 0);
    return 0;
}

// Audio callback function
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *is = userdata;
    int audio_size, len1;

    // Safety check
    if (!is || !stream) {
        return;
    }

    // Clear output buffer
    memset(stream, 0, len);
    
    // If paused, just return silence
    if (is->paused) {
        return;
    }
    
    // Safety check for audio buffer
    if (!is->audio_buf) {
        fprintf(stderr, "Audio buffer is NULL in callback\n");
        return;
    }
    
    while (len > 0) {
        // If we have data in our buffer, use it
        if (is->audio_buf_index >= is->audio_buf_size) {
            // We've consumed all data, get more
            audio_size = audio_decode_frame(is, is->audio_buf, MAX_AUDIO_FRAME_SIZE);
            if (audio_size < 0) {
                // Error: output silence
                is->audio_buf_size = 1024; // Small amount of silence
                memset(is->audio_buf, 0, is->audio_buf_size);
                
                // Only attempt to restart if we're at the end of file
                if (is->ic && is->ic->pb && avio_feof(is->ic->pb)) {
                    fprintf(stderr, "End of file in audio callback, seeking to start\n");
                    
                    // We've reached EOF, so seek back to the start
                    if (is->ic && is->audio_st) {
                        int ret = av_seek_frame(is->ic, is->audio_stream, 0, AVSEEK_FLAG_BACKWARD);
                        if (ret < 0) {
                            char errbuf[256];
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            fprintf(stderr, "Error seeking to start: %s\n", errbuf);
                        } else {
                            fprintf(stderr, "Successfully seeked to start\n");
                        }
                        
                        // Flush decoders
                        if (is->audio_ctx) {
                            avcodec_flush_buffers(is->audio_ctx);
                        }
                        
                        // Flush the packet queue
                        SDL_LockMutex(is->audioq.mutex);
                        AVPacketList *pkt1 = is->audioq.first_pkt;
                        while (pkt1) {
                            AVPacketList *tmp = pkt1;
                            pkt1 = pkt1->next;
                            av_packet_unref(&tmp->pkt);
                            av_free(tmp);
                        }
                        is->audioq.first_pkt = is->audioq.last_pkt = NULL;
                        is->audioq.nb_packets = 0;
                        SDL_UnlockMutex(is->audioq.mutex);
                    }
                } else {
                    // Not EOF or can't seek, just wait
                    SDL_Delay(10);
                }
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        
        // Copy from our buffer to the SDL stream
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        
        memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}


// Read thread to fetch packets
/*static int read_thread(void *arg) {
    VideoState *is = arg;
    AVPacket pkt;

    while (!is->quit) {
        if (av_read_frame(is->ic, &pkt) < 0) {
            is->quit = 1; // EOF or error
            break;
        }
        if (pkt.stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, &pkt);
        } else {
            av_packet_unref(&pkt);
        }
    }
    return 0;
}*/

static int read_thread(void *arg) {
    VideoState *is = arg;
    AVPacket *pkt = av_packet_alloc();
    int ret;
    
    if (!pkt) {
        fprintf(stderr, "Could not allocate packet\n");
        is->quit = 1;
        return -1;
    }
    
    fprintf(stderr, "Read thread started\n");

    while (!is->quit) {
        // Reset packet before reuse
        av_packet_unref(pkt);
        
        ret = av_read_frame(is->ic, pkt);
        if (ret < 0) {
            // Log the actual error
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            
            // Only print error if it's not EOF
            if (ret != AVERROR_EOF) {
                fprintf(stderr, "av_read_frame error: %s\n", errbuf);
            } else {
                fprintf(stderr, "End of file reached, continuing...\n");
            }

            // At EOF, we don't quit - let the audio callback handle looping
            if (ret == AVERROR_EOF) {
                // Sleep to avoid busy-waiting
                SDL_Delay(10);
                continue;
            }
            
            // For other errors, we'll quit
            is->quit = 1;
            break;
        }

        // Check if this packet is from our audio stream
        if (pkt->stream_index == is->audio_stream) {
            // Put packet in queue
            if (packet_queue_put(&is->audioq, pkt) < 0) {
                fprintf(stderr, "Error putting packet in queue\n");
                // Don't unref here - packet_queue_put takes ownership whether it succeeds or fails
            }
        } else {
            // Not our stream, discard
            av_packet_unref(pkt);
        }
        
        // Don't hog the CPU - give other threads a chance
        SDL_Delay(1);
    }
    
    av_packet_free(&pkt);
    fprintf(stderr, "Read thread exiting\n");
    return 0;
}


// Close streams and free resources
static void stream_close(VideoState *is) {
    if (!is) return;
    
    // Stop and close audio
    if (is->audio_dev) {
        SDL_PauseAudioDevice(is->audio_dev, 1);
        SDL_CloseAudioDevice(is->audio_dev);
    }
    
    // Free audio resampling context
    if (is->swr_ctx) {
        swr_free(&is->swr_ctx);
    }
    
    // Free audio frame
    if (is->audio_frame) {
        av_frame_free(&is->audio_frame);
    }
    
    // Free audio buffer
    if (is->audio_buf) {
        av_free(is->audio_buf);
    }
    
    // Clean up channel layouts
    av_channel_layout_uninit(&is->audio_src_ch_layout);
    av_channel_layout_uninit(&is->audio_tgt_ch_layout);
    
    // Clean up codec context
    if (is->audio_ctx) {
        avcodec_free_context(&is->audio_ctx);
    }
    
    // Close input file
    if (is->ic) {
        avformat_close_input(&is->ic);
    }
    
    // Clean up packet queue
    if (is->audioq.mutex) {
        SDL_LockMutex(is->audioq.mutex);
        // Free any remaining packets
        AVPacketList *pkt1 = is->audioq.first_pkt;
        while (pkt1) {
            AVPacketList *tmp = pkt1;
            pkt1 = pkt1->next;
            av_packet_unref(&tmp->pkt);
            av_free(tmp);
        }
        SDL_UnlockMutex(is->audioq.mutex);
        SDL_DestroyMutex(is->audioq.mutex);
    }
    if (is->audioq.cond) {
        SDL_DestroyCond(is->audioq.cond);
    }
    
    // Free the state structure
    av_free(is);
}

// Exit handler
static void do_exit(VideoState *is) {
    stream_close(is);
    SDL_Quit();
    exit(1);
}

/*int decode_audio(AVCodecContext *avctx, AVPacket *avpkt, AVFrame *frame) {
    int ret;

    // Send the packet to the decoder
    ret = avcodec_send_packet(avctx, avpkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending packet to decoder: %s\n", av_err2str(ret));
        return ret;
    }

    // Receive decoded frames
    while (ret >= 0) {
        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // EAGAIN: Decoder needs more data; EOF: End of stream
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error receiving frame: %s\n", av_err2str(ret));
            break;
        }

        // Frame is decoded successfully!
        // Access frame->data (audio data) and frame->nb_samples (number of samples)
        int num_samples = frame->nb_samples;
        int channels = frame->ch_layout.nb_channels;
        printf("Decoded frame with %d samples, %d channels\n", num_samples, channels);

        // Process the frame (e.g., send to audio playback)
        // Note: You may need to handle the frame->format and buffer sizes
    }

    return 0; // Success
}*/