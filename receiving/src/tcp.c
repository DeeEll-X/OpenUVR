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
#include "udp.h"
#include "ouvr_packet.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
// for memset
#include <string.h>

#include <time.h>
#include <sys/time.h>

#define SERVER_PORT_BUFFER 21221
#define CLIENT_PORT_BUFFER 21222

typedef struct tcp_net_context
{
    int fd;
    struct sockaddr_in serv_addr, cli_addr;
} tcp_net_context;

static int tcp_initialize(struct ouvr_ctx *ctx)
{
    if (ctx->net_priv != NULL)
    {
        free(ctx->net_priv);
    }
    tcp_net_context *c = calloc(1, sizeof(tcp_net_context));
    ctx->net_priv = c;

    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) {
        printf("Couldn't create socket\n");
        return -1;
    }

    int reuseaddr_val = 1;
    setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_val, sizeof(int));
    
    c->serv_addr.sin_family = AF_INET;
    inet_pton(AF_INET, SERVER_IP, &c->serv_addr.sin_addr.s_addr);
    c->serv_addr.sin_port = htons(SERVER_PORT_BUFFER);

    c->cli_addr.sin_family = AF_INET;
    inet_pton(AF_INET, CLIENT_IP, &c->cli_addr.sin_addr.s_addr);
    c->cli_addr.sin_port = htons(CLIENT_PORT_BUFFER);

    if (bind(c->fd, (struct sockaddr *)&c->cli_addr, sizeof(c->cli_addr)) < 0) {
        printf("Couldn't bind\n");
        return -1;
    }
    if (connect(c->fd, (struct sockaddr *)&c->serv_addr, sizeof(c->serv_addr)) < 0) {
        printf("Couldn't connect\n");
        return -1;
    }

    
    return 0;
}

#ifdef TIME_NETWORK
    static float avg_recv_time = 0;
    static float avg_transfer_time = 0;
#endif

static int tcp_receive_packet(struct ouvr_ctx *ctx, struct ouvr_packet *pkt)
{
#ifdef TIME_NETWORK
    struct timeval start_time, end_time;
    int has_received_first = 0;
#endif
    tcp_net_context *c = ctx->net_priv;

    struct timevalue sending_tv;

    register int r;
    unsigned char *pos;
    int nleft = 0;
    int size;

    size = sizeof(nleft);
    pos = &nleft;
    while(size > 0)
    {
        r = read(c->fd, pos, size);
        if (r < 0){
            printf("reading nleft error: %d, errno: %d\n", r, errno);
            return -1;
        }
        else if (r > 0){
            pos += r;
            size -= r;
        }
    }
    pkt->size = nleft;

    size = sizeof(sending_tv);
    pos = &sending_tv;
    while ((size > 0))
    {
        r = read(c->fd, pos, size);
        if(r < 0){
            printf("reading sending time error: %d, errno: %d\n", r, errno);
            return -1;
        }
        else if(r > 0){
            pos += r;
            size -= r;
        }
    }

#ifdef TIME_NETWORK
    gettimeofday(&start_time, NULL);
#endif

    pos = pkt->data;
    while (nleft > 0)
    {
        r = read(c->fd, pos, nleft);
        if (r < -1)
        {
            printf("Reading error: %d\n", r);
            return -1;
        }
        else if (r > 0)
        {
            pos += r;
            nleft -= r;
        }
    }

#ifdef TIME_NETWORK
    gettimeofday(&end_time, NULL);
    long recved = end_time.tv_usec - start_time.tv_usec + (end_time.tv_sec - start_time.tv_sec)* 1000000;
    avg_recv_time = 0.998 * avg_recv_time + 0.002 * recved;
    printf("total recv avg: %f, recv elapsed: %ld\n", avg_recv_time, recved);

    long transfered = end_time.tv_usec - sending_tv.usec + (end_time.tv_sec - sending_tv.sec) * 1000000;
    avg_transfer_time = 0.998 * avg_transfer_time + 0.002 * transfered;
    printf("sendtime: sec: %ld, usec: %ld, endtime: sec: %ld, usec: %ld\n", sending_tv.sec, sending_tv.usec, end_time.tv_sec, end_time.tv_usec);
    printf("total transfer avg: %f, transfered: %ld\n", avg_transfer_time, transfered);
#endif

    return 0;
}

struct ouvr_network tcp_handler = {
    .init = tcp_initialize,
    .recv_packet = tcp_receive_packet,
};
