#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>

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

    VideoState *is = av_mallocz(sizeof(VideoState));
    if (!is) {
        fprintf(stderr, "Could not allocate VideoState\n");
        return 1;
    }

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
    if (avformat_open_input(&is->ic, "/home/malkj/Downloads/Unique Formats/Jhariah.flac", NULL, NULL) != 0) {
        fprintf(stderr, "Could not open input file '%s'\n", argv[1]);
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

// Put packet into queue
static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1) return -1;
    pkt1->pkt = *pkt;
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

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->nb_packets > 0) {
            pkt1 = q->first_pkt;
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) q->last_pkt = NULL;
            q->nb_packets--;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// Open audio stream component
static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) return AVERROR(ENOMEM);

    if (avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar) < 0) {
        avcodec_free_context(&avctx);
        return -1;
    }

    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        avcodec_free_context(&avctx);
        return -1;
    }

    avctx->pkt_timebase = ic->streams[stream_index]->time_base;
    if (avcodec_open2(avctx, codec, NULL) < 0) {
        avcodec_free_context(&avctx);
        return -1;
    }

    is->audio_ctx = avctx;
    is->audio_st = ic->streams[stream_index];

    // Set up SDL audio
    wanted_spec.freq = avctx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = avctx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024; // Buffer size
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;

    is->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 0);
    if (is->audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return -1;
    }

    // Start audio playback
    SDL_PauseAudioDevice(is->audio_dev, 0);
    return 0;
}

// Audio callback function
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *is = userdata;
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();

    if (!frame) return;

    // Clear output buffer
    memset(stream, 0, len);

    while (len > 0 && !is->quit) {
        if (packet_queue_get(&is->audioq, &pkt, 0) <= 0) {
            SDL_Delay(10); // Wait if no packets
            break;
        }

        // Send packet to decoder
        int ret = avcodec_send_packet(is->audio_ctx, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error sending packet to decoder\n");
            av_packet_unref(&pkt);
            continue;
        }

        // Receive frames from decoder
        while (ret >= 0 && len > 0) {
            ret = avcodec_receive_frame(is->audio_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // Need more packets
            } else if (ret < 0) {
                fprintf(stderr, "Error receiving frame\n");
                break;
            }

            // Process decoded frame
            int data_size = av_samples_get_buffer_size(NULL, frame->ch_layout.nb_channels,
                                                     frame->nb_samples,
                                                     frame->format, 1);
            if (data_size <= len) {
                memcpy(stream, frame->data[0], data_size);
                stream += data_size;
                len -= data_size;
            }
        }

        av_packet_unref(&pkt);
    }

    av_frame_free(&frame);
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
    AVPacket pkt;
    int ret;

    while (!is->quit) {
        ret = av_read_frame(is->ic, &pkt);
        if (ret < 0) {
            // Log the actual error
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "av_read_frame error: %s\n", errbuf);

            // Check if it's EOF or an actual error
            if (ret == AVERROR_EOF) {
                fprintf(stderr, "End of file reached\n");
            }

            is->quit = 1;
            break;
        }

        if (pkt.stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, &pkt);
        } else {
            av_packet_unref(&pkt);
        }
    }
    return 0;
}


// Close streams and free resources
static void stream_close(VideoState *is) {
    if (is->audio_dev) {
        SDL_CloseAudioDevice(is->audio_dev);
    }
    if (is->audio_ctx) {
        avcodec_free_context(&is->audio_ctx);
    }
    if (is->ic) {
        avformat_close_input(&is->ic);
    }
    if (is->audioq.mutex) {
        SDL_DestroyMutex(is->audioq.mutex);
    }
    if (is->audioq.cond) {
        SDL_DestroyCond(is->audioq.cond);
    }
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