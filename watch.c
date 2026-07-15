#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include <alsa/asoundlib.h>

typedef struct QueueNode {
    AVPacket *packet;
    struct QueueNode *next;
} QueueNode;

typedef struct {
    QueueNode *head;
    QueueNode *tail;
    volatile int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond_first;
    int abort;
} PacketQueue;

void queue_init(PacketQueue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond_first, NULL);
    q->abort = 0;
}

void queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacket *new_pkt = av_packet_alloc();
    av_packet_ref(new_pkt, pkt);

    QueueNode *node = malloc(sizeof(QueueNode));
    node->packet = new_pkt;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);
    if (q->abort) {
        av_packet_free(&new_pkt);
        free(node);
        pthread_mutex_unlock(&q->mutex);
        return;
    }

    if (!q->tail) {
        q->head = node;
    } else {
        q->tail->next = node;
    }
    q->tail = node;
    q->size++;
    pthread_cond_signal(&q->cond_first);
    pthread_mutex_unlock(&q->mutex);
}

AVPacket* queue_get(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while (!q->head && !q->abort) {
        pthread_cond_wait(&q->cond_first, &q->mutex);
    }
    if (q->abort && !q->head) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    QueueNode *node = q->head;
    q->head = q->head->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->size--;
    pthread_mutex_unlock(&q->mutex);

    AVPacket *pkt = node->packet;
    free(node);
    return pkt;
}

void queue_free(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort = 1;
    pthread_cond_broadcast(&q->cond_first);
    QueueNode *curr = q->head;
    while (curr) {
        QueueNode *next = curr->next;
        av_packet_free(&curr->packet);
        free(curr);
        curr = next;
    }
    q->head = q->tail = NULL;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

volatile double audio_clock = 0.0;
pthread_mutex_t clock_mutex = PTHREAD_MUTEX_INITIALIZER;

void set_audio_clock(double val) {
    pthread_mutex_lock(&clock_mutex);
    audio_clock = val;
    pthread_mutex_unlock(&clock_mutex);
}

double get_audio_clock() {
    pthread_mutex_lock(&clock_mutex);
    double val = audio_clock;
    pthread_mutex_unlock(&clock_mutex);
    return val;
}

typedef struct {
    PacketQueue *queue;
    AVCodecContext *codec_ctx;
    snd_pcm_t *pcm_handle;
    SwrContext *swr_ctx;
    AVRational time_base;
} AudioThreadArgs;

typedef struct {
    PacketQueue *queue;
    AVCodecContext *codec_ctx;
    struct SwsContext *sws_ctx;
    uint8_t *fbp;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    AVFrame *frame_rgb;
    AVRational time_base;
} VideoThreadArgs;

void* audio_thread_func(void *arg) {
    AudioThreadArgs *args = (AudioThreadArgs *)arg;
    AVFrame *audio_frame = av_frame_alloc();

    int max_out_samples = 8192;
    int channels = args->codec_ctx->ch_layout.nb_channels;
    int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    uint8_t *output_buffer = malloc(max_out_samples * channels * bytes_per_sample);

    while (1) {
        AVPacket *packet = queue_get(args->queue);
        if (!packet) break;

        if (avcodec_send_packet(args->codec_ctx, packet) >= 0) {
            while (avcodec_receive_frame(args->codec_ctx, audio_frame) >= 0) {
                int64_t delay = swr_get_delay(args->swr_ctx, args->codec_ctx->sample_rate);
                int out_samples = av_rescale_rnd(delay + audio_frame->nb_samples,
                                                 args->codec_ctx->sample_rate,
                                                 args->codec_ctx->sample_rate,
                                                 AV_ROUND_UP);

                if (out_samples > max_out_samples) {
                    max_out_samples = out_samples;
                    output_buffer = realloc(output_buffer, max_out_samples * channels * bytes_per_sample);
                }

                int converted = swr_convert(args->swr_ctx, &output_buffer, out_samples,
                                            (const uint8_t **)audio_frame->data, audio_frame->nb_samples);

                if (converted > 0) {
                    double pts = 0.0;
                    if (audio_frame->pts != AV_NOPTS_VALUE) {
                        pts = audio_frame->pts * av_q2d(args->time_base);
                    }

                    snd_pcm_sframes_t delay_frames = 0;
                    if (snd_pcm_delay(args->pcm_handle, &delay_frames) == 0) {
                        pts -= (double)delay_frames / args->codec_ctx->sample_rate;
                    }
                    set_audio_clock(pts);

                    int err = snd_pcm_writei(args->pcm_handle, output_buffer, converted);
                    if (err == -EPIPE) {
                        snd_pcm_prepare(args->pcm_handle);
                        snd_pcm_writei(args->pcm_handle, output_buffer, converted);
                    } else if (err == -EAGAIN) {
                        usleep(1000);
                        snd_pcm_writei(args->pcm_handle, output_buffer, converted);
                    }
                }
            }
        }
        av_packet_free(&packet);
    }
    free(output_buffer);
    av_frame_free(&audio_frame);
    return NULL;
}

void* video_thread_func(void *arg) {
    VideoThreadArgs *args = (VideoThreadArgs *)arg;
    AVFrame *frame = av_frame_alloc();

    while (1) {
        AVPacket *packet = queue_get(args->queue);
        if (!packet) break;

        if (avcodec_send_packet(args->codec_ctx, packet) >= 0) {
            while (avcodec_receive_frame(args->codec_ctx, frame) >= 0) {
                double pts = 0.0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    pts = frame->pts * av_q2d(args->time_base);
                }

                double audio_pts = get_audio_clock();
                double diff = pts - audio_pts;

                if (diff < -0.100) {
                    continue;
                }

                if (diff > 0.010) {
                    usleep((unsigned int)(diff * 1000000.0));
                }

                sws_scale(args->sws_ctx, (uint8_t const * const *)frame->data,
                          frame->linesize, 0, args->codec_ctx->height,
                          args->frame_rgb->data, args->frame_rgb->linesize);

                for (int y = 0; y < args->vinfo.yres; y++) {
                    uint8_t *dest = args->fbp + (y * args->finfo.line_length);
                    uint8_t *src = args->frame_rgb->data[0] + (y * args->frame_rgb->linesize[0]);
                    memcpy(dest, src, args->vinfo.xres * (args->vinfo.bits_per_pixel / 8));
                }
            }
        }
        av_packet_free(&packet);
    }
    av_frame_free(&frame);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <video_file.mp4>\n", argv[0]);
        return 1;
    }

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd == -1) {
        perror("Error opening framebuffer /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);

    long screensize = finfo.smem_len;
    uint8_t *fbp = (uint8_t *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    AVFormatContext *format_ctx = NULL;
    if (avformat_open_input(&format_ctx, argv[1], NULL, NULL) < 0) {
        fprintf(stderr, "Could not open video file %s\n", argv[1]);
        return 1;
    }
    avformat_find_stream_info(format_ctx, NULL);

    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1) {
            video_stream_idx = i;
        } else if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx == -1) {
            audio_stream_idx = i;
        }
    }

    AVCodecParameters *v_codec_params = format_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec *v_codec = avcodec_find_decoder(v_codec_params->codec_id);
    AVCodecContext *v_codec_ctx = avcodec_alloc_context3(v_codec);
    avcodec_parameters_to_context(v_codec_ctx, v_codec_params);
    avcodec_open2(v_codec_ctx, v_codec, NULL);

    AVFrame *frame_rgb = av_frame_alloc();
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB32, vinfo.xres, vinfo.yres, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(num_bytes * sizeof(uint8_t));
    av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer,
                         AV_PIX_FMT_RGB32, vinfo.xres, vinfo.yres, 1);

    struct SwsContext *sws_ctx = sws_getContext(
        v_codec_ctx->width, v_codec_ctx->height, v_codec_ctx->pix_fmt,
        vinfo.xres, vinfo.yres, AV_PIX_FMT_RGB32,
        SWS_POINT, NULL, NULL, NULL
    );

    PacketQueue audio_queue;
    PacketQueue video_queue;
    queue_init(&audio_queue);
    queue_init(&video_queue);

    pthread_t audio_thread;
    pthread_t video_thread;
    AudioThreadArgs audio_args;
    VideoThreadArgs video_args;

    AVCodecContext *a_codec_ctx = NULL;
    snd_pcm_t *pcm_handle = NULL;
    SwrContext *swr_ctx = NULL;

    if (audio_stream_idx != -1) {
        AVCodecParameters *a_codec_params = format_ctx->streams[audio_stream_idx]->codecpar;
        const AVCodec *a_codec = avcodec_find_decoder(a_codec_params->codec_id);
        a_codec_ctx = avcodec_alloc_context3(a_codec);
        avcodec_parameters_to_context(a_codec_ctx, a_codec_params);
        avcodec_open2(a_codec_ctx, a_codec, NULL);

        if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            fprintf(stderr, "Error: Could not open ALSA audio device.\n");
            return 1;
        }

        snd_pcm_hw_params_t *hw_params;
        snd_pcm_hw_params_alloca(&hw_params);
        snd_pcm_hw_params_any(pcm_handle, hw_params);
        snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm_handle, hw_params, a_codec_ctx->ch_layout.nb_channels);
        unsigned int rate = a_codec_ctx->sample_rate;
        snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &rate, 0);

        snd_pcm_uframes_t buffer_size = rate;
        snd_pcm_uframes_t period_size = rate / 4;
        snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_size);
        snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, 0);
        snd_pcm_hw_params(pcm_handle, hw_params);

        swr_ctx = swr_alloc();
        av_opt_set_chlayout(swr_ctx, "in_chlayout", &a_codec_ctx->ch_layout, 0);
        av_opt_set_int(swr_ctx, "in_sample_rate", a_codec_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);

        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, a_codec_ctx->ch_layout.nb_channels);
        av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_layout, 0);
        av_opt_set_int(swr_ctx, "out_sample_rate", a_codec_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        swr_init(swr_ctx);

        audio_args.queue = &audio_queue;
        audio_args.codec_ctx = a_codec_ctx;
        audio_args.pcm_handle = pcm_handle;
        audio_args.swr_ctx = swr_ctx;
        audio_args.time_base = format_ctx->streams[audio_stream_idx]->time_base;

        pthread_create(&audio_thread, NULL, audio_thread_func, &audio_args);
    }

    video_args.queue = &video_queue;
    video_args.codec_ctx = v_codec_ctx;
    video_args.sws_ctx = sws_ctx;
    video_args.fbp = fbp;
    video_args.vinfo = vinfo;
    video_args.finfo = finfo;
    video_args.frame_rgb = frame_rgb;
    video_args.time_base = format_ctx->streams[video_stream_idx]->time_base;

    pthread_create(&video_thread, NULL, video_thread_func, &video_args);

    AVPacket *packet = av_packet_alloc();
    while (av_read_frame(format_ctx, packet) >= 0) {
        while (video_queue.size > 150 && !video_queue.abort) {
            usleep(10000);
        }

        if (packet->stream_index == video_stream_idx) {
            queue_put(&video_queue, packet);
        } else if (packet->stream_index == audio_stream_idx && pcm_handle != NULL) {
            queue_put(&audio_queue, packet);
        }
        av_packet_unref(packet);
    }

    queue_free(&video_queue);
    pthread_join(video_thread, NULL);

    if (audio_stream_idx != -1) {
        queue_free(&audio_queue);
        pthread_join(audio_thread, NULL);
    }

    av_packet_free(&packet);
    av_free(buffer);
    av_frame_free(&frame_rgb);
    avcodec_free_context(&v_codec_ctx);
    if (a_codec_ctx) avcodec_free_context(&a_codec_ctx);
    if (swr_ctx) swr_free(&swr_ctx);
    avformat_close_input(&format_ctx);
    munmap(fbp, screensize);
    close(fb_fd);
    if (pcm_handle) {
        snd_pcm_drain(pcm_handle);
        snd_pcm_close(pcm_handle);
    }
    return 0;
}
