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
#include "raw.h"
#include "ouvr_packet.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
// for memset
#include <string.h>
// for ETH_P_802_EX1
#include <linux/if_ether.h>

#include <time.h>
#include <sys/time.h>

#define RECV_SIZE 1450

typedef struct raw_net_context
{
    unsigned char eth_header[14];
    int fd;
    struct msghdr msg;
    struct iovec iov[4];
} raw_net_context;

unsigned char const global_eth_header[14] = {0x9c, 0xda, 0x3e, 0xa3, 0xd8, 0x29, 0xb8, 0x27, 0xeb, 0xce, 0x97, 0x68, 0x88, 0xb5};

static int raw_initialize(struct ouvr_ctx *ctx)
{
    if (ctx->net_priv != NULL)
    {
        free(ctx->net_priv);
    }
    raw_net_context *c = calloc(1, sizeof(raw_net_context));
    ctx->net_priv = c;

    memcpy(c->eth_header, global_eth_header, sizeof(c->eth_header));
    c->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_802_EX1));
    if (c->fd < 0){
        printf("Couldn't create socket\n");
        return -1;
    }

    int flags = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, flags | (int)O_NONBLOCK);
    
    c->iov[0].iov_base = c->eth_header;
    c->iov[0].iov_len = sizeof(c->eth_header);
    srand(17); 
    c->msg.msg_iov = c->iov;
    c->msg.msg_iovlen = 4;
    return 0;
}

#ifdef TIME_NETWORK
    static float avg_time = 0;
    static float avg_transfer_time = 0;
#endif

static int raw_receive_packet(struct ouvr_ctx *ctx, struct ouvr_packet *pkt)
{
#ifdef TIME_NETWORK
    struct timeval start_time, end_time;
    int has_received_first = 0;
#endif
    raw_net_context *c = ctx->net_priv;
    register ssize_t r;
    unsigned char *start_pos = pkt->data;
    int offset = 0;
    int expected_size = 1;
    struct timespec time_of_last_receive = {.tv_sec = 0}, time_temp;
    struct timevalue sending_tv;
    pkt->size = 0;
    c->iov[1].iov_len = sizeof(expected_size);
    c->iov[1].iov_base = &expected_size;
    c->iov[2].iov_len = sizeof(sending_tv);
    c->iov[2].iov_base = &sending_tv;
    c->iov[3].iov_len = RECV_SIZE;
    while (offset < expected_size)
    {
        c->iov[3].iov_base = start_pos + offset;
        r = recvmsg(c->fd, &c->msg, 0);
        if (r < -1)
        {
            printf("Reading error: %d\n", r);
            return -1;
        }
        else if (r > 0)
        {
#ifdef TIME_NETWORK
            if(!has_received_first){
                gettimeofday(&start_time, NULL);
                has_received_first = 1;
            }
#endif
            offset += r - (sizeof(c->eth_header) + sizeof(expected_size) + sizeof(sending_tv));
            pkt->size += r - (sizeof(c->eth_header) + sizeof(expected_size) + sizeof(sending_tv));
            clock_gettime(CLOCK_MONOTONIC, &time_of_last_receive);
        }
        else
        {
            clock_gettime(CLOCK_MONOTONIC, &time_temp);
            long elapsed = time_temp.tv_nsec - time_of_last_receive.tv_nsec;
            if(time_temp.tv_sec > time_of_last_receive.tv_sec)
                elapsed += 1000000000;
            if(elapsed > 3000000 && time_of_last_receive.tv_sec > 0){
                pkt->size = 0;
                //printf("dropped %ld\n", time_of_last_receive.tv_nsec);
                ctx->flag_send_iframe = 5;
                break;
            }
        }
    }
#ifdef TIME_NETWORK
    gettimeofday(&end_time, NULL);
    long elapsed = end_time.tv_usec - start_time.tv_usec + (end_time.tv_sec > start_time.tv_sec ? 1000000 : 0);
    avg_time = 0.998 * avg_time + 0.002 * elapsed;
    printf("\rnet avg: %f,  elapsed: %ld", avg_time, elapsed);

    long transfered = end_time.tv_usec - sending_tv.usec + (end_time.tv_sec - sending_tv.sec) * 1000000;
    avg_transfer_time = 0.998 * avg_transfer_time + 0.002 * transfered;
    printf("sendtime: sec: %d, usec: %d, endtime: sec: %ld, usec: %ld\n", sending_tv.sec, sending_tv.usec, end_time.tv_sec, end_time.tv_usec);
    printf("\rtotal transfer avg: %f, transfered: %ld\n", avg_transfer_time, transfered);
#endif
#if 0
    if(!(rand() % 120)){
	    pkt->size = 0;
	    printf("dropped\n");
	    ctx->flag_send_iframe = 5;
    }
#endif
    return 0;
}

struct ouvr_network raw_handler = {
    .init = raw_initialize,
    .recv_packet = raw_receive_packet,
};

#include "openmax_render.h"
int raw_receive_and_decode(struct ouvr_ctx *ctx)
{
    OMX_BUFFERHEADERTYPE *bufs[5];
    omxr_get_available_buffers(bufs, 5);

    raw_net_context *c = ctx->net_priv;
    register ssize_t r;
    int total_received = 0;
    int expected_size = 1;
    struct timespec time_of_last_receive = {.tv_sec = 0}, time_temp;
    struct timevalue sending_tv;
    c->iov[1].iov_len = sizeof(expected_size);
    c->iov[1].iov_base = &expected_size;
    c->iov[2].iov_len = sizeof(sending_tv);
    c->iov[2].iov_base = &sending_tv;
    c->iov[3].iov_len = RECV_SIZE;

    int buf_idx = 0;
    int buf_offset = 0;
    int max_buf_size = bufs[buf_idx]->nAllocLen - RECV_SIZE;
    while(total_received < expected_size) {
        c->iov[3].iov_base = bufs[buf_idx]->pBuffer + buf_offset;
        r = recvmsg(c->fd, &c->msg, 0);
        if (r < -1)
        {
            printf("Reading error: %d\n", r);
            return -1;
        }
        else if (r > 0)
        {
            int to_add = r - (sizeof(c->eth_header) + sizeof(expected_size) + sizeof(sending_tv));
            buf_offset += to_add;
            total_received += to_add;
            clock_gettime(CLOCK_MONOTONIC, &time_of_last_receive);
        }
        else
        {
            clock_gettime(CLOCK_MONOTONIC, &time_temp);
            long elapsed = time_temp.tv_nsec - time_of_last_receive.tv_nsec;
            if(time_temp.tv_sec > time_of_last_receive.tv_sec)
                elapsed += 1000000000;
            if(elapsed > 3000000 && time_of_last_receive.tv_sec > 0){
                ctx->flag_send_iframe = 5;
                return -1;
            }
        }
        if(buf_offset >= max_buf_size && total_received < expected_size) {
            bufs[buf_idx]->nFilledLen = buf_offset;
            buf_idx++;
            buf_offset = 0;
            max_buf_size = bufs[buf_idx]->nAllocLen - RECV_SIZE;
        }
    }
    if(buf_idx >= 5){
        printf("buf_idx: %d, exp_size: %d\n", buf_idx, expected_size);
        exit(1);
    }
    if(bufs[0]->pBuffer[4] != 0x6) {
        ctx->flag_send_iframe = 0;
    }
    bufs[buf_idx]->nFilledLen = buf_offset;
    omxr_empty_buffers(bufs, buf_idx + 1);

    return 0;
}
