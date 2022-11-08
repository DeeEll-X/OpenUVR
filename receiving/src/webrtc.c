#include "webrtc.h"
#include "ouvr_packet.h"
#include "feedback_net.h"
#include <rtc/rtc.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

static int websocket_port = 30888;

static void RTC_API wbOpenCallback(int id, void *ptr);
static void RTC_API wbClosedCallback(int id, void *ptr);
static void RTC_API wbErrorCallback(int id, const char *error, void *ptr);
static void RTC_API wbMessageCallback(int id, const char *message, int size, void *ptr);
static void RTC_API wbServerClientCallbackFunc(int wsserver, int ws, void *ptr);

static void RTC_API descriptionCallback(int pc, const char *sdp, const char *type, void *ptr);
static void RTC_API candidateCallback(int pc, const char *cand, const char *mid, void *ptr);
static void RTC_API stateChangeCallback(int pc, rtcState state, void *ptr);
static void RTC_API gatheringStateCallback(int pc, rtcGatheringState state, void *ptr);

static void RTC_API trClosedCallback(int id, void *ptr);
static void RTC_API trMessageCallback(int id, const char *message, int size, void *ptr);

static char *rtcGatheringState_print(rtcGatheringState state);
static char *state_print(rtcState state);

typedef uint32_t SSRC;

static SSRC ssrc = 42;

typedef struct {
    struct ouvr_ctx *ctx;
	rtcState state;
	rtcGatheringState gatheringState;
	int pc;
	int tr;
	int ws;
} Peer;

static Peer PEER;


static int webrtc_initialize(struct ouvr_ctx *ctx)
{
    printf("enter webrtc initialize()\n");
    fflush(stdout);
    rtcInitLogger(RTC_LOG_INFO, NULL);

	// Create peer
	rtcConfiguration config;
	memset(&config, 0, sizeof(config));

	memset(&PEER, 0, sizeof(Peer));
    PEER.ctx = ctx;
	PEER.pc = rtcCreatePeerConnection(&config);
	fprintf(stderr, "Peer %d created\n", PEER.pc);

	if (PEER.pc <= 0) {
		fprintf(stderr, "Create peer connection failed, error = %d\n", PEER.pc);
		return -1;
	}

	rtcSetUserPointer(PEER.pc, &PEER);
	rtcSetLocalDescriptionCallback(PEER.pc, descriptionCallback);
	rtcSetLocalCandidateCallback(PEER.pc, candidateCallback);
	rtcSetStateChangeCallback(PEER.pc, stateChangeCallback);
	rtcSetGatheringStateChangeCallback(PEER.pc, gatheringStateCallback);

	rtcTrackInit trackInit = {
		RTC_DIRECTION_RECVONLY,
		RTC_CODEC_H264,
		96,
		ssrc,
		"video",
		NULL,
		NULL,
		NULL,
	};
	PEER.tr = rtcAddTrackEx(PEER.pc, &trackInit);
	rtcSetUserPointer(PEER.tr, &PEER);
	rtcSetClosedCallback(PEER.tr, trClosedCallback);
	rtcSetMessageCallback(PEER.tr, trMessageCallback);
	rtcSetLocalDescription(PEER.pc, NULL);

	char url[40];
    sprintf(url, "ws://%s:%d/%d", SERVER_IP, websocket_port, 4);
    printf("websocket url:%s\n",url);
	PEER.ws = rtcCreateWebSocket(url);

	if(PEER.ws < 0) {
		fprintf(stderr, "Create websocket failed, error = %d\n", PEER.ws);
		return -1;
	}

	rtcSetUserPointer(PEER.ws, &PEER);
	rtcSetOpenCallback(PEER.ws, wbOpenCallback);
	rtcSetClosedCallback(PEER.ws, wbClosedCallback);
	rtcSetErrorCallback(PEER.ws, wbErrorCallback);
	rtcSetMessageCallback(PEER.ws, wbMessageCallback);
	printf("ws callback added\n");
    return 0;
}

static int webrtc_receive_packet(struct ouvr_ctx *ctx, struct ouvr_packet *pkt)
{
    return 0;
}

struct ouvr_network webrtc_handler = {
    .init = webrtc_initialize,
    .recv_packet = webrtc_receive_packet,
};


static void RTC_API wbOpenCallback(int id, void *ptr)
{
	// Peer* peer = (Peer *)ptr;
	// char buffer[1024];
	// buffer[0] = 'a';
	// int size = rtcGetLocalDescription(peer->pc, &buffer[1], 1023);
	// rtcSendMessage(peer->ws,buffer, size+1);
	// buffer[1023] = 0;
	// printf("WebSocket %d connected, sent description\n%s\n", id, buffer);
}

static void RTC_API wbClosedCallback(int id, void *ptr)
{
	printf("WebSocket %d closed\n", id);
}

static void RTC_API wbErrorCallback(int id, const char *error, void *ptr)
{
	printf("WebSocket failed: error %s\n", error);
}

static void RTC_API wbMessageCallback(int id, const char *message, int size, void *ptr)
{
	Peer* peer = (Peer *)ptr;
    printf("websocket reveiced message\n");
	if(message[0] == 'o') {
		rtcSetRemoteDescription(peer->pc, message+1, "offer");
		printf("WebSocket received offer description\n%-*s\n", size, message);
		char buffer[1024];
		buffer[0] = 'a';
		int size = rtcGetLocalDescription(peer->pc, &buffer[1], 1023);
		rtcSendMessage(peer->ws, buffer, size+1);
	} else {
		// TODO
	}
}

static void RTC_API descriptionCallback(int pc, const char *sdp, const char *type, void *ptr) {
	printf("Description %s:\n%s\n", type, sdp);
}

static void RTC_API candidateCallback(int pc, const char *cand, const char *mid, void *ptr) {
	printf("Candidate %s: %s\n", mid, cand);
}

static void RTC_API stateChangeCallback(int pc, rtcState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->state = state;

	if (state == RTC_FAILED || state == RTC_DISCONNECTED)
	{
		peer->pc = 0;
	}
}

static void RTC_API gatheringStateCallback(int pc, rtcGatheringState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->gatheringState = state;
	printf("Gathering state %s: %s\n", "offerer", rtcGatheringState_print(state));
}

static void RTC_API trClosedCallback(int id, void *ptr)
{
	printf("Track %d closed\n", id);
}

#ifdef TIME_NETWORK
	static float avg_transfer_time = 0;
#endif

static void RTC_API trMessageCallback(int id, const char *message, int size, void *ptr)
{
	printf("tr receive message size = %d\n", size);
#ifdef TIME_NETWORK
	uint32_t timestamp = *((uint32_t *)(message+4));
	printf("timestamp: %d\n", timestamp);
	struct timeval curtime;
	gettimeofday(&curtime, NULL);
	long transfered = ((curtime.tv_sec%3600) - timestamp / 90000) * 1000000 + curtime.tv_usec - (timestamp % 90000) * 100/9;
	avg_transfer_time = 0.998 * avg_transfer_time + 0.002 * transfered;
	// printf("total transfer avg: %f, transfered: %ld\n", avg_transfer_time, transfered);
#endif
	Peer* peer = (Peer *)ptr;
    struct ouvr_ctx *ctx = peer->ctx;
    while(ctx->packets == NULL){
		printf("ctx->packets not initialized\n");
		sleep(1);
	}
    struct ouvr_packet *pkt = ctx->packets[1];
	// memcpy(pkt->data, message, size);
	pkt->data = message;
	pkt->size = size;
	if(pkt->size && ctx->dec->process_frame(ctx, pkt)!=0){
		printf("trMessageCallback: decoder->process_frame failed!\n");
	}
	// feedback_send(ctx);
}

static char *rtcGatheringState_print(rtcGatheringState state) {
	char *str = NULL;
	switch (state) {
	case RTC_GATHERING_NEW:
		str = "RTC_GATHERING_NEW";
		break;
	case RTC_GATHERING_INPROGRESS:
		str = "RTC_GATHERING_INPROGRESS";
		break;
	case RTC_GATHERING_COMPLETE:
		str = "RTC_GATHERING_COMPLETE";
		break;
	default:
		break;
	}

	return str;
}
static char *state_print(rtcState state) {
	char *str = NULL;
	switch (state) {
	case RTC_NEW:
		str = "RTC_NEW";
		break;
	case RTC_CONNECTING:
		str = "RTC_CONNECTING";
		break;
	case RTC_CONNECTED:
		str = "RTC_CONNECTED";
		break;
	case RTC_DISCONNECTED:
		str = "RTC_DISCONNECTED";
		break;
	case RTC_FAILED:
		str = "RTC_FAILED";
		break;
	case RTC_CLOSED:
		str = "RTC_CLOSED";
		break;
	default:
		break;
	}

	return str;
}