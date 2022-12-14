
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

#include "include/libavutil/frame.h"
#include "include/libavutil/pixfmt.h"
#include "include/libavcodec/avcodec.h"
#include "include/libavutil/avutil.h"
#include "include/libavutil/opt.h"
#include "include/libavutil/imgutils.h"
#include "include/libavutil/hwcontext.h"
#include "include/libavutil/hwcontext_cuda.h"
#include <cuda.h>
#include <npp.h>

#include "ffmpeg_cuda_encode.h"
#include "ouvr_packet.h"

#define WIDTH 1920
#define HEIGHT 1080

// #define USE_YUV

#ifdef USE_YUV
#define PIX_FMT AV_PIX_FMT_YUV420P
#else
#define PIX_FMT AV_PIX_FMT_0BGR32
#endif

typedef struct ffmpeg_cuda_encode_context
{
    AVCodecContext *enc_ctx;
    AVFrame *frame;
    int idx;
    CUcontext *cuda_ctx;
    CUDA_MEMCPY2D memCpyStruct;
    CUgraphicsResource resource;
} ffmpeg_cuda_encode_context;

static int ffmpeg_initialize(struct ouvr_ctx *ctx)
{
    int ret;
    if (ctx->enc_priv != NULL)
    {
        PRINT_ERR("Cannot initialize ffmpeg CUDA because encoder is already initialized\n");
        return -1;
    }
    ffmpeg_cuda_encode_context *e = calloc(1, sizeof(ffmpeg_cuda_encode_context));
    ctx->enc_priv = e;
    AVCodec *enc = avcodec_find_encoder_by_name("h264_nvenc");
    if (enc == NULL)
    {
        PRINT_ERR("Couldn't find encoder\n");
        return -1;
    }
    e->enc_ctx = avcodec_alloc_context3(enc);
    e->enc_ctx->width = WIDTH;
    e->enc_ctx->height = HEIGHT;
    e->enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    e->enc_ctx->framerate = (AVRational){60, 1};
    e->enc_ctx->time_base = (AVRational){1, 60};
    e->enc_ctx->bit_rate = 15000000;
    e->enc_ctx->gop_size = 480;
    e->enc_ctx->max_b_frames = 0;
    e->enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
    e->enc_ctx->sw_pix_fmt = PIX_FMT;
    ret = av_opt_set(e->enc_ctx->priv_data, "preset", "llhq", 0);
    ret = av_opt_set(e->enc_ctx->priv_data, "rc", "cbr_ld_hq", 0);
    ret = av_opt_set_int(e->enc_ctx->priv_data, "zerolatency", 1, 0);
    ret = av_opt_set_int(e->enc_ctx->priv_data, "delay", 0, 0);
    // "Instantaneous Decoder Refresh", so that when we generate I-frames mid-sequence, they will reset the reference picture buffer,
    // allowing the client to join and immediately begin rendering when it requests I-frames.
    ret = av_opt_set(e->enc_ctx->priv_data, "forced-idr", "true", 0);
    ret = av_opt_set_int(e->enc_ctx->priv_data, "temporal-aq", 1, 0);
    ret = av_opt_set_int(e->enc_ctx->priv_data, "spatial-aq", 1, 0);
    ret = av_opt_set_int(e->enc_ctx->priv_data, "aq-strength", 15, 0);

    //
    //

    ret = av_hwdevice_ctx_create(&e->enc_ctx->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, "Quadro P4000", NULL, 0);
    if (ret != 0)
    {
        PRINT_ERR("hwdevice_ctx_create failed %d\n", ret);
        return -1;
    }
    AVHWDeviceContext *thing1 = (AVHWDeviceContext *)(e->enc_ctx->hw_device_ctx->data);
    AVCUDADeviceContext *thing2 = (AVCUDADeviceContext *)(thing1->hwctx);
    e->cuda_ctx = &(thing2->cuda_ctx);

    e->enc_ctx->hw_frames_ctx = av_hwframe_ctx_alloc(e->enc_ctx->hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)e->enc_ctx->hw_frames_ctx->data;
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = PIX_FMT;
    frames_ctx->width = WIDTH;
    frames_ctx->height = HEIGHT;
    frames_ctx->device_ref = e->enc_ctx->hw_device_ctx;
    frames_ctx->device_ctx = (AVHWDeviceContext *)e->enc_ctx->hw_device_ctx->data;
    ret = av_hwframe_ctx_init(e->enc_ctx->hw_frames_ctx);
    if (ret < 0)
    {
        PRINT_ERR("av_hwframe_ctx_init failed\n");
        return -1;
    }

    CUcontext oldctx;
    CUresult err = cuCtxPopCurrent(&oldctx);
    err = cuCtxPushCurrent(*(e->cuda_ctx));
    err = cuGraphicsGLRegisterBuffer(&e->resource, ctx->pbo_handle, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if (err != 0)
    {
        PRINT_ERR("error in registering GL buffer: %d\n", err);
        exit(1);
    }

    err = cuCtxPopCurrent(&oldctx);

    //
    //

    ret = avcodec_open2(e->enc_ctx, enc, NULL);
    if (ret < 0)
    {
        PRINT_ERR("avcodec_open2 failed\n");
        return -1;
    }

    e->frame = av_frame_alloc();
    e->frame->format = e->enc_ctx->pix_fmt;
    e->frame->width = e->enc_ctx->width;
    e->frame->height = e->enc_ctx->height;

    // printf("before: %p\n", e->frame->data[0]);
    av_hwframe_get_buffer(e->enc_ctx->hw_frames_ctx, e->frame, 0);
    // printf("after: %p\n", e->frame->data[0]);
    // printf("linesize: %d\n", e->frame->linesize[0]);

    e->memCpyStruct.srcXInBytes = 0;
    e->memCpyStruct.srcY = 0;
    e->memCpyStruct.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    e->memCpyStruct.dstXInBytes = 0;
    e->memCpyStruct.dstY = 0;
    e->memCpyStruct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

    return 0;
}

static int ffmpeg_process_frame(struct ouvr_ctx *ctx, struct ouvr_packet *pkt)
{
    int ret;
    ffmpeg_cuda_encode_context *e = ctx->enc_priv;
    AVFrame *frame = e->frame;
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;
    CUresult err;

    ret = avcodec_receive_packet(e->enc_ctx, &packet);
    if (ret >= 0)
    {
        // This takes ~5 microseconds, so nothing to worry about
        // memcpy(pkt->data, packet.data, packet.size);
        // This can be ~ 10 microseconds faster, but not memory safe probably:
        pkt->data = packet.data;
        pkt->size = packet.size;
        return 1;
    }
    else if (ret != -11)
    {
        PRINT_ERR("avcodec_receive_packet error: %d\n", ret);
        return -1;
    }

    CUcontext oldctx;
    err = cuCtxPopCurrent(&oldctx);
    err = cuCtxPushCurrent(*(e->cuda_ctx));

/* TODO: see if saturation can be fixed on NPPI conversion to YUV420 */
#ifdef USE_YUV
    NppiSize fdsa;
    fdsa.width = WIDTH;
    fdsa.height = HEIGHT;
    nppiRGBToYUV420_8u_C3P3R(e->memCpyStruct.srcDevice, WIDTH * 3, e->frame->data, e->frame->linesize, fdsa);
#else
    err = cuMemcpy2D(&e->memCpyStruct);
    if (err != 0)
    {
        PRINT_ERR("cuMemcpy2D err=%d\n", err);
        exit(1);
    }
#endif


    // err = cuMemcpy((CUdeviceptr)e->frame->data[0],srcDevicePtr, WIDTH*HEIGHT*4);
    //printf("memcpy: %d %d\n", err, e->frame->linesize[0]);

    cuCtxPopCurrent(&oldctx);

    frame->pts = e->idx++;
    if (ctx->flag_send_iframe > 0)
    {
        frame->pict_type = AV_PICTURE_TYPE_I;
        ctx->flag_send_iframe = ~ctx->flag_send_iframe;
    }
    else
    {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        if (ctx->flag_send_iframe < 0)
        {
            ctx->flag_send_iframe++;
        }
    }

    ret = avcodec_send_frame(e->enc_ctx, frame);
    if (ret != 0 && ret != -11)
    {
        PRINT_ERR("avcodec_send_frame() failed: %d\n", ret);
        return -1;
    }

    return 0;
}
static void ffmpeg_cuda_copy(struct ouvr_ctx *ctx)
{
    ffmpeg_cuda_encode_context *e = ctx->enc_priv;
    CUcontext oldctx;
    CUdeviceptr srcDevPtr;
    CUresult err = cuCtxPopCurrent(&oldctx);
    err = cuCtxPushCurrent(*(e->cuda_ctx));
    CUgraphicsResource resource = e->resource;

    err = cuGraphicsResourceSetMapFlags(resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    if (err != 0)
    {
        PRINT_ERR("setMapFlags err=%d\n", err);
        exit(1);
    }

    err = cuGraphicsMapResources(1, &resource, 0);
    if (err != 0)
    {
        PRINT_ERR("mapResources err=%d\n", err);
        exit(1);
    }

    size_t _ = 0;
    err = cuGraphicsResourceGetMappedPointer(&srcDevPtr, &_, resource);
    if (err != 0)
    {
        PRINT_ERR("getMappedArray err=%d\n", err);
        exit(1);
    }

    err = cuGraphicsUnmapResources(1, &resource, 0);
    if (err != 0)
    {
        PRINT_ERR("unmapResources err=%d\n", err);
        exit(1);
    }

    e->memCpyStruct.srcDevice = srcDevPtr;
    e->memCpyStruct.dstDevice = (CUdeviceptr)e->frame->data[0];
    e->memCpyStruct.dstPitch = e->frame->linesize[0];
    e->memCpyStruct.WidthInBytes = WIDTH * 4;
    e->memCpyStruct.Height = HEIGHT;

    err = cuCtxPopCurrent(&oldctx);
}

static void ffmpeg_deinitialize(struct ouvr_ctx *ctx)
{
    ffmpeg_cuda_encode_context *e = ctx->enc_priv;
    CUresult err = cuGraphicsUnregisterResource(e->resource);
    if (err != 0)
    {
        PRINT_ERR("ffmpeg_deinitialize failed at cuGraphicsUnregisterResource, err=%d\n", err);
        exit(1);
    }
    avcodec_free_context(&e->enc_ctx);
    free(e);
    ctx->enc_priv = NULL;
}

struct ouvr_encoder ffmpeg_cuda_encode = {
    .init = ffmpeg_initialize,
    .process_frame = ffmpeg_process_frame,
    .cuda_copy = ffmpeg_cuda_copy,
    .deinit = ffmpeg_deinitialize,
};