#include <rtc/rtc.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int SOCKET;

#include <unistd.h>


static void RTC_API wbOpenCallback(int id, void *ptr);
static void RTC_API wbClosedCallback(int id, void *ptr);
static void RTC_API wbErrorCallback(int id, const char *error, void *ptr);
static void RTC_API wbMessageCallback(int id, const char *message, int size, void *ptr);
static void RTC_API wbServerClientCallbackFunc(int wsserver, int ws, void *ptr);

static void RTC_API trClosedCallback(int id, void *ptr);
static void RTC_API trMessageCallback(int id, const char *message, int size, void *ptr);

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

typedef struct {
	uint8_t _first;
	uint8_t _payloadType;
	uint16_t _seqNumber;
	uint32_t _timestamp;
	SSRC _ssrc;
} RtpHeader;

static SSRC ssrc = 42;

typedef struct {
	rtcState state;
	rtcGatheringState gatheringState;
	int pc;
	int tr;
	int ws;

	SOCKET sock;
	struct sockaddr_in addr;
} Peer;

static Peer PEER;

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


int main(int argc, char **argv) {
	rtcInitLogger(RTC_LOG_INFO, NULL);

	// Create peer
	rtcConfiguration config;
	memset(&config, 0, sizeof(config));

	memset(&PEER, 0, sizeof(Peer));
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

	char* url = "ws://localhost:30888/3";
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

	PEER.sock = socket(AF_INET, SOCK_DGRAM, 0);
	PEER.addr.sin_family = AF_INET;
	PEER.addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	PEER.addr.sin_port = htons(5000);

	sendto(PEER.sock, "", 0, 0, &(PEER.addr), sizeof(PEER.addr));
	while(true){}
}


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


static void RTC_API trClosedCallback(int id, void *ptr)
{
	printf("Track %d closed\n", id);
}

static void RTC_API trMessageCallback(int id, const char *message, int size, void *ptr)
{
	Peer* peer = (Peer *)ptr;
	int sendsz = sendto(peer->sock, message, size, 0, &(peer->addr), sizeof(peer->addr));

	// printf("tr receive message size = %d sendto = %d\n", size, sendsz);
}
