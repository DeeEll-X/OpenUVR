/*
    The MIT License (MIT)

    Copyright (c) 2020 OpenUVR

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

    Authors:
    Alec Rohloff
    Zackary Allen
    Kung-Min Lin
    Chengyi Nie
    Hung-Wei Tseng
*/
/**
 * Contains the primary entry points to OpenUVR. Using these functions requires the caller to manage OpenGL buffers and copy data from them. For functions which handle everything themselves, see "openuvr_managed.c".
 */
#include "openuvr.h"
#include "ouvr_packet.h"
#include "tcp.h"
#include "udp.h"
#include "raw.h"
#include "raw_ring.h"
#include "udp_compat.h"
#include "webrtc.h"
#include "inject.h"
#include "ffmpeg_encode.h"
#include "gst_encode.h"
#include "rgb_encode.h"
#include "pulse_audio.h"
#include "feedback_net.h"
#include "input_recv.h"
#include "ssim_dummy_net.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include <sys/time.h>
#include <time.h>

typedef struct pthread_context
{
    int should_exit;
    pthread_t send_thread;
} ouvr_pthread_context;

struct openuvr_context *openuvr_alloc_context(enum OPENUVR_ENCODER_TYPE enc_type, enum OPENUVR_NETWORK_TYPE net_type, uint8_t *pix_buf, unsigned int pbo)
{
    struct openuvr_context *ret = calloc(1, sizeof(struct openuvr_context));
    struct ouvr_ctx *ctx = calloc(1, sizeof(struct ouvr_ctx));

    switch (net_type)
    {
    case OPENUVR_NETWORK_TCP:
        ctx->net = &tcp_handler;
        break;
    case OPENUVR_NETWORK_RAW:
        ctx->net = &raw_handler;
        break;
    case OPENUVR_NETWORK_RAW_RING:
        ctx->net = &raw_ring_handler;
        break;
    case OPENUVR_NETWORK_INJECT:
        ctx->net = &inject_handler;
        break;
    case OPENUVR_NETWORK_UDP_COMPAT:
        ctx->net = &udp_compat_handler;
        break;
    case OPENUVR_NETWORK_WEBRTC:
        ctx->net = &webrtc_handler;
        break;
    case OPENUVR_NETWORK_UDP:
    default:
        ctx->net = &udp_handler;
    }
#ifdef MEASURE_SSIM
    ctx->net = &ssim_dummy_net_handler;
#endif
    if (ctx->net->init(ctx) != 0)
    {
        goto err;
    }

    ctx->pix_buf = pix_buf;
    ctx->pbo_handle = pbo;

    switch (enc_type)
    {
    case OPENUVR_ENCODER_RGB:
        ctx->enc = &rgb_encode;
        break;
    case OPENUVR_ENCODER_H264:
    default:
        ctx->enc = &gst_encode;
        // ctx->enc = &ffmpeg_encode;
    }
    if (ctx->enc->init(ctx) != 0)
    {
        goto err;
    }

    //// commented out since pulse_audio currently freezes. Uncomment to re-enable audio
    // ctx->aud = &pulse_audio;
    // if (ctx->aud->init(ctx) != 0)
    // {
    //     goto err;
    // }

    ctx->packet = ouvr_packet_alloc();

    receive_input_loop_start();
    ctx->flag_send_iframe = 0;
    feedback_initialize(ctx);

    ret->priv = ctx;

    return ret;

err:
    free(ctx);
    free(ret);
    return NULL;
}

#ifdef TIME_ENCODING
float avg_enc_time = 0;
#endif
#ifdef TIME_NETWORK
float avg_send_time = 0;
#endif

int openuvr_send_frame(struct openuvr_context *context)
{
#ifdef UE4DEBUG
    // PRINT_ERR("openuvr_send_frame entered\n");
#endif
    int ret;
    struct ouvr_ctx *ctx = context->priv;
#if defined(TIME_ENCODING) || defined(TIME_NETWORK)
    struct timeval start, end;
#endif

    //// Uncomment to re-enable audio capture and sending:
    // ret = ctx->aud->encode_frame(ctx, ctx->packet);
    // if (ret < 0)
    // {
    //     return -1;
    // }
    // else if (ret > 0)
    // {
    //     ret = ctx->net->send_packet(ctx, ctx->packet);
    //     if (ret < 0)
    //     {
    //         return -1;
    //     }
    // }

    feedback_receive(ctx);

#ifdef TIME_ENCODING
    gettimeofday(&start, NULL);
#endif
    do
    {
#ifdef UE4DEBUG
        // PRINT_ERR("before process_frame\n");
#endif
        ret = ctx->enc->process_frame(ctx, ctx->packet);
    } while (ret == 0);
#ifdef UE4DEBUG
    // PRINT_ERR("process_frame break while\n");
#endif
    if (ret < 0)
    {
#ifdef UE4DEBUG
        PRINT_ERR("process_frame ret %d\n", ret);
#endif
        return -1;
    }
    // PRINT_ERR("print encoding time\n");
#ifdef TIME_ENCODING
    gettimeofday(&end, NULL);
    int elapsed = end.tv_usec - start.tv_usec + (end.tv_sec > start.tv_sec ? 1000000 : 0);
    avg_enc_time = 0.998 * avg_enc_time + 0.002 * elapsed;
    PRINT_ERR("enc avg: %f, actual: %d\n", avg_enc_time, elapsed);
    // fprintf(stderr, "enc avg: %f, actual: %d\n", avg_enc_time, elapsed);
    fflush(stdout);
#endif

#ifdef TIME_NETWORK
    gettimeofday(&start, NULL);
#endif
#ifdef UE4DEBUG
    // PRINT_ERR("before send_packet\n");
#endif
    ret = ctx->net->send_packet(ctx, ctx->packet);
    if (ret < 0)
    {
#ifdef UE4DEBUG
    PRINT_ERR("send_packet return %d\n",ret);
#endif
        return -1;
    }
    // PRINT_ERR("print network stack time\n");
#ifdef TIME_NETWORK
    gettimeofday(&end, NULL);
    elapsed = end.tv_usec - start.tv_usec + (end.tv_sec - start.tv_sec) * 1000000 ;
    avg_send_time = 0.998 * avg_send_time + 0.002 * elapsed;
    fprintf(stderr, "send avg: %f, actual: %d\n", avg_send_time, elapsed);
#endif

    return 0;
}

int openuvr_cuda_copy(struct openuvr_context *context)
{
    struct ouvr_ctx *ctx = context->priv;
    if (ctx->enc->cuda_copy)
        ctx->enc->cuda_copy(ctx);
    return 0;
}

long prev_quot = 0;

void *send_loop_continuous(void *arg)
{
    struct openuvr_context *context = arg;
    struct ouvr_ctx *ctx = context->priv;
    ouvr_pthread_context *pth_ctx = ctx->main_priv;

    struct timespec cur_time;
    struct timespec wait_time = {.tv_sec = 0, .tv_nsec = 500000};

    long div = 1e9 / 60;
    long quot = 0;

    while (!pth_ctx->should_exit)
    {
        do
        {
            clock_gettime(CLOCK_MONOTONIC, &cur_time);
            quot = cur_time.tv_nsec / div;
            nanosleep(&wait_time, NULL);
        } while (quot == prev_quot);
        prev_quot = quot;

#ifdef UE4DEBUG
        // PRINT_ERR("before openuvr_send_frame(context)\n");
#endif
        if (openuvr_send_frame(context) != 0)
        {
            pth_ctx->should_exit = 1;
        }

        nanosleep(&wait_time, NULL);
    }

    return NULL;
}

int openuvr_init_thread_continuous(struct openuvr_context *context)
{
    ouvr_pthread_context *pth_ctx = malloc(sizeof(ouvr_pthread_context));
    pth_ctx->should_exit = 0;
    pthread_create(&pth_ctx->send_thread, NULL, send_loop_continuous, context);

    struct ouvr_ctx *ctx = context->priv;
    ctx->main_priv = pth_ctx;
    return 0;
}

void openuvr_close(struct openuvr_context *context)
{
    if (context == NULL || context->priv == NULL)
    {
        return;
    }
    struct ouvr_ctx *ctx = context->priv;
    ouvr_pthread_context *pth_ctx = ctx->main_priv;
    pth_ctx->should_exit = 1;
    pthread_join(pth_ctx->send_thread, NULL);
    ctx->enc->deinit(ctx);
    ctx->aud->deinit(ctx);

    free(ctx->main_priv);
    free(ctx);
    free(context);
}
