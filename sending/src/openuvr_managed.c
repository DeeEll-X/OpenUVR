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
 * Handles all the nitty-gritty of OpenGL.
 * Call openuvr_managed_init() from a location where the active OpenGL context is the context that includes the game screen's framebuffer.
 * Call openuvr_managed_copy_framebuffer() from a function which is called at every frame and also has the same OpenGL context as openuvr_managed_init().
 * If the host program uses SDL, you can put this function immediately before SDL_GL_SwapWindow() since it reads from the back buffer, so calling it in this way will copy the pixels beofre they're displayed on the screen.
 */
#include "openuvr.h"
#include "ouvr_packet.h"
#include <stdlib.h>
#include <stdio.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <sys/time.h>

#include "ssim_plugin.h"

//To enable SSIM measurement, compile with "make MODE_MEASURE_SSIM=1"
#ifdef MEASURE_SSIM
#include "include/libavcodec/avcodec.h"
#include "include/libavutil/avutil.h"
#include "include/libavutil/opt.h"
#include "include/libavutil/imgutils.h"
#include "include/libswscale/swscale.h"

#define NUM_SAVED_FRAMES 150

static AVFrame *frame = NULL;
static int const srcstride[1] = {1920 * 4};
struct SwsContext *swsctx;
static uint8_t *saved_frames[NUM_SAVED_FRAMES];
static int cur_frame = 0;
static int num_intermediary_frames = 3000;
static uint8_t *y_buf;
#endif

static GLuint pbo = 0;
static uint8_t *cpu_encoding_buf = NULL;
struct openuvr_context *ctx = NULL;

int openuvr_managed_init(enum OPENUVR_ENCODER_TYPE enc_type, enum OPENUVR_NETWORK_TYPE net_type)
{
#ifdef UE4DEBUG
    PRINT_ERR("openuvr_managed_init entering\n");
#endif
    GLint dims[4] = {0};
    glGetIntegerv(GL_VIEWPORT, dims);
    GLint w = dims[2];
    GLint h = dims[3];
    if (w != 1920 || h != 1080)
    {
        PRINT_ERR("Viewport dimensions are %dx%d. You must set the dimensions to 1920x1080.\n", w, h);
        return -1;
    }

    if (enc_type == OPENUVR_ENCODER_H264_CUDA)
    {

        glGenBuffers(1, &pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, 1920 * 1080 * 4, 0, GL_DYNAMIC_COPY);
    }
    else
    {

        cpu_encoding_buf = (uint8_t *)malloc(1920 * 1080 * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, cpu_encoding_buf);
    }

    GLint size = 0;
    glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &size);
#ifdef UE4DEBUG
    // PRINT_ERR("openuvr_alloc_context entering\n");
#endif
    ctx = openuvr_alloc_context(enc_type, net_type, cpu_encoding_buf, pbo);
#ifdef UE4DEBUG
    PRINT_ERR("openuvr_alloc_context finished\n");
#endif
    if (ctx == NULL)
    {
        PRINT_ERR("Couldn't allocate openuvr context\n");
        return -1;
    }
    openuvr_cuda_copy(ctx);
#ifdef UE4DEBUG
    PRINT_ERR("openuvr_cuda_copy finished\n");
#endif

//To enable SSIM measurement, compile with "make MODE_MEASURE_SSIM=1"
#ifdef MEASURE_SSIM
    // Required to run python later
    dlopen("libpython3.5m.so", RTLD_LAZY | RTLD_GLOBAL);

    frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 1920;
    frame->height = 1080;
    av_image_alloc(frame->data, frame->linesize, 1920, 1080, AV_PIX_FMT_YUV420P, 32);
    swsctx = sws_getContext(1920, 1080, AV_PIX_FMT_RGB0, 1920, 1080, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    for (int i = 0; i < NUM_SAVED_FRAMES; i++)
        saved_frames[i] = malloc(1920 * 1080 * 4);
    y_buf = malloc(1920 * 1080);
#else
    openuvr_init_thread_continuous(ctx);
#endif
#ifdef UE4DEBUG
    PRINT_ERR("openuvr_managed_init finished\n");
#endif
    return 0;
}

#ifdef TIME_COPY
float avg_copy_time = 0;
#endif
void openuvr_managed_copy_framebuffer()
{
#ifdef TIME_COPY
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif
    GLuint bound_pbo;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, (GLint *)&bound_pbo);
    if (pbo != 0)
    {
        if (bound_pbo != pbo)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, 1920 * 1080 * 4, 0, GL_DYNAMIC_COPY);
        }

        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, 0);

//To enable SSIM measurement, compile with "make MODE_MEASURE_SSIM=1"
#ifdef MEASURE_SSIM
        if (num_intermediary_frames > 0)
        {
            num_intermediary_frames--;
            return;
        }

        uint8_t *pix = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 1920 * 1080 * 4, GL_MAP_READ_BIT);
        memcpy(saved_frames[cur_frame], pix, 1920 * 1080 * 4);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

        if (cur_frame == NUM_SAVED_FRAMES - 1)
        {
            for (int i = 0; i < NUM_SAVED_FRAMES; i++)
            {
                sws_scale(swsctx, &saved_frames[i], srcstride, 0, 1080, frame->data, frame->linesize);
                py_ssim_set_ref_image_data(frame->data[0]);

                uint8_t *pix = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 1920 * 1080 * 4, GL_MAP_WRITE_BIT);
                memcpy(pix, saved_frames[i], 1920 * 1080 * 4);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                openuvr_cuda_copy(ctx);

                openuvr_send_frame(ctx);
            }
            cur_frame = 0;
            num_intermediary_frames = NUM_SAVED_FRAMES * 6;
        }
        else
        {
            cur_frame++;
        }

#endif
    }
    else if (cpu_encoding_buf != 0)
    {
        glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, cpu_encoding_buf);

//To enable SSIM measurement, compile with "make MODE_MEASURE_SSIM=1"
#ifdef MEASURE_SSIM
        if (num_intermediary_frames > 0)
        {
            num_intermediary_frames--;
            return;
        }

        memcpy(saved_frames[cur_frame], cpu_encoding_buf, 1920 * 1080 * 4);

        if (cur_frame == NUM_SAVED_FRAMES - 1)
        {
            for (int i = 0; i < NUM_SAVED_FRAMES; i++)
            {
                sws_scale(swsctx, &saved_frames[i], srcstride, 0, 1080, frame->data, frame->linesize);
                py_ssim_set_ref_image_data(frame->data[0]);

                memcpy(cpu_encoding_buf, saved_frames[i], 1920 * 1080 * 4);
                openuvr_send_frame(ctx);
            }
            cur_frame = 0;
            num_intermediary_frames = NUM_SAVED_FRAMES * 6;
        }
        else
        {
            cur_frame++;
        }
#endif
    }
#ifdef TIME_COPY
    gettimeofday(&end, NULL);
    int elapsed = end.tv_usec - start.tv_usec + (end.tv_sec > start.tv_sec ? 1000000 : 0);
    avg_copy_time = 0.998 * avg_copy_time + 0.002 * elapsed;
    // PRINT_ERR("enc avg: %f, actual: %d\n", avg_enc_time, elapsed);
    fprintf(stderr, "enc copy: %f, actual: %d\n", avg_copy_time, elapsed);
    fflush(stdout);
#endif
}
