#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include <alsa/asoundlib.h>

typedef struct AudioQueueNode {
    AVPacket *packet;
    struct AudioQueueNode *next;
} AudioQueueNode;

typedef struct {
    AudioQueueNode *head;
    AudioQueueNode *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int abort;
} AudioQueue;

void queue_init(AudioQueue *q) {
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->abort = 0;
}

void queue_put(AudioQueue *q, AVPacket *pkt) {
    AVPacket *new_pkt = av_packet_alloc();
    av_packet_ref(new_pkt, pkt);

    AudioQueueNode *node = malloc(sizeof(AudioQueueNode));
    node->packet = new_pkt;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);
    if (!q->tail) {
        q->head = node;
    } else {
        q->tail->next = node;
    }
    q->tail = node;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

AVPacket* queue_get(AudioQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while (!q->head && !q->abort) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    if (q->abort && !q->head) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    AudioQueueNode *node = q->head;
    q->head = q->head->next;
    if (!q->head) {
        q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mutex);

    AVPacket *pkt = node->packet;
    free(node);
    return pkt;
}

void queue_free(AudioQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort = 1;
    pthread_cond_broadcast(&q->cond);
    AudioQueueNode *curr = q->head;
    while (curr) {
        AudioQueueNode *next = curr->next;
        av_packet_free(&curr->packet);
        free(curr);
        curr = next;
    }
    q->head = q->tail = NULL;
    pthread_mutex_unlock(&q->mutex);
}

typedef struct {
    AudioQueue *queue;
    AVCodecContext *codec_ctx;
    snd_pcm_t *pcm_handle;
    SwrContext *swr_ctx;
} AudioThreadArgs;

void* audio_thread_func(void *arg) {
    AudioThreadArgs *args = (AudioThreadArgs *)arg;
    AVFrame *audio_frame = av_frame_alloc();

    while (1) {
        AVPacket *packet = queue_get(args->queue);
        if (!packet) break;

        if (avcodec_send_packet(args->codec_ctx, packet) >= 0) {
            while (avcodec_receive_frame(args->codec_ctx, audio_frame) >= 0) {
                int out_samples = audio_frame->nb_samples;
                uint8_t *output_buffer = NULL;
                av_samples_alloc(&output_buffer, NULL, args->codec_ctx->ch_layout.nb_channels, out_samples, AV_SAMPLE_FMT_S16, 0);

                swr_convert(args->swr_ctx, &output_buffer, out_samples, (const uint8_t **)audio_frame->data, audio_frame->nb_samples);

                int err = snd_pcm_writei(args->pcm_handle, output_buffer, out_samples);
                if (err == -EPIPE) {
                    snd_pcm_prepare(args->pcm_handle);
                    snd_pcm_writei(args->pcm_handle, output_buffer, out_samples);
                } else if (err == -EAGAIN) {
                    usleep(1000);
                    snd_pcm_writei(args->pcm_handle, output_buffer, out_samples);
                }

                av_freep(&output_buffer);
            }
        }
        av_packet_free(&packet);
    }
    av_frame_free(&audio_frame);
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

    AVCodecContext *a_codec_ctx = NULL;
    snd_pcm_t *pcm_handle = NULL;
    SwrContext *swr_ctx = NULL;
    AudioQueue audio_queue;
    pthread_t audio_thread;
    AudioThreadArgs thread_args;

    if (audio_stream_idx != -1) {
        queue_init(&audio_queue);
        AVCodecParameters *a_codec_params = format_ctx->streams[audio_stream_idx]->codecpar;
        const AVCodec *a_codec = avcodec_find_decoder(a_codec_params->codec_id);
        a_codec_ctx = avcodec_alloc_context3(a_codec);
        avcodec_parameters_to_context(a_codec_ctx, a_codec_params);
        avcodec_open2(a_codec_ctx, a_codec, NULL);

        if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            fprintf(stderr, "Error: Could not open ALSA audio device.\n");
            return 1;
        }

        snd_pcm_set_params(pcm_handle,
                           SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           a_codec_ctx->ch_layout.nb_channels,
                           a_codec_ctx->sample_rate,
                           1,
                           150000);

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

        thread_args.queue = &audio_queue;
        thread_args.codec_ctx = a_codec_ctx;
        thread_args.pcm_handle = pcm_handle;
        thread_args.swr_ctx = swr_ctx;

        pthread_create(&audio_thread, NULL, audio_thread_func, &thread_args);
    }

    AVFrame *frame = av_frame_alloc();
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

    AVPacket *packet = av_packet_alloc();
    struct timespec start, end;
    const long frame_delay_us = 33333;

    while (av_read_frame(format_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(v_codec_ctx, packet) >= 0) {
                while (avcodec_receive_frame(v_codec_ctx, frame) >= 0) {
                    clock_gettime(CLOCK_MONOTONIC, &start);

                    sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
                              frame->linesize, 0, v_codec_ctx->height,
                              frame_rgb->data, frame_rgb->linesize);

                    for (int y = 0; y < vinfo.yres; y++) {
                        uint8_t *dest = fbp + (y * finfo.line_length);
                        uint8_t *src = frame_rgb->data[0] + (y * frame_rgb->linesize[0]);
                        memcpy(dest, src, vinfo.xres * (vinfo.bits_per_pixel / 8));
                    }

                    clock_gettime(CLOCK_MONOTONIC, &end);
                    long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
                    (end.tv_nsec - start.tv_nsec) / 1000;

                    if (elapsed_us < frame_delay_us) {
                        usleep(frame_delay_us - elapsed_us);
                    }
                }
            }
        }
        else if (packet->stream_index == audio_stream_idx && pcm_handle != NULL) {
            queue_put(&audio_queue, packet);
        }
        av_packet_unref(packet);
    }

    if (audio_stream_idx != -1) {
        queue_free(&audio_queue);
        pthread_join(audio_thread, NULL);
    }

    av_packet_free(&packet);
    av_free(buffer);
    av_frame_free(&frame_rgb);
    av_frame_free(&frame);
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
