// hydra-native-patch-bay: the webRTC patch bay of hydra-native (pb, s0.initStream) on libdatachannel. It acts as
// the patch bay of the hydra web editor (src/lib/patch-bay/rtc-patch-bay.js and pb-live.js of
// https://github.com/hydra-synth/hydra, with the peers of simple-peer 9): socket.io 2 signalling over a WebSocket,
// one peer connection per peer, VP8 video in both directions. As in rtc-patch-bay.js, the peer that asks for a
// stream (initSource) is the initiator: it offers to receive video and opens the data channel, and the other peer
// answers with its output. A peer that asks for the stream of a peer that it only sends to starts the connection
// again as the initiator, and then sends its output too.
// ffmpeg does the video codec: one child encodes the output to VP8 for all peers, and one child per received stream
// decodes it. The helper puts VP8 into RTP packets and takes it out again (RFC 7741).
//
// usage: hydra-native-patch-bay SERVER ROOM SOCKET
// SOCKET is the path of a local socket for the frames. Each connection starts with one line that names its channel:
//   output WIDTHxHEIGHT FPS                        hydra-native sends RGBA frames of its output (rows from the bottom)
//   stream-N-WIDTHxHEIGHT                          the helper sends the RGBA frames of a received stream
// stdin, one JSON object per line, from src/patch_bay.c:
//   { type: 'call', method, args }                 a method of pb: initSource, setName, broadcast
// stdout, one JSON object per line:
//   { type: 'state', id, streamId, nick, nicks, peers }  after each change of the peers or names; streamId is the
//                                                    socket id, which the other peers list in peers
//   { type: 'publish', on }                         whether a peer connection wants the output frames
//   { type: 'video', nick, channel, width, height, resized } a received stream, whose frames come on `channel`;
//                                                    `resized` for a new frame size of a stream sent before
//   { type: 'video closed', nick }                   the peer of a received stream left
//   { type: 'broadcast', data }, { type: 'message', data }, { type: 'log', text, error }
//
// All work runs on the main thread in a poll loop. The callbacks of libdatachannel come from its threads: they only
// put events into a queue for the main thread.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cJSON.h>
#include <rtc/rtc.h>

#include "patch_bay_websocket.h"
#include "process.h"

#define MIN_PING_MS 1000 // engine.io gives 25000
#define MAX_PING_MS 3600000
#define CLOSE_WAIT_MS 1000 // for the close handshake of the signalling WebSocket
#define POLL_MS 100 // the resolution of the timers
#define ID_LENGTH 8 // pb.id, the start of a random UUID as in rtc-patch-bay.js
#define MAX_FRAME_SIDE 16384
#define MAX_PEERS 256 // the newest peers of the room in the tables: the signalling server tells no peer that leaves
#define MAX_UNSENT 256 // packets while the signalling server is not connected; more are dropped
#define MAX_CONNECTIONS 16 // peer connections at the same time
#define MAX_LINE (1 << 20)
#define MAX_CHANNEL_LINE 64
#define MAX_WAITING 8 // local connections that did not send their channel line yet
#define MAX_WATCHES (8 + MAX_WAITING + 3 * MAX_CONNECTIONS)
#define MAX_QUEUED_PACKETS 4096 // RTP packets for the main thread; more are dropped
#define MAX_MID 63
#define RGBA_BYTES 4
#define DEFAULT_PAYLOAD_TYPE 96
#define MAX_PAYLOAD_TYPE 127 // RTP has 7 bits for it
#define STUN_SERVER "stun:stun.l.google.com:19302" // the first STUN server of the defaults of simple-peer
#define DATA_CHANNEL_LABEL "hydra-native"
#define TRACK_NAME "hydra-native" // the CNAME and the stream id (msid) of the output
#define TRACK_ID "video"
// The certificate files of the systems, for the check of a wss:// signalling server: Debian and Ubuntu, Fedora,
// macOS and others.
static const char *const CERTIFICATE_FILES[] = { "/etc/ssl/certs/ca-certificates.crt",
                                                 "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem" };

// The video: ffmpeg encodes the output with a keyframe each second, so that a new receiver and a receiver after a
// lost packet start within a second; the PLI of a receiver does not restart the encoder.
#define MAX_OUTPUT_FPS 240
#define VIDEO_BITRATE "2M"
#define MAX_ENCODED_FRAME (1 << 22)
#define MAX_DECODER_BACKLOG (1 << 22) // bytes of VP8 that wait for a decoder; more are dropped
#define IVF_FILE_HEADER 32
#define IVF_FRAME_HEADER 12
#define IVF_TIMEBASE 1000
#define RTP_HEADER 12
#define RTP_VERSION 2
#define RTP_CLOCK_KHZ 90 // VP8 uses a clock of 90 kHz
#define RTP_MARKER 0x80
#define MAX_RTP_PAYLOAD 1200 // with the headers below the MTU of most paths
#define NACK_PACKETS 512 // sent packets that a NACK can ask for again
#define KEYFRAME_REQUEST_MS 1000
#define VP8_START 0x10 // the S bit of the payload descriptor, partition 0
#define VP8_KEYFRAME_HEADER 10 // frame tag, start code, width and height
// Engine.IO 3 packets, and the socket.io 2 packets inside its message packets.
#define SIGNALLING_PATH "/socket.io/?EIO=3&transport=websocket"
#define ENGINE_OPEN '0'
#define ENGINE_PING "2"
#define ENGINE_MESSAGE '4'
#define SOCKET_CONNECT '0'
#define SOCKET_EVENT '2'
#define SOCKET_EVENT_PREFIX "42"

enum event_type {
    EVENT_SIGNALLING, // value: enum websocket_event
    EVENT_DESCRIPTION, // id: the peer connection; text: the type, second: the SDP
    EVENT_CANDIDATE, // text: the candidate, second: the mid
    EVENT_STATE, // value: rtcState
    EVENT_SIGNALING_STATE, // value: rtcSignalingState
    EVENT_DATA_CHANNEL, // value: the channel of the other peer
    EVENT_CHANNEL_OPEN, // id: the data channel
    EVENT_CHANNEL_CLOSED,
    EVENT_PACKET, // id: the track; text: the RTP packet
};

struct event {
    struct event *next;
    enum event_type type;
    int id, value;
    size_t size;
    const char *second;
    char text[];
};

struct buffer {
    unsigned char *data;
    size_t start, size, capacity;
};

// A received stream: RTP packets with VP8 go to an ffmpeg decoder, whose RGBA frames go to hydra-native.
struct receiver {
    int number, width, height; // a new frame size gives a new channel name
    char channel_name[MAX_CHANNEL_LINE];
    char *nick; // of the 'video' message, NULL before the first frame
    int connection; // of hydra-native, which reads the frames, or -1
    struct buffer to_hydra;
    struct process *decoder;
    int decoder_input, decoder_output;
    struct buffer to_decoder;
    uint32_t decoded_count;
    unsigned char *frame; // RGBA from the decoder
    size_t frame_used;
    unsigned char *encoded; // the VP8 frame of the RTP packets so far
    size_t encoded_size;
    bool assembling, sequenced, waiting_keyframe;
    uint32_t timestamp;
    uint16_t next_sequence;
    int64_t keyframe_requested;
};

struct peer {
    char *id; // the socket id of the other peer
    bool initiator, closed;
    int connection, channel, track; // of libdatachannel, 0 when none
    char mid[MAX_MID + 1];
    int payload_type;
    bool sending, sent_keyframe;
    uint32_t ssrc;
    uint16_t sequence;
    bool negotiating, negotiation_queued;
    struct receiver receiver;
};

struct video_media {
    bool found;
    int payload_type; // of VP8, or -1
    char mid[MAX_MID + 1];
    rtcDirection direction;
};

static const char *server, *room, *socket_path;
static bool stopping, quitting;
static int64_t stop_deadline;
static volatile sig_atomic_t quit_signal;
static char own_id[ID_LENGTH + 1];
static char *nick, *stream_id;
// The tables of pb-live.js and rtc-patch-bay.js: JSON objects keep the order of their members, as JavaScript does.
static cJSON *nick_from_id, *id_from_nick, *peer_list;
static struct peer *peers[MAX_CONNECTIONS]; // the peer connections (rtcPeers)
static int peer_count;
static bool publishing;
static int received_streams;

static struct {
    pthread_mutex_t lock;
    struct event *first, *last;
    int packets;
    int wake[2];
} events = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = { -1, -1 } };

static bool connected; // to the socket.io namespace
static bool signalling_open;
static int64_t ping_interval, next_ping;
static char *unsent[MAX_UNSENT]; // event packets before the connection, which socket.io-client also buffers
static int unsent_count;

static int listener = -1;
static struct {
    int stream;
    size_t used;
    char line[MAX_CHANNEL_LINE];
} waiting[MAX_WAITING];

static struct {
    int stream; // of hydra-native, or -1
    unsigned char *frame;
    size_t frame_size, used;
    struct process *encoder;
    int encoder_input, encoder_output;
    struct buffer to_encoder;
    unsigned char *encoded; // the IVF stream of the encoder
    size_t encoded_used;
    bool header_read;
} output = { .stream = -1, .encoder_input = -1, .encoder_output = -1 };

static struct {
    struct pollfd fds[MAX_WATCHES];
    void (*actions[MAX_WATCHES])(void *context);
    void *contexts[MAX_WATCHES];
    int count;
} watches;

static char input_line[MAX_LINE];
static size_t input_used;

static void peer_close(struct peer *peer);
static void send_state(void);

// ---------------------------------------------------------------- small helpers

static int64_t now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static uint16_t read16(const unsigned char *bytes) { return (uint16_t)(bytes[0] << 8 | bytes[1]); }
static uint32_t read32(const unsigned char *bytes) { return (uint32_t)read16(bytes) << 16 | read16(bytes + 2); }
static uint16_t read16_little(const unsigned char *bytes) { return (uint16_t)(bytes[0] | bytes[1] << 8); }

static void write16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value >> 8);
    bytes[1] = (unsigned char)value;
}

static void write32(unsigned char *bytes, uint32_t value) {
    write16(bytes, (uint16_t)(value >> 16));
    write16(bytes + 2, (uint16_t)value);
}

static void write_little(unsigned char *bytes, uint64_t value, int count) {
    for (int index = 0; index < count; index++) bytes[index] = (unsigned char)(value >> (8 * index));
}

static void set_nonblocking(int stream) { fcntl(stream, F_SETFL, fcntl(stream, F_GETFL) | O_NONBLOCK); }

static void close_stream(int *stream) {
    if (*stream >= 0) close(*stream);
    *stream = -1;
}

static void buffer_free(struct buffer *buffer) {
    free(buffer->data);
    *buffer = (struct buffer){ 0 };
}

static bool buffer_empty(const struct buffer *buffer) { return buffer->start == buffer->size; }

// Appends `size` bytes, unless the buffer would hold more than `limit` bytes.
static bool buffer_append(struct buffer *buffer, const void *data, size_t size, size_t limit) {
    if (buffer->start) {
        memmove(buffer->data, buffer->data + buffer->start, buffer->size - buffer->start);
        buffer->size -= buffer->start;
        buffer->start = 0;
    }
    if (buffer->size + size > limit) return false;
    if (buffer->size + size > buffer->capacity) {
        unsigned char *grown = realloc(buffer->data, buffer->size + size);
        if (!grown) return false;
        buffer->data = grown;
        buffer->capacity = buffer->size + size;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return true;
}

// Writes as much as the non-blocking stream takes; false when the reader is gone.
static bool buffer_flush(struct buffer *buffer, int stream) {
    while (!buffer_empty(buffer)) {
        ssize_t count = write(stream, buffer->data + buffer->start, buffer->size - buffer->start);
        if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        buffer->start += (size_t)count;
    }
    buffer->start = buffer->size = 0;
    return true;
}

static void stop_child(struct process **process, int *input, int *output_stream) {
    close_stream(input);
    close_stream(output_stream);
    process_free(*process);
    *process = NULL;
}

// ---------------------------------------------------------------- JSON lines

static const char *member_string(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static cJSON *member_object(const cJSON *object, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsObject(item) ? item : NULL;
}

static bool has_member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name) != NULL;
}

// Sets a member and keeps its place when it exists, as JavaScript does.
static void set_member(cJSON *object, const char *name, cJSON *value) {
    if (has_member(object, name)) cJSON_ReplaceItemInObjectCaseSensitive(object, name, value);
    else cJSON_AddItemToObject(object, name, value);
}

// Writes one line to stdout and frees the object.
static void print_object(cJSON *object) {
    char *text = cJSON_PrintUnformatted(object);
    if (text) printf("%s\n", text);
    fflush(stdout);
    free(text);
    cJSON_Delete(object);
}

static cJSON *message_new(const char *type) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", type);
    return message;
}

__attribute__((format(printf, 2, 3))) static void log_text(bool error, const char *format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    cJSON *message = message_new("log");
    cJSON_AddStringToObject(message, "text", text);
    if (error) cJSON_AddTrueToObject(message, "error");
    print_object(message);
}

static cJSON *member_names(const cJSON *object) {
    cJSON *names = cJSON_CreateArray();
    for (const cJSON *item = object->child; item; item = item->next)
        cJSON_AddItemToArray(names, cJSON_CreateString(item->string));
    return names;
}

static void send_state(void) {
    cJSON *state = message_new("state");
    cJSON_AddStringToObject(state, "id", own_id);
    if (stream_id) cJSON_AddStringToObject(state, "streamId", stream_id);
    if (nick) cJSON_AddStringToObject(state, "nick", nick);
    cJSON_AddItemToObject(state, "nicks", member_names(id_from_nick));
    cJSON_AddItemToObject(state, "peers", member_names(peer_list));
    print_object(state);
}

// ---------------------------------------------------------------- the event queue

// Called from the threads of libdatachannel: `text` (with `size` bytes) and `second` are copied.
static void push_event(enum event_type type, int id, int value, const char *text, size_t size, const char *second) {
    size_t second_size = second ? strlen(second) + 1 : 0;
    struct event *event = malloc(sizeof *event + size + 1 + second_size);
    if (!event) return;
    *event = (struct event){ .type = type, .id = id, .value = value, .size = size };
    if (size) memcpy(event->text, text, size);
    event->text[size] = '\0';
    if (second) event->second = memcpy(event->text + size + 1, second, second_size);
    pthread_mutex_lock(&events.lock);
    bool dropped = type == EVENT_PACKET && events.packets >= MAX_QUEUED_PACKETS;
    if (!dropped) {
        if (type == EVENT_PACKET) events.packets++;
        if (events.last) events.last->next = event;
        else events.first = event;
        events.last = event;
    }
    pthread_mutex_unlock(&events.lock);
    if (dropped) return free(event);
    ssize_t written = write(events.wake[1], "", 1); // a full pipe already wakes the main thread
    (void)written;
}

static struct event *pop_event(void) {
    pthread_mutex_lock(&events.lock);
    struct event *event = events.first;
    if (event) {
        events.first = event->next;
        if (!events.first) events.last = NULL;
        if (event->type == EVENT_PACKET) events.packets--;
    }
    pthread_mutex_unlock(&events.lock);
    return event;
}

static void RTC_API on_description(int pc, const char *sdp, const char *type, void *pointer) {
    push_event(EVENT_DESCRIPTION, pc, 0, type, strlen(type), sdp);
}

static void RTC_API on_candidate(int pc, const char *candidate, const char *mid, void *pointer) {
    push_event(EVENT_CANDIDATE, pc, 0, candidate, strlen(candidate), mid);
}

static void RTC_API on_state(int pc, rtcState state, void *pointer) {
    push_event(EVENT_STATE, pc, state, NULL, 0, NULL);
}

static void RTC_API on_signaling_state(int pc, rtcSignalingState state, void *pointer) {
    push_event(EVENT_SIGNALING_STATE, pc, state, NULL, 0, NULL);
}

static void RTC_API on_channel_open(int channel, void *pointer) {
    push_event(EVENT_CHANNEL_OPEN, channel, 0, NULL, 0, NULL);
}

static void RTC_API on_channel_closed(int channel, void *pointer) {
    push_event(EVENT_CHANNEL_CLOSED, channel, 0, NULL, 0, NULL);
}

// The channel of the initiator arrives open, so its close callback is set here, before the main thread sees it.
static void RTC_API on_data_channel(int pc, int channel, void *pointer) {
    rtcSetClosedCallback(channel, on_channel_closed);
    push_event(EVENT_DATA_CHANNEL, pc, channel, NULL, 0, NULL);
}

static void RTC_API on_track_message(int track, const char *message, int size, void *pointer) {
    if (size > 0) push_event(EVENT_PACKET, track, 0, message, (size_t)size, NULL); // size < 0: a text message
}

static void on_websocket(enum websocket_event event, const char *text, size_t size) {
    push_event(EVENT_SIGNALLING, 0, event, text, size, NULL);
}

// ---------------------------------------------------------------- signalling (socket.io 2)

static void send_packet(char *text) {
    if (!text) return;
    if (connected && websocket_send(text)) free(text);
    else if (!connected && unsent_count < MAX_UNSENT) unsent[unsent_count++] = text;
    else free(text);
}

// Emits a socket.io event with one or two arguments, which it takes.
static void emit_event(const char *name, cJSON *first, cJSON *second) {
    cJSON *packet = cJSON_CreateArray();
    cJSON_AddItemToArray(packet, cJSON_CreateString(name));
    cJSON_AddItemToArray(packet, first);
    if (second) cJSON_AddItemToArray(packet, second);
    char *json = cJSON_PrintUnformatted(packet);
    cJSON_Delete(packet);
    if (!json) return;
    char *text = malloc(strlen(SOCKET_EVENT_PREFIX) + strlen(json) + 1);
    if (text) sprintf(text, "%s%s", SOCKET_EVENT_PREFIX, json);
    free(json);
    send_packet(text);
}

static void broadcast_nick(const char *previous) {
    cJSON *update = message_new("update-nick");
    cJSON_AddStringToObject(update, "id", own_id);
    cJSON_AddStringToObject(update, "nick", nick);
    if (previous) cJSON_AddStringToObject(update, "previous", previous);
    emit_event("broadcast", update, NULL);
}

static void set_name(const char *name) {
    char *previous = nick;
    nick = strdup(name);
    if (nick) broadcast_nick(previous);
    free(previous);
}

static void forget_peer(const char *id) {
    const char *name = member_string(nick_from_id, id);
    const char *named = name ? member_string(id_from_nick, name) : NULL;
    if (named && !strcmp(named, id)) cJSON_DeleteItemFromObjectCaseSensitive(id_from_nick, name);
    cJSON_DeleteItemFromObjectCaseSensitive(nick_from_id, id);
    cJSON_DeleteItemFromObjectCaseSensitive(peer_list, id);
}

static void new_peer(const char *id) {
    if (!has_member(peer_list, id) && cJSON_GetArraySize(peer_list) >= MAX_PEERS) {
        char *oldest = strdup(peer_list->child->string);
        if (oldest) forget_peer(oldest);
        free(oldest);
    }
    set_member(peer_list, id, cJSON_CreateNull());
    set_member(nick_from_id, id, cJSON_CreateString(id));
    set_member(id_from_nick, id, cJSON_CreateString(id));
    if (nick) broadcast_nick(NULL);
}

static void receive_broadcast(const cJSON *data) {
    cJSON *message = message_new("broadcast");
    cJSON_AddItemToObject(message, "data", cJSON_Duplicate(data, true));
    print_object(message);
    const char *type = member_string(data, "type"), *id = member_string(data, "id");
    const char *name = member_string(data, "nick"), *previous = member_string(data, "previous");
    if (!type || strcmp(type, "update-nick") || !id || !name || !has_member(peer_list, id) ||
        (previous && !strcmp(previous, name)))
        return;
    const char *old = member_string(nick_from_id, id);
    if (old) cJSON_DeleteItemFromObjectCaseSensitive(id_from_nick, old);
    set_member(nick_from_id, id, cJSON_CreateString(name));
    set_member(id_from_nick, name, cJSON_CreateString(id));
}

// ---------------------------------------------------------------- peers on libdatachannel

static void send_signal(struct peer *peer, cJSON *signal) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "id", peer->id);
    cJSON_AddItemToObject(message, "message", signal);
    cJSON_AddStringToObject(message, "type", "signal");
    emit_event("message", message, NULL);
}

static struct peer *peer_with_id(const char *id) {
    for (int index = 0; index < peer_count; index++)
        if (!strcmp(peers[index]->id, id)) return peers[index];
    return NULL;
}

static struct peer *peer_with_connection(int connection) {
    for (int index = 0; index < peer_count; index++)
        if (peers[index]->connection == connection) return peers[index];
    return NULL;
}

static struct peer *peer_with_channel(int channel) {
    for (int index = 0; index < peer_count; index++)
        if (peers[index]->channel == channel) return peers[index];
    return NULL;
}

static struct peer *peer_with_track(int track) {
    for (int index = 0; index < peer_count; index++)
        if (peers[index]->track == track) return peers[index];
    return NULL;
}

static struct peer *peer_with_channel_name(const char *name) {
    for (int index = 0; index < peer_count; index++)
        if (!strcmp(peers[index]->receiver.channel_name, name)) return peers[index];
    return NULL;
}

// simple-peer: a peer that answers an offer without video asks the initiator for a video transceiver. The web
// editor offers only the data channel, because simple-peer 9 ignores its offerConstraints.
static void request_video(struct peer *peer) {
    if (peer->sending) return;
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "kind", "video");
    cJSON *signal = message_new("transceiverRequest");
    cJSON_AddItemToObject(signal, "transceiverRequest", request);
    send_signal(peer, signal);
}

// simple-peer: an initiator queues a negotiation that comes during another one.
static void negotiate(struct peer *peer) {
    if (peer->negotiating) {
        peer->negotiation_queued = true;
        return;
    }
    peer->negotiating = true;
    if (rtcSetLocalDescription(peer->connection, "offer") < 0) {
        peer->negotiating = false;
        log_text(true, "cannot make an offer for %s", peer->id);
    }
}

// The video track of the connection, with the payload type of VP8 that the offer gives; `mid` NULL for a new one.
static bool add_track(struct peer *peer, const char *mid, rtcDirection direction, int payload_type) {
    peer->ssrc = arc4random();
    rtcTrackInit init = { .direction = direction, .codec = RTC_CODEC_VP8, .payloadType = payload_type,
                          .ssrc = peer->ssrc, .mid = mid, .name = TRACK_NAME, .msid = TRACK_NAME,
                          .trackId = TRACK_ID };
    int track = rtcAddTrackEx(peer->connection, &init);
    if (track < 0) {
        log_text(true, "cannot add the video of %s", peer->id);
        return false;
    }
    peer->track = track;
    peer->payload_type = payload_type;
    peer->sending = direction == RTC_DIRECTION_SENDONLY || direction == RTC_DIRECTION_SENDRECV;
    peer->receiver.waiting_keyframe = true;
    rtcGetTrackMid(track, peer->mid, sizeof peer->mid);
    rtcChainRtcpReceivingSession(track); // receiver reports, and the PLI of rtcRequestKeyframe
    if (peer->sending) rtcChainRtcpNackResponder(track, NACK_PACKETS);
    rtcSetMessageCallback(track, on_track_message);
    return true;
}

// The first video section of a description: its mid, direction and the payload type of VP8.
static struct video_media find_video(const char *sdp) {
    struct video_media video = { .payload_type = -1, .direction = RTC_DIRECTION_SENDRECV };
    bool inside = false;
    for (const char *line = sdp; *line;) {
        size_t length = strcspn(line, "\r\n");
        if (!strncmp(line, "m=", 2)) {
            if (video.found) break;
            inside = !strncmp(line, "m=video ", 8);
            video.found = inside;
        } else if (inside) {
            int payload_type, used = 0;
            char encoding[16];
            if (!strncmp(line, "a=mid:", 6) && length - 6 <= MAX_MID) {
                memcpy(video.mid, line + 6, length - 6);
                video.mid[length - 6] = '\0';
            } else if (video.payload_type < 0 && sscanf(line, "a=rtpmap:%d %15[^/\r\n]%n", &payload_type, encoding,
                                                        &used) == 2 && (size_t)used <= length && payload_type >= 0 &&
                       payload_type <= MAX_PAYLOAD_TYPE && !strcasecmp(encoding, "VP8")) {
                video.payload_type = payload_type;
            } else if (!strncmp(line, "a=sendonly", length) && length == 10) {
                video.direction = RTC_DIRECTION_RECVONLY; // the direction of the answer
            } else if (!strncmp(line, "a=recvonly", length) && length == 10) {
                video.direction = RTC_DIRECTION_SENDONLY;
            } else if (!strncmp(line, "a=inactive", length) && length == 10) {
                video.direction = RTC_DIRECTION_INACTIVE;
            }
        }
        line += length;
        while (*line == '\r' || *line == '\n') line++;
    }
    return video;
}

// An offer of the initiator: a non-initiator answers it with its output where the offer has video.
static void receive_offer(struct peer *peer, const char *sdp) {
    struct video_media video = find_video(sdp);
    if (rtcSetRemoteDescription(peer->connection, sdp, "offer") < 0) {
        log_text(true, "the offer of %s is invalid", peer->id);
        return;
    }
    if (video.found && video.payload_type < 0) log_text(true, "the video of %s is not VP8", peer->id);
    else if (video.found && !video.mid[0]) log_text(true, "the video of %s has no mid", peer->id);
    else if (video.found && (!peer->track || strcmp(peer->mid, video.mid)))
        add_track(peer, video.mid, video.direction, video.payload_type);
    if (rtcSetLocalDescription(peer->connection, "answer") < 0) log_text(true, "cannot answer %s", peer->id);
}

// A signal of simple-peer: a description, a candidate, or a request of the non-initiator to negotiate again.
static void peer_signal(struct peer *peer, const cJSON *signal) {
    if (peer->initiator && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(signal, "renegotiate"))) negotiate(peer);
    const cJSON *request = member_object(signal, "transceiverRequest");
    // An initiator has its video track from the start: a request adds one only if that failed.
    if (peer->initiator && request) {
        const char *kind = member_string(request, "kind");
        if (!kind || strcmp(kind, "video")) log_text(false, "the patch bay receives only video");
        else if (!peer->track && add_track(peer, NULL, RTC_DIRECTION_RECVONLY, DEFAULT_PAYLOAD_TYPE)) negotiate(peer);
    }
    const cJSON *candidate = member_object(signal, "candidate");
    const char *text = member_string(candidate, "candidate");
    if (text && text[0] && rtcAddRemoteCandidate(peer->connection, text, member_string(candidate, "sdpMid")) < 0)
        log_text(true, "an invalid candidate of %s", peer->id);
    const char *sdp = member_string(signal, "sdp"), *type = member_string(signal, "type");
    if (!sdp) return;
    if (type && !strcmp(type, "offer")) receive_offer(peer, sdp);
    else if (type && !strcmp(type, "answer") && rtcSetRemoteDescription(peer->connection, sdp, "answer") >= 0) return;
    else log_text(true, "an invalid description of %s", peer->id);
}

static struct peer *peer_create(const char *id, bool initiator, bool send) {
    if (peer_count >= MAX_CONNECTIONS) {
        log_text(true, "no connection to %s: the patch bay has %d connections", id, MAX_CONNECTIONS);
        return NULL;
    }
    const char *ice_servers[] = { STUN_SERVER };
    rtcConfiguration configuration = { .iceServers = ice_servers, .iceServersCount = 1,
                                       .disableAutoNegotiation = true,
                                       // simple-peer adds the video in a later negotiation than the data channel
                                       .forceMediaTransport = true };
    int connection = rtcCreatePeerConnection(&configuration);
    struct peer *peer = connection < 0 ? NULL : calloc(1, sizeof *peer);
    char *copy = peer ? strdup(id) : NULL;
    if (!copy) {
        if (connection >= 0) rtcDeletePeerConnection(connection);
        free(peer);
        log_text(true, "cannot connect to %s", id);
        return NULL;
    }
    peer->id = copy;
    peer->initiator = initiator;
    peer->connection = connection;
    peer->receiver.connection = peer->receiver.decoder_input = peer->receiver.decoder_output = -1;
    peers[peer_count++] = peer;
    rtcSetLocalDescriptionCallback(connection, on_description);
    rtcSetLocalCandidateCallback(connection, on_candidate);
    rtcSetStateChangeCallback(connection, on_state);
    rtcSetSignalingStateChangeCallback(connection, on_signaling_state);
    rtcSetDataChannelCallback(connection, on_data_channel);
    // A non-initiator adds its output when an offer with video comes, with the payload type of the offer.
    if (initiator) {
        add_track(peer, NULL, send ? RTC_DIRECTION_SENDRECV : RTC_DIRECTION_RECVONLY, DEFAULT_PAYLOAD_TYPE);
        int channel = rtcCreateDataChannel(connection, DATA_CHANNEL_LABEL);
        if (channel >= 0) {
            peer->channel = channel;
            rtcSetOpenCallback(channel, on_channel_open);
            rtcSetClosedCallback(channel, on_channel_closed);
        }
        negotiate(peer);
    }
    send_state();
    return peer;
}

static void stop_decoder(struct receiver *receiver) {
    stop_child(&receiver->decoder, &receiver->decoder_input, &receiver->decoder_output);
    buffer_free(&receiver->to_decoder);
    free(receiver->frame);
    receiver->frame = NULL;
    receiver->frame_used = 0;
}

static void peer_close(struct peer *peer) {
    if (peer->closed) return;
    peer->closed = true;
    struct receiver *receiver = &peer->receiver;
    stop_decoder(receiver);
    close_stream(&receiver->connection);
    buffer_free(&receiver->to_hydra);
    if (receiver->nick) {
        cJSON *message = message_new("video closed");
        cJSON_AddStringToObject(message, "nick", receiver->nick);
        print_object(message);
    }
    if (peer->channel) rtcDeleteDataChannel(peer->channel);
    if (peer->track) rtcDeleteTrack(peer->track);
    rtcDeletePeerConnection(peer->connection); // waits for its callbacks; their events find no peer later
    for (int index = 0; index < peer_count; index++) {
        if (peers[index] != peer) continue;
        peers[index] = peers[--peer_count];
        break;
    }
    free(receiver->nick);
    free(receiver->encoded);
    free(peer->id);
    free(peer);
    send_state();
}

static void init_source(const char *name) {
    const char *id = member_string(id_from_nick, name);
    if (!id) return;
    struct peer *peer = peer_with_id(id);
    if (!peer) {
        peer_create(id, true, false);
    } else if (!peer->initiator) {
        // rtc-patch-bay.js: a connection that only sends starts again in both directions.
        char *copy = strdup(id);
        peer_close(peer);
        if (copy) peer_create(copy, true, true);
        free(copy);
    }
}

// ---------------------------------------------------------------- sending the output

// Sends one VP8 frame in RTP packets; a receiver starts with a keyframe.
static void send_frame(struct peer *peer, const unsigned char *frame, size_t size, uint32_t timestamp) {
    bool keyframe = !(frame[0] & 1);
    if (!peer->sending || !rtcIsOpen(peer->track) || (!peer->sent_keyframe && !keyframe)) return;
    peer->sent_keyframe = true;
    unsigned char packet[RTP_HEADER + 1 + MAX_RTP_PAYLOAD];
    for (size_t offset = 0; offset < size;) {
        size_t chunk = size - offset < MAX_RTP_PAYLOAD ? size - offset : MAX_RTP_PAYLOAD;
        packet[0] = RTP_VERSION << 6;
        packet[1] = (unsigned char)((offset + chunk == size ? RTP_MARKER : 0) | peer->payload_type);
        write16(packet + 2, peer->sequence++);
        write32(packet + 4, timestamp);
        write32(packet + 8, peer->ssrc);
        packet[RTP_HEADER] = offset ? 0 : VP8_START;
        memcpy(packet + RTP_HEADER + 1, frame + offset, chunk);
        rtcSendMessage(peer->track, (const char *)packet, (int)(RTP_HEADER + 1 + chunk));
        offset += chunk;
    }
}

static void stop_output(void) {
    close_stream(&output.stream);
    stop_child(&output.encoder, &output.encoder_input, &output.encoder_output);
    buffer_free(&output.to_encoder);
    free(output.frame);
    free(output.encoded);
    output.frame = output.encoded = NULL;
    output.used = output.encoded_used = 0;
    output.header_read = false;
}

// The IVF stream of the encoder: a file header, then each frame with a header of its own.
static void read_encoder(void *context) {
    if (output.encoder_output < 0) return;
    size_t capacity = IVF_FILE_HEADER + IVF_FRAME_HEADER + MAX_ENCODED_FRAME;
    ssize_t count = read(output.encoder_output, output.encoded + output.encoded_used, capacity - output.encoded_used);
    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) {
        log_text(true, "the video encoder (ffmpeg) ended");
        return stop_output();
    }
    if (count < 0) return;
    output.encoded_used += (size_t)count;
    size_t start = 0;
    if (!output.header_read && output.encoded_used >= IVF_FILE_HEADER) {
        output.header_read = true;
        start = IVF_FILE_HEADER;
    }
    while (output.header_read && output.encoded_used - start >= IVF_FRAME_HEADER) {
        const unsigned char *header = output.encoded + start;
        size_t size = (size_t)header[0] | (size_t)header[1] << 8 | (size_t)header[2] << 16 | (size_t)header[3] << 24;
        if (!size || size > MAX_ENCODED_FRAME) {
            log_text(true, "the video encoder sent an invalid frame");
            return stop_output();
        }
        if (output.encoded_used - start < IVF_FRAME_HEADER + size) break;
        uint32_t timestamp = (uint32_t)(now_ms() * RTP_CLOCK_KHZ);
        for (int index = 0; index < peer_count; index++)
            send_frame(peers[index], header + IVF_FRAME_HEADER, size, timestamp);
        start += IVF_FRAME_HEADER + size;
    }
    memmove(output.encoded, output.encoded + start, output.encoded_used - start);
    output.encoded_used -= start;
}

static void write_encoder(void *context) {
    if (output.encoder_input >= 0 && !buffer_flush(&output.to_encoder, output.encoder_input)) {
        log_text(true, "the video encoder (ffmpeg) ended");
        stop_output();
    }
}

// Frames of the output; a frame that comes while the encoder still reads the one before is dropped.
static void read_output(void *context) {
    if (output.stream < 0) return;
    ssize_t count = read(output.stream, output.frame + output.used, output.frame_size - output.used);
    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) return stop_output();
    if (count < 0) return;
    output.used += (size_t)count;
    if (output.used < output.frame_size) return;
    output.used = 0;
    if (buffer_empty(&output.to_encoder) && output.encoder_input >= 0) {
        buffer_append(&output.to_encoder, output.frame, output.frame_size, output.frame_size);
        write_encoder(NULL);
    }
}

// One connection per publishing period of hydra-native, with the frame size and rate in its first line.
static void start_output(int stream, const char *format) {
    int width = 0, height = 0, rate = 0;
    if (!format || sscanf(format, "%dx%d %d", &width, &height, &rate) != 3 || width <= 0 || height <= 0 ||
        width > MAX_FRAME_SIDE || height > MAX_FRAME_SIDE || rate <= 0 || rate > MAX_OUTPUT_FPS) {
        close(stream);
        return;
    }
    stop_output();
    output.stream = stream;
    output.frame_size = (size_t)width * (size_t)height * RGBA_BYTES;
    output.frame = malloc(output.frame_size);
    output.encoded = malloc(IVF_FILE_HEADER + IVF_FRAME_HEADER + MAX_ENCODED_FRAME);
    char dimensions[32], fps[16];
    snprintf(dimensions, sizeof dimensions, "%dx%d", width, height);
    snprintf(fps, sizeof fps, "%d", rate); // also the keyframe interval: one second
    // With the default threads, libvpx falls behind and ffmpeg queues the frames for more than a second.
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s",
                           dimensions, "-r", fps, "-i", "-", "-vf", "vflip", "-pix_fmt", "yuv420p", "-c:v",
                           "libvpx", "-deadline", "realtime", "-cpu-used", "8", "-lag-in-frames", "0",
                           "-error-resilient", "1", "-g", fps, "-b:v", VIDEO_BITRATE, "-threads", "1", "-f", "ivf",
                           "-flush_packets", "1", "-", NULL };
    if (output.frame && output.encoded)
        output.encoder = process_start(argv, &output.encoder_input, &output.encoder_output);
    if (!output.encoder) {
        log_text(true, "cannot start the video encoder: the patch bay needs ffmpeg");
        return stop_output();
    }
    set_nonblocking(output.stream);
    set_nonblocking(output.encoder_input);
    set_nonblocking(output.encoder_output);
    for (int index = 0; index < peer_count; index++) peers[index]->sent_keyframe = false;
}

// ---------------------------------------------------------------- receiving streams

static void request_keyframe(struct peer *peer) {
    int64_t now = now_ms();
    if (now - peer->receiver.keyframe_requested < KEYFRAME_REQUEST_MS) return;
    peer->receiver.keyframe_requested = now;
    rtcRequestKeyframe(peer->track);
}

// A lost packet: the decoder waits for the next keyframe, as the depacketizer has no jitter buffer.
static void lose_frame(struct peer *peer) {
    peer->receiver.assembling = false;
    peer->receiver.waiting_keyframe = true;
    request_keyframe(peer);
}

// The first frame of a stream announces it as 'got video' of pb-live.js; a new frame size opens a new channel.
static void open_channel(struct peer *peer, int width, int height) {
    struct receiver *receiver = &peer->receiver;
    close_stream(&receiver->connection);
    buffer_free(&receiver->to_hydra);
    if (!receiver->number) receiver->number = ++received_streams;
    receiver->width = width;
    receiver->height = height;
    snprintf(receiver->channel_name, sizeof receiver->channel_name, "stream-%d-%dx%d", receiver->number, width,
             height);
    cJSON *message = message_new("video");
    if (receiver->nick) {
        cJSON_AddTrueToObject(message, "resized");
    } else {
        const char *name = member_string(nick_from_id, peer->id);
        receiver->nick = strdup(name ? name : peer->id);
    }
    cJSON_AddStringToObject(message, "nick", receiver->nick ? receiver->nick : peer->id);
    cJSON_AddStringToObject(message, "channel", receiver->channel_name);
    cJSON_AddNumberToObject(message, "width", width);
    cJSON_AddNumberToObject(message, "height", height);
    print_object(message);
}

static bool start_decoder(struct peer *peer, int width, int height) {
    struct receiver *receiver = &peer->receiver;
    stop_decoder(receiver);
    receiver->frame = malloc((size_t)width * (size_t)height * RGBA_BYTES);
    // One thread for the decoder and for the rawvideo encoder of the output: frame threads hold frames back.
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-probesize", "32", "-analyzeduration", "0",
                           "-flags", "low_delay", "-threads", "1", "-f", "ivf", "-i", "-", "-fps_mode", "passthrough",
                           "-threads", "1", "-f", "rawvideo", "-pix_fmt", "rgba", "-flush_packets", "1", "-", NULL };
    if (receiver->frame) receiver->decoder = process_start(argv, &receiver->decoder_input, &receiver->decoder_output);
    if (!receiver->decoder) {
        log_text(true, "cannot start the video decoder: the patch bay needs ffmpeg");
        stop_decoder(receiver);
        return false;
    }
    set_nonblocking(receiver->decoder_input);
    set_nonblocking(receiver->decoder_output);
    unsigned char header[IVF_FILE_HEADER] = { 'D', 'K', 'I', 'F', 0, 0, IVF_FILE_HEADER, 0, 'V', 'P', '8', '0' };
    write_little(header + 12, (uint64_t)width, 2);
    write_little(header + 14, (uint64_t)height, 2);
    write_little(header + 16, IVF_TIMEBASE, 4);
    write_little(header + 20, 1, 4);
    buffer_append(&receiver->to_decoder, header, sizeof header, MAX_DECODER_BACKLOG);
    receiver->decoded_count = 0;
    open_channel(peer, width, height);
    return true;
}

static void write_decoder(void *context) {
    struct receiver *receiver = &((struct peer *)context)->receiver;
    if (receiver->decoder_input >= 0 && !buffer_flush(&receiver->to_decoder, receiver->decoder_input)) {
        log_text(true, "the video decoder (ffmpeg) ended");
        stop_decoder(receiver);
    }
}

// A complete VP8 frame: a keyframe gives the frame size, and a new size starts a new decoder.
static void decode_frame(struct peer *peer, const unsigned char *frame, size_t size) {
    struct receiver *receiver = &peer->receiver;
    static const unsigned char START_CODE[] = { 0x9d, 0x01, 0x2a };
    if (!(frame[0] & 1)) {
        if (size < VP8_KEYFRAME_HEADER || memcmp(frame + 3, START_CODE, sizeof START_CODE)) return lose_frame(peer);
        int width = read16_little(frame + 6) & 0x3fff, height = read16_little(frame + 8) & 0x3fff;
        if (!width || !height) return lose_frame(peer);
        if ((!receiver->decoder || width != receiver->width || height != receiver->height) &&
            !start_decoder(peer, width, height))
            return;
        receiver->waiting_keyframe = false;
    } else if (receiver->waiting_keyframe || !receiver->decoder) {
        return request_keyframe(peer);
    }
    unsigned char header[IVF_FRAME_HEADER];
    write_little(header, size, 4);
    write_little(header + 4, receiver->decoded_count++, 8);
    if (!buffer_append(&receiver->to_decoder, header, sizeof header, MAX_DECODER_BACKLOG) ||
        !buffer_append(&receiver->to_decoder, frame, size, MAX_DECODER_BACKLOG))
        return lose_frame(peer); // the frame may be cut; the next keyframe starts clean
    write_decoder(peer);
}

// An RTP packet with VP8 (RFC 3550, RFC 7741). A gap in the sequence numbers drops the frame.
static void receive_packet(struct peer *peer, const unsigned char *packet, size_t size) {
    struct receiver *receiver = &peer->receiver;
    if (size < RTP_HEADER || packet[0] >> 6 != RTP_VERSION || (packet[1] & 0x7f) != peer->payload_type) return;
    size_t header = RTP_HEADER + 4 * (size_t)(packet[0] & 0x0f), end = size;
    if (packet[0] & 0x10) { // header extension
        if (size < header + 4) return;
        header += 4 + 4 * (size_t)read16(packet + header + 2);
    }
    if (packet[0] & 0x20) { // padding
        if (packet[size - 1] > size) return;
        end -= packet[size - 1];
    }
    if (header > end) return;
    uint16_t sequence = read16(packet + 2);
    if (receiver->sequenced && sequence != receiver->next_sequence) lose_frame(peer);
    receiver->sequenced = true;
    receiver->next_sequence = sequence + 1;
    const unsigned char *payload = packet + header;
    size_t length = end - header, descriptor = 1;
    if (!length) return; // padding only
    if (payload[0] & 0x80 && length > 1) { // extended control bits: picture id, TL0PICIDX, TID and KEYIDX
        unsigned char extension = payload[1];
        descriptor = 2;
        if (extension & 0x80) descriptor += descriptor < length && payload[descriptor] & 0x80 ? 2 : 1;
        if (extension & 0x40) descriptor++;
        if (extension & 0x30) descriptor++;
    }
    if (descriptor >= length) return;
    uint32_t timestamp = read32(packet + 4);
    if ((payload[0] & 0x17) == VP8_START) {
        receiver->assembling = true;
        receiver->encoded_size = 0;
        receiver->timestamp = timestamp;
    } else if (!receiver->assembling || timestamp != receiver->timestamp) {
        return;
    }
    size_t part = length - descriptor;
    if (!receiver->encoded) receiver->encoded = malloc(MAX_ENCODED_FRAME);
    if (!receiver->encoded || receiver->encoded_size + part > MAX_ENCODED_FRAME) return lose_frame(peer);
    memcpy(receiver->encoded + receiver->encoded_size, payload + descriptor, part);
    receiver->encoded_size += part;
    if (!(packet[1] & RTP_MARKER)) return;
    receiver->assembling = false;
    decode_frame(peer, receiver->encoded, receiver->encoded_size);
}

static void write_hydra(void *context) {
    struct receiver *receiver = &((struct peer *)context)->receiver;
    if (receiver->connection >= 0 && !buffer_flush(&receiver->to_hydra, receiver->connection)) {
        close_stream(&receiver->connection);
        buffer_free(&receiver->to_hydra);
    }
}

// RGBA frames of the decoder; one that comes while hydra-native still reads the one before is dropped.
static void read_decoder(void *context) {
    struct peer *peer = context;
    struct receiver *receiver = &peer->receiver;
    if (receiver->decoder_output < 0) return;
    size_t frame_size = (size_t)receiver->width * (size_t)receiver->height * RGBA_BYTES;
    ssize_t count = read(receiver->decoder_output, receiver->frame + receiver->frame_used,
                         frame_size - receiver->frame_used);
    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) {
        log_text(true, "the video decoder (ffmpeg) ended");
        stop_decoder(receiver);
        return lose_frame(peer);
    }
    if (count < 0) return;
    receiver->frame_used += (size_t)count;
    if (receiver->frame_used < frame_size) return;
    receiver->frame_used = 0;
    if (receiver->connection < 0 || !buffer_empty(&receiver->to_hydra)) return;
    buffer_append(&receiver->to_hydra, receiver->frame, frame_size, frame_size);
    write_hydra(peer);
}

// ---------------------------------------------------------------- the events of the signalling server

static void receive_message(const cJSON *data) {
    const char *type = member_string(data, "type");
    if (!type || strcmp(type, "signal")) {
        cJSON *line = message_new("message");
        cJSON_AddItemToObject(line, "data", cJSON_Duplicate(data, true));
        print_object(line);
        return;
    }
    const char *id = member_string(data, "id");
    const cJSON *signal = member_object(data, "message");
    if (!id || !signal) return;
    struct peer *peer = peer_with_id(id);
    if (!peer && !has_member(peer_list, id)) {
        log_text(true, "a signal of %s, which is not a peer of the room", id);
        return;
    }
    // simple-peer sends offers only as the initiator: an offer to an initiator comes from a new connection of the
    // other peer, which asked for our stream (init_source).
    const char *signal_type = member_string(signal, "type");
    if (peer && peer->initiator && signal_type && !strcmp(signal_type, "offer")) {
        peer_close(peer);
        peer = NULL;
    }
    if (!peer) peer = peer_create(id, false, true);
    if (peer) peer_signal(peer, signal);
}

static void receive_event(const cJSON *event) {
    if (!cJSON_IsArray(event) || cJSON_GetArraySize(event) < 2) return;
    const cJSON *first = cJSON_GetArrayItem(event, 0), *argument = cJSON_GetArrayItem(event, 1);
    const char *name = cJSON_IsString(first) ? first->valuestring : "";
    if (!strcmp(name, "ready")) {
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(argument, "peers");
        for (const cJSON *id = cJSON_IsArray(ids) ? ids->child : NULL; id; id = id->next)
            if (cJSON_IsString(id)) new_peer(id->valuestring);
        if (!nick) set_name(own_id);
    } else if (!strcmp(name, "new peer")) {
        if (cJSON_IsString(argument)) new_peer(argument->valuestring);
    } else if (!strcmp(name, "broadcast")) {
        receive_broadcast(argument);
    } else if (!strcmp(name, "message")) {
        receive_message(argument);
        return;
    } else {
        return;
    }
    send_state();
}

static void open_engine(const char *text) {
    cJSON *handshake = cJSON_Parse(text);
    free(stream_id);
    const char *sid = member_string(handshake, "sid");
    stream_id = sid ? strdup(sid) : NULL;
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(handshake, "pingInterval");
    if (cJSON_IsNumber(interval) && interval->valuedouble > 0) {
        double milliseconds = interval->valuedouble;
        ping_interval = milliseconds < MIN_PING_MS ? MIN_PING_MS : milliseconds > MAX_PING_MS ? MAX_PING_MS
                                                                                              : (int64_t)milliseconds;
        next_ping = now_ms() + ping_interval;
    }
    cJSON_Delete(handshake);
}

static void receive_signalling(const struct event *event) {
    const char *text = event->text;
    switch ((enum websocket_event)event->value) {
    case WEBSOCKET_OPEN:
        signalling_open = true;
        break;
    case WEBSOCKET_MESSAGE:
        if (text[0] == ENGINE_OPEN) {
            open_engine(text + 1);
        } else if (text[0] == ENGINE_MESSAGE && text[1] == SOCKET_CONNECT) {
            connected = true;
            for (int index = 0; index < unsent_count; index++) send_packet(unsent[index]);
            unsent_count = 0;
        } else if (text[0] == ENGINE_MESSAGE && text[1] == SOCKET_EVENT) {
            cJSON *packet = cJSON_Parse(text + 2);
            receive_event(packet);
            cJSON_Delete(packet);
        }
        break;
    case WEBSOCKET_ERROR:
        log_text(true, "the signalling server %s: %s", server, text);
        break;
    case WEBSOCKET_CLOSED:
        connected = signalling_open = false;
        ping_interval = 0;
        if (stopping) quitting = true;
        else log_text(true, "the signalling server %s closed the connection", server);
        break;
    }
}

static void handle_event(const struct event *event) {
    if (event->type == EVENT_SIGNALLING) return receive_signalling(event);
    struct peer *peer = event->type == EVENT_PACKET                 ? peer_with_track(event->id)
                        : event->type == EVENT_CHANNEL_OPEN ||
                                  event->type == EVENT_CHANNEL_CLOSED ? peer_with_channel(event->id)
                                                                      : peer_with_connection(event->id);
    if (!peer) {
        if (event->type == EVENT_DATA_CHANNEL) rtcDeleteDataChannel(event->value); // of a closed connection
        return;
    }
    cJSON *signal;
    switch (event->type) {
    case EVENT_DESCRIPTION:
        signal = message_new(event->text);
        cJSON_AddStringToObject(signal, "sdp", event->second);
        send_signal(peer, signal);
        if (!strcmp(event->text, "answer")) request_video(peer);
        break;
    case EVENT_CANDIDATE:
        signal = message_new("candidate");
        cJSON *candidate = cJSON_AddObjectToObject(signal, "candidate");
        cJSON_AddStringToObject(candidate, "candidate", event->text);
        if (event->second && event->second[0]) cJSON_AddStringToObject(candidate, "sdpMid", event->second);
        send_signal(peer, signal);
        break;
    case EVENT_STATE:
        if (event->value == RTC_FAILED || event->value == RTC_CLOSED) peer_close(peer);
        break;
    case EVENT_SIGNALING_STATE:
        if (event->value != RTC_SIGNALING_STABLE) break;
        peer->negotiating = false;
        if (!peer->negotiation_queued) break;
        peer->negotiation_queued = false;
        negotiate(peer);
        break;
    case EVENT_DATA_CHANNEL:
        // The data channel of the initiator (simple-peer) arrives open: it counts as the connection.
        if (peer->channel) {
            rtcDeleteDataChannel(event->value);
            break;
        }
        peer->channel = event->value;
        send_state();
        break;
    case EVENT_CHANNEL_OPEN:
        send_state();
        break;
    case EVENT_CHANNEL_CLOSED:
        peer_close(peer);
        break;
    case EVENT_PACKET:
        receive_packet(peer, (const unsigned char *)event->text, event->size);
        break;
    case EVENT_SIGNALLING:
        break;
    }
}

static void handle_events(void *context) {
    char drain[256];
    while (read(events.wake[0], drain, sizeof drain) > 0) {}
    struct event *event;
    while ((event = pop_event())) {
        if (!quitting) handle_event(event);
        free(event);
    }
}

static bool connect_signalling(void) {
    size_t length = strlen(server);
    while (length && server[length - 1] == '/') length--;
    const char *scheme = !strncmp(server, "https://", 8) ? "wss://" : !strncmp(server, "http://", 7) ? "ws://" : NULL;
    if (!scheme) return false;
    size_t skip = scheme[2] == 's' ? 8 : 7;
    char url[2048];
    if ((size_t)snprintf(url, sizeof url, "%s%.*s%s", scheme, (int)(length - skip), server + skip, SIGNALLING_PATH) >=
        sizeof url)
        return false;
    const char *certificates = NULL;
    for (size_t index = 0; index < sizeof CERTIFICATE_FILES / sizeof *CERTIFICATE_FILES && !certificates; index++)
        if (!access(CERTIFICATE_FILES[index], R_OK)) certificates = CERTIFICATE_FILES[index];
    if (!certificates && skip == 8) log_text(true, "no CA certificates to check the signalling server %s", server);
    return websocket_open(url, certificates, on_websocket);
}

// ---------------------------------------------------------------- the local socket

static void read_channel_line(void *context) {
    int slot = (int)(intptr_t)context;
    int stream = waiting[slot].stream;
    if (stream < 0) return;
    ssize_t count = read(stream, waiting[slot].line + waiting[slot].used, 1); // the frames follow the line
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (count <= 0) return close_stream(&waiting[slot].stream);
    if (waiting[slot].line[waiting[slot].used] != '\n') {
        if (++waiting[slot].used == MAX_CHANNEL_LINE) close_stream(&waiting[slot].stream);
        return;
    }
    waiting[slot].line[waiting[slot].used] = '\0';
    waiting[slot].stream = -1;
    char *line = waiting[slot].line, *format = strchr(line, ' ');
    if (format) *format++ = '\0';
    struct peer *peer = line[0] ? peer_with_channel_name(line) : NULL;
    if (!strcmp(line, "output")) {
        start_output(stream, format);
    } else if (peer) {
        close_stream(&peer->receiver.connection);
        buffer_free(&peer->receiver.to_hydra);
        peer->receiver.connection = stream;
    } else {
        close(stream);
    }
}

static void accept_connection(void *context) {
    int stream = accept(listener, NULL, NULL);
    if (stream < 0) return;
    fcntl(stream, F_SETFD, FD_CLOEXEC);
    set_nonblocking(stream);
    for (int slot = 0; slot < MAX_WAITING; slot++) {
        if (waiting[slot].stream >= 0) continue;
        waiting[slot].stream = stream;
        waiting[slot].used = 0;
        return;
    }
    close(stream);
}

static bool listen_local(void) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof address.sun_path) {
        fprintf(stderr, "hydra-native-patch-bay: the socket path %s is too long\n", socket_path);
        return false;
    }
    strcpy(address.sun_path, socket_path);
    unlink(socket_path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, MAX_WAITING)) {
        fprintf(stderr, "hydra-native-patch-bay: cannot listen on %s: %s\n", socket_path, strerror(errno));
        return false;
    }
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    set_nonblocking(listener);
    for (int slot = 0; slot < MAX_WAITING; slot++) waiting[slot].stream = -1;
    return true;
}

// ---------------------------------------------------------------- stdin and the end

// The end of the input, SIGTERM or SIGINT: leave the room as pb._destroy() does. Closed data channels tell the
// peers at once; the connections end with the process, so that the close of the WebSocket gives the channels the
// time to send.
static void shut_down(void) {
    if (stopping) return;
    stopping = true;
    stop_deadline = now_ms() + CLOSE_WAIT_MS;
    for (int index = 0; index < peer_count; index++)
        if (peers[index]->channel) rtcClose(peers[index]->channel);
    stop_output();
    if (signalling_open) websocket_close();
    else quitting = true;
}

static void call(const char *method, const cJSON *arguments) {
    const cJSON *first = cJSON_IsArray(arguments) ? cJSON_GetArrayItem(arguments, 0) : NULL;
    const char *text = cJSON_IsString(first) ? first->valuestring : NULL;
    if (!method) return;
    if (!strcmp(method, "initSource") && text) init_source(text);
    else if (!strcmp(method, "setName") && text) set_name(text);
    else if (!strcmp(method, "broadcast") && first) emit_event("broadcast", cJSON_Duplicate(first, true), NULL);
    else return;
    send_state();
}

static void read_input(void *context) {
    ssize_t count = read(STDIN_FILENO, input_line + input_used, MAX_LINE - 1 - input_used);
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (count <= 0) return shut_down();
    input_used += (size_t)count;
    char *start = input_line, *end;
    while ((end = memchr(start, '\n', input_used - (size_t)(start - input_line)))) {
        *end = '\0';
        cJSON *message = cJSON_Parse(start);
        const char *type = member_string(message, "type");
        if (type && !strcmp(type, "call"))
            call(member_string(message, "method"), cJSON_GetObjectItemCaseSensitive(message, "args"));
        cJSON_Delete(message);
        start = end + 1;
    }
    input_used -= (size_t)(start - input_line);
    memmove(input_line, start, input_used);
    if (input_used == MAX_LINE - 1) input_used = 0; // a line too long for the buffer is dropped
}

static void check_publish(void) {
    bool on = peer_count > 0;
    if (on == publishing) return;
    publishing = on;
    cJSON *message = message_new("publish");
    cJSON_AddBoolToObject(message, "on", on);
    print_object(message);
}

static void on_quit_signal(int number) {
    quit_signal = 1;
    ssize_t written = write(events.wake[1], "", 1);
    (void)written;
}

static void watch(int stream, short wanted, void (*action)(void *context), void *context) {
    if (stream < 0 || watches.count == MAX_WATCHES) return;
    watches.fds[watches.count] = (struct pollfd){ .fd = stream, .events = wanted };
    watches.actions[watches.count] = action;
    watches.contexts[watches.count++] = context;
}

// Each turn polls the streams of this moment. The actions that can close a peer (stdin and the events) come last,
// so that the actions before them never see a freed peer; each action uses the stream of its context, which an
// action before it may have closed.
static void run_once(void) {
    watches.count = 0;
    watch(listener, POLLIN, accept_connection, NULL);
    for (int slot = 0; slot < MAX_WAITING; slot++)
        watch(waiting[slot].stream, POLLIN, read_channel_line, (void *)(intptr_t)slot);
    watch(output.stream, POLLIN, read_output, NULL);
    watch(output.encoder_output, POLLIN, read_encoder, NULL);
    if (!buffer_empty(&output.to_encoder)) watch(output.encoder_input, POLLOUT, write_encoder, NULL);
    for (int index = 0; index < peer_count; index++) {
        struct receiver *receiver = &peers[index]->receiver;
        watch(receiver->decoder_output, POLLIN, read_decoder, peers[index]);
        if (!buffer_empty(&receiver->to_decoder)) watch(receiver->decoder_input, POLLOUT, write_decoder, peers[index]);
        if (!buffer_empty(&receiver->to_hydra)) watch(receiver->connection, POLLOUT, write_hydra, peers[index]);
    }
    if (!stopping) watch(STDIN_FILENO, POLLIN, read_input, NULL);
    watch(events.wake[0], POLLIN, handle_events, NULL);
    if (poll(watches.fds, (nfds_t)watches.count, POLL_MS) > 0) {
        for (int index = 0; index < watches.count; index++)
            if (watches.fds[index].revents) watches.actions[index](watches.contexts[index]);
    }
    int64_t now = now_ms();
    if (quit_signal) shut_down();
    if (stopping && now >= stop_deadline) quitting = true;
    if (ping_interval && signalling_open && now >= next_ping) {
        websocket_send(ENGINE_PING);
        next_ping = now + ping_interval;
    }
    check_publish();
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fputs("usage: hydra-native-patch-bay SERVER ROOM SOCKET\n", stderr);
        return 2;
    }
    server = argv[1];
    room = argv[2];
    socket_path = argv[3];
    signal(SIGPIPE, SIG_IGN); // hydra-native may close our stdout or a frame channel first
    if (pipe(events.wake) || !listen_local()) return 1;
    for (int index = 0; index < 2; index++) {
        fcntl(events.wake[index], F_SETFD, FD_CLOEXEC);
        set_nonblocking(events.wake[index]);
    }
    struct sigaction quit_action = { .sa_handler = on_quit_signal };
    sigaction(SIGTERM, &quit_action, NULL);
    sigaction(SIGINT, &quit_action, NULL);
    snprintf(own_id, sizeof own_id, "%08x", arc4random());
    nick_from_id = cJSON_CreateObject();
    id_from_nick = cJSON_CreateObject();
    peer_list = cJSON_CreateObject();

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "uuid", own_id);
    emit_event("join", cJSON_CreateString(room), user);
    if (!connect_signalling()) log_text(true, "invalid signalling server %s", server);

    while (!quitting) run_once();

    while (peer_count) peer_close(peers[0]);
    stop_output();
    for (int slot = 0; slot < MAX_WAITING; slot++) close_stream(&waiting[slot].stream);
    for (int index = 0; index < unsent_count; index++) free(unsent[index]);
    handle_events(NULL);
    close(listener);
    unlink(socket_path);
    cJSON_Delete(nick_from_id);
    cJSON_Delete(id_from_nick);
    cJSON_Delete(peer_list);
    free(nick);
    free(stream_id);
    return 0;
}
