#include "webrtc.h"
#include "ouvr_packet.h"
#include "rtc/rtc.h"

static int websocket_port = 30888;

static void RTC_API wbOpenCallback(int id, void *ptr);
static void RTC_API wbClosedCallback(int id, void *ptr);
static void RTC_API wbErrorCallback(int id, const char *error, void *ptr);
static void RTC_API wbMessageCallback(int id, const char *message, int size, void *ptr);
static void RTC_API wbServerClientCallbackFunc(int wsserver, int ws, void *ptr);
static void RTC_API trClosedCallback(int id, void *ptr);

static void RTC_API pcClosedCallback(int id, void *ptr);

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
char *state_print(rtcState state) {
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

typedef uint32_t SSRC;
static SSRC ssrc = 42;

typedef struct {
	rtcState state;
	rtcGatheringState gatheringState;
	int pc;
	int tr;
	int ws;
} Peer;

static Peer peer;


static void RTC_API descriptionCallback(int pc, const char *sdp, const char *type, void *ptr) {
	printf("Description %s:\n%s\n", "offerer", sdp);
}

static void RTC_API candidateCallback(int pc, const char *cand, const char *mid, void *ptr) {
	printf("Candidate %s: %s\n", "offerer", cand);
}

static void RTC_API stateChangeCallback(int pc, rtcState state, void *ptr) {
	Peer *peer = (Peer *)ptr;
	peer->state = state;
	printf("State %s: %s\n", "offerer", state_print(state));

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


static int webrtc_initialize(struct ouvr_ctx *ctx)
{
    rtcInitLogger(RTC_LOG_DEBUG, NULL);

	memset(&peer, 0, sizeof(Peer));
	rtcWsServerConfiguration wsConfig = {
		websocket_port,
		false,
		NULL,
		NULL,
		NULL
	};

	int wssId = rtcCreateWebSocketServer(&wsConfig, &wbServerClientCallbackFunc);
	if (wssId < 0){
		fprintf(stderr, "Error creating websocket server: errorcode: %d\n", wssId);
		return -1;
	}
    return 0;
};

static int webrtc_send_packet(struct ouvr_ctx * ctx, struct ouvr_packet *pkt)
{
	if(peer.pc && peer.state == RTC_CONNECTED){
		rtcSendMessage(peer.tr, pkt->data, pkt->size);
	}
    return 0;
}

struct ouvr_network webrtc_handler = {
    .init = webrtc_initialize,
    .send_packet = webrtc_send_packet,
};

static void RTC_API wbOpenCallback(int id, void *ptr)
{
	Peer* peer = (Peer *)ptr;
	char buffer[1024];
	buffer[0] = 'o';
	int size = rtcGetLocalDescription(peer->pc, &buffer[1], 1023);
	rtcSendMessage(peer->ws,buffer, size+1);
	buffer[1023] = 0;
	printf("WebSocket %d connected, sent description\n%s\n", id, buffer);
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
	if(message[0] == 'a') {
		rtcSetRemoteDescription(peer->pc, message+1, "answer");
		printf("WebSocket received description\n%-*s\n", size, message);
		rtcClose(peer->ws);
	} else {
		// TODO
	}
}

static void RTC_API wbServerClientCallbackFunc(int wsserver, int ws, void *ptr)
{
	char address[256];
	if (rtcGetWebSocketRemoteAddress(ws, address, 256) < 0) {
		fprintf(stderr, "rtcGetWebSocketRemoteAddress failed\n");
		return;
	}

	// Create peer
	rtcConfiguration config;
	memset(&config, 0, sizeof(config));

	if (peer.pc == 0)
	{
		peer.pc = rtcCreatePeerConnection(&config);
		fprintf(stderr, "Peer %d created\n", peer.pc);
	} else 
	{
		rtcClose(ws);
		return;
	}

	rtcSetUserPointer(peer.pc, &peer);
	rtcSetLocalDescriptionCallback(peer.pc, descriptionCallback);
	rtcSetLocalCandidateCallback(peer.pc, candidateCallback);
	rtcSetStateChangeCallback(peer.pc, stateChangeCallback);
	rtcSetGatheringStateChangeCallback(peer.pc, gatheringStateCallback);
	rtcSetClosedCallback(peer.pc, pcClosedCallback);

	rtcTrackInit trackInit = {
		RTC_DIRECTION_SENDONLY,
		RTC_CODEC_H264,
		96,
		ssrc,
		"video",
		"video-send",
		NULL,
		NULL,
	};
	peer.tr = rtcAddTrackEx(peer.pc, &trackInit);
	rtcSetUserPointer(peer.tr, &peer);
	rtcSetClosedCallback(peer.tr, trClosedCallback);

	rtcSetLocalDescription(peer.pc, "offer");

	peer.ws = ws;
	rtcSetUserPointer(peer.ws, &peer);
	rtcSetOpenCallback(ws, wbOpenCallback);
	rtcSetClosedCallback(ws, wbClosedCallback);
	rtcSetErrorCallback(ws, wbErrorCallback);
	rtcSetMessageCallback(ws, wbMessageCallback);

	printf("WebSocketServer: Received client connection from %s\n", address);
	
}
static void RTC_API trClosedCallback(int id, void *ptr)
{
	printf("Track %d closed\n", id);
}

static void RTC_API pcClosedCallback(int id, void *ptr)
{
	printf("Peer %d closed\n", id);
}