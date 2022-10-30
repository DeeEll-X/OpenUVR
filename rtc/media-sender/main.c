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
} Peer;

static Peer PEERS[8];
static pthread_mutex_t lock;

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


int main(int argc, char **argv) {
	memset(PEERS, 0, sizeof(PEERS));
	if (pthread_mutex_init(&lock, NULL) != 0)
    {
        printf("\n mutex init failed\n");
        return 1;
    }

	rtcInitLogger(RTC_LOG_DEBUG, NULL);

	rtcWsServerConfiguration wsConfig = {
		30888,
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

	SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(6000);

	if (bind(sock, &addr, sizeof(addr)) < 0)
	{
		fprintf(stderr, "Failed to bind UDP socket on 127.0.0.1:6000");
		return -1;
	}

	int rcvBufSize = 212992;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, sizeof(rcvBufSize));
	const int BUFFER_SIZE = 2048;

	while(true){
		// Receive from UDP
		char buffer[BUFFER_SIZE];
		int len;
		while ((len = recv(sock, buffer, BUFFER_SIZE, 0)) >= 0) {
			if (len < 12)
				continue;

			((RtpHeader*)buffer)->_ssrc = htonl(ssrc);

			pthread_mutex_lock(&lock);
			for(int i=0; i < (sizeof(PEERS) / sizeof(Peer)); i++)
			{
				Peer* peer = &PEERS[i];
				if (peer->pc != 0 && peer->state == RTC_CONNECTED)
				{
					rtcSendMessage(peer->tr, buffer, len);
				}
			}
			pthread_mutex_unlock(&lock);
		}
	}
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

	pthread_mutex_lock(&lock);
	Peer *peer = NULL;
	for(int i=0; i < (sizeof(PEERS) / sizeof(Peer)); i++)
	{
		if (PEERS[i].pc == 0)
		{
			peer = &PEERS[i];
			memset(peer, 0, sizeof(Peer));
			peer->pc = rtcCreatePeerConnection(&config);
			fprintf(stderr, "Peer (%d-%d) created\n", i, peer->pc);
			break;
		}
	}
	pthread_mutex_unlock(&lock);

	if (!peer) {
		fprintf(stderr, "Error allocating memory for peer\n");
		rtcClose(ws);
		return;
	}

	rtcSetUserPointer(peer->pc, peer);
	rtcSetLocalDescriptionCallback(peer->pc, descriptionCallback);
	rtcSetLocalCandidateCallback(peer->pc, candidateCallback);
	rtcSetStateChangeCallback(peer->pc, stateChangeCallback);
	rtcSetGatheringStateChangeCallback(peer->pc, gatheringStateCallback);
	rtcSetClosedCallback(peer->pc, pcClosedCallback);

	rtcTrackInit trackInit = {
		RTC_DIRECTION_SENDONLY,
		RTC_CODEC_H264,
		96,
		ssrc,
		"video",
		NULL,
		NULL,
		NULL,
	};
	peer->tr = rtcAddTrackEx(peer->pc, &trackInit);
	rtcSetUserPointer(peer->tr, peer);
	rtcSetClosedCallback(peer->tr, trClosedCallback);



	rtcSetLocalDescription(peer->pc, "offer");

	peer->ws = ws;
	rtcSetUserPointer(peer->ws, peer);
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