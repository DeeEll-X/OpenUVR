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
#include <fcntl.h>
#include <sys/types.h>
//#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
// for memset
#include <string.h>
// for ETH_P_802_EX1
#include <linux/if_ether.h>
// for sockaddr_ll
#include <linux/if_packet.h>

#include <time.h>

#define MY_SLL_IFINDEX 3

#define SEND_SIZE 1450

typedef struct raw_net_context
{
    uint8_t eth_header[14];
    int fd;
    struct sockaddr_ll raw_addr;
    struct msghdr msg;
    struct iovec iov[4];
} raw_net_context;

// MUD MAC + host MAC + 2 unchanged
// pi eth0 mac: e4:5f:01:be:a8:cf
// pi wlan mac: e4:5f:01:be:a8:d2
// host mac: d8:bb:c1:4a:07:b7
static uint8_t const global_eth_header[14] = {0xe4, 0x5f, 0x01, 0xbe, 0xa8, 0xcf, 0xd8, 0xbb, 0xc1, 0x4a, 0x07, 0xb7, 0x88, 0xb5};
// static uint8_t const global_eth_header[14] = {0xb8, 0x27, 0xeb, 0xce, 0x97, 0x68, 0x9c, 0xda, 0x3e, 0xa3, 0xd8, 0x29, 0x88, 0xb5};

static int raw_initialize(struct ouvr_ctx *ctx)
{
    if (ctx->net_priv != NULL)
    {
        free(ctx->net_priv);
    }
    raw_net_context *c = calloc(1, sizeof(raw_net_context));
    ctx->net_priv = c;
    memcpy(c->eth_header, global_eth_header, 14);
    c->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_802_EX1));
    if (c->fd < 0)
    {
        PRINT_ERR("Couldn't create socket\n");
        return -1;
    }

    //When you send packets, it is enough to specify sll_family, sll_addr, sll_halen, sll_ifindex, and sll_protocol.
    c->raw_addr.sll_family = AF_PACKET;
    //this is the hardcoded index of wlp2s0 device
    c->raw_addr.sll_ifindex = MY_SLL_IFINDEX;
    //ethertype ETH_P_802_EX1 (0x88b5) is reserved for private use
    c->raw_addr.sll_protocol = htons(ETH_P_802_EX1);
    memcpy(c->raw_addr.sll_addr, c->eth_header, 6);
    c->raw_addr.sll_halen = 6;
    c->raw_addr.sll_pkttype = PACKET_OTHERHOST;
    //msg.msg_flags = MSG_DONTWAIT;

    c->iov[0].iov_base = c->eth_header;
    c->iov[0].iov_len = sizeof(c->eth_header);

    // int flags = fcntl(fd, F_GETFL, 0);
    // fcntl(fd, F_SETFL, flags | (int)O_NONBLOCK);

    c->msg.msg_name = &c->raw_addr;
    c->msg.msg_namelen = sizeof(c->raw_addr);
    c->msg.msg_control = 0;
    c->msg.msg_controllen = 0;
    c->msg.msg_iov = c->iov;
    c->msg.msg_iovlen = 4;
    return 0;
}

static int raw_send_packet(struct ouvr_ctx *ctx, struct ouvr_packet *pkt)
{
    raw_net_context *c = ctx->net_priv;
    register ssize_t r;
    uint8_t *start_pos = pkt->data;
    int offset = 0;
    struct timeval tv;
    struct timevalue sending_tv;
    gettimeofday(&tv, NULL);
    sending_tv.sec = tv.tv_sec;
    sending_tv.usec = tv.tv_usec;
    c->iov[1].iov_len = sizeof(pkt->size);
    c->iov[1].iov_base = &(pkt->size);
    c->iov[2].iov_len = sizeof(sending_tv);
    c->iov[2].iov_base = &(sending_tv);
    c->iov[3].iov_len = SEND_SIZE;
    while (offset < pkt->size)
    {
        c->iov[3].iov_base = start_pos + offset;
        r = sendmsg(c->fd, &c->msg, 0);
        if (r < -1)
        {
            PRINT_ERR("sendmsg returned %ld\n", r);
            return -1;
        }
        else if (r > 0)
        {
            offset += c->iov[2].iov_len;
            if (offset + SEND_SIZE > pkt->size)
            {
                c->iov[3].iov_len = pkt->size - offset;
            }
        }
    }
    return 0;
}

static void raw_deinitialize(struct ouvr_ctx *ctx)
{
    raw_net_context *c = ctx->net_priv;
    close(c->fd);
    free(ctx->net_priv);
}

struct ouvr_network raw_handler = {
    .init = raw_initialize,
    .send_packet = raw_send_packet,
    .deinit = raw_deinitialize,
};
