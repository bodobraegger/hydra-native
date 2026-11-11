// hydra-native-patch-bay: the webRTC patch bay of hydra-native (pb, s0.initStream) on the webrtcbin of GStreamer. It
// acts as the patch bay of the hydra web editor (src/lib/patch-bay/rtc-patch-bay.js and pb-live.js of
// https://github.com/hydra-synth/hydra, with the peers of simple-peer 9): socket.io 2 signalling over a WebSocket of
// libsoup, one webrtcbin per peer, VP8 video in both directions. As in rtc-patch-bay.js, the peer that asks for a
// stream (initSource) is the initiator: it offers to receive video and opens the data channel, and the other peer
// answers with its output. A peer that asks for the stream of a peer that it only sends to starts the connection
// again as the initiator, and then sends its output too.
//
// usage: hydra-native-patch-bay SERVER ROOM SOCKET
// SOCKET is the path of a local socket for the frames. Each connection starts with one line that names its channel:
//   output WIDTHxHEIGHT                            hydra-native sends RGBA frames of its output (rows from the bottom)
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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixinputstream.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define PUBLISH_CHECK_MS 500
#define MIN_PING_MS 1000 // engine.io gives 25000
#define CLOSE_WAIT_MS 1000 // for the close handshake of the signalling WebSocket
#define ID_LENGTH 8 // pb.id, the start of a random UUID as in rtc-patch-bay.js
#define MAX_FRAME_SIDE 16384
#define MAX_PEERS 256 // the newest peers of the room in the tables: the signalling server tells no peer that leaves
#define MAX_UNSENT 256 // packets while the signalling server is not connected; more are dropped
#define MAX_CONNECTIONS 16 // peer connections at the same time, each a pipeline with threads of its own
#define RGBA_BYTES 4
#define DEFAULT_PAYLOAD_TYPE 96
#define MAX_PAYLOAD_TYPE 127 // RTP has 7 bits for it
#define NO_VIDEO (-1)
#define STUN_SERVER "stun://stun.l.google.com:19302" // the first STUN server of the defaults of simple-peer
#define DATA_CHANNEL_LABEL "hydra-native"
#define VIDEO_CAPS "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=%d,rtcp-fb-nack-pli=true"
// The output frames come with the rows from the bottom (glReadPixels). The queue of appsrc drops the oldest frame
// when the encoder is slower than hydra-native.
#define SENDER_PIPELINE                                                                                              \
    "appsrc name=source is-live=true format=time do-timestamp=true max-buffers=2 leaky-type=downstream"              \
    " ! videoflip method=vertical-flip ! videoconvert ! vp8enc deadline=1 ! rtpvp8pay pt=%d"
#define RECEIVER_PIPELINE                                                                                            \
    "rtpvp8depay ! vp8dec ! videoconvert ! video/x-raw,format=RGBA ! appsink name=sink sync=false max-buffers=1"     \
    " drop=true"
// Engine.IO 3 packets, and the socket.io 2 packets inside its message packets.
#define SIGNALLING_PATH "/socket.io/?EIO=3&transport=websocket"
#define ENGINE_OPEN '0'
#define ENGINE_PING "2"
#define ENGINE_MESSAGE '4'
#define SOCKET_CONNECT '0'
#define SOCKET_EVENT '2'
#define SOCKET_EVENT_PREFIX "42"

struct peer {
    char *id; // the socket id of the other peer
    bool initiator, closed;
    GstElement *pipeline, *webrtc;
    GstElement *source; // the appsrc of the output, or NULL while this connection only receives
    GstWebRTCDataChannel *channel;
    guint bus_watch;
    bool negotiating, negotiation_queued;
    // The received stream. A new frame size gives a new channel name.
    int number, width, height;
    char *channel_name;
    char *nick; // of the 'video' message, NULL before the first frame
    GSocketConnection *connection; // of hydra-native, which reads the frames
    bool writing;
    guint8 *frame;
};

static const char *server, *room, *socket_path;
static GMainLoop *loop;
static bool stopping;
static char own_id[ID_LENGTH + 1];
static char *nick, *stream_id;
// The tables of pb-live.js and rtc-patch-bay.js: JSON objects keep the order of their members, as JavaScript does.
static JsonObject *nick_from_id, *id_from_nick, *peer_list;
static GHashTable *peers; // socket id -> struct peer, the peer connections (rtcPeers)
static bool publishing;
static int received_streams;
static GDataInputStream *input;

static SoupSession *session;
static SoupWebsocketConnection *signaller;
static bool connected; // to the socket.io namespace
static guint ping_timer;
static GPtrArray *unsent; // event packets before the connection, which socket.io-client also buffers

// The frames of the output come through a GDataInputStream, which reads in a thread of the GLib pool. So each read
// has a buffer of its own, and a new output cancels the read of the old one instead of closing its socket under it.
static struct {
    GSocketConnection *connection;
    GDataInputStream *input;
    GCancellable *cancellable;
    gsize frame_size;
    GstCaps *caps; // of the newest output, also for the peers that start later
} output;

static void peer_close(struct peer *peer);
static void send_state(void);

// ---------------------------------------------------------------- JSON lines

static JsonNode *object_node(JsonObject *object) {
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    return node;
}

static JsonNode *parse_json(const char *text) {
    JsonParser *parser = json_parser_new();
    JsonNode *root = json_parser_load_from_data(parser, text, -1, NULL) ? json_parser_steal_root(parser) : NULL;
    g_object_unref(parser);
    return root;
}

static const char *node_string(JsonNode *node) {
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
               ? json_node_get_string(node)
               : NULL;
}

static JsonObject *node_object(JsonNode *node) {
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static const char *member_string(JsonObject *object, const char *name) {
    return object ? node_string(json_object_get_member(object, name)) : NULL;
}

// Writes one line to stdout, from any thread: printf holds the lock of stdout for the whole line.
static void print_object(JsonObject *object) {
    JsonNode *node = object_node(object);
    char *text = json_to_string(node, FALSE);
    printf("%s\n", text);
    fflush(stdout);
    g_free(text);
    json_node_unref(node);
}

static JsonObject *message_new(const char *type) {
    JsonObject *message = json_object_new();
    json_object_set_string_member(message, "type", type);
    return message;
}

G_GNUC_PRINTF(2, 3) static void log_text(bool error, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char *text = g_strdup_vprintf(format, arguments);
    va_end(arguments);
    JsonObject *message = message_new("log");
    json_object_set_string_member(message, "text", text);
    if (error) json_object_set_boolean_member(message, "error", TRUE);
    print_object(message);
    g_free(text);
}

static JsonArray *member_names(JsonObject *object) {
    JsonArray *names = json_array_new();
    GList *members = json_object_get_members(object);
    for (GList *item = members; item; item = item->next) json_array_add_string_element(names, item->data);
    g_list_free(members);
    return names;
}

static void send_state(void) {
    JsonObject *state = message_new("state");
    json_object_set_string_member(state, "id", own_id);
    if (stream_id) json_object_set_string_member(state, "streamId", stream_id);
    if (nick) json_object_set_string_member(state, "nick", nick);
    json_object_set_array_member(state, "nicks", member_names(id_from_nick));
    json_object_set_array_member(state, "peers", member_names(peer_list));
    print_object(state);
}

// ---------------------------------------------------------------- signalling (socket.io 2)

static void send_packet(char *text) {
    if (connected && soup_websocket_connection_get_state(signaller) == SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_send_text(signaller, text);
        g_free(text);
    } else if (unsent->len < MAX_UNSENT) {
        g_ptr_array_add(unsent, text);
    } else {
        g_free(text);
    }
}

// Emits a socket.io event with one or two arguments, which it takes.
static void emit_event(const char *name, JsonNode *first, JsonNode *second) {
    JsonArray *packet = json_array_new();
    json_array_add_string_element(packet, name);
    json_array_add_element(packet, first);
    if (second) json_array_add_element(packet, second);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, packet);
    char *json = json_to_string(node, FALSE);
    send_packet(g_strconcat(SOCKET_EVENT_PREFIX, json, NULL));
    g_free(json);
    json_node_unref(node);
}

static void broadcast_nick(const char *previous) {
    JsonObject *update = message_new("update-nick");
    json_object_set_string_member(update, "id", own_id);
    json_object_set_string_member(update, "nick", nick);
    if (previous) json_object_set_string_member(update, "previous", previous);
    emit_event("broadcast", object_node(update), NULL);
}

static void set_name(const char *name) {
    char *previous = nick;
    nick = g_strdup(name);
    broadcast_nick(previous);
    g_free(previous);
}

static void forget_peer(const char *id) {
    const char *name = member_string(nick_from_id, id);
    if (name && !g_strcmp0(member_string(id_from_nick, name), id)) json_object_remove_member(id_from_nick, name);
    json_object_remove_member(nick_from_id, id);
    json_object_remove_member(peer_list, id);
}

static void new_peer(const char *id) {
    if (!json_object_has_member(peer_list, id) && json_object_get_size(peer_list) >= MAX_PEERS) {
        GList *members = json_object_get_members(peer_list);
        forget_peer(members->data);
        g_list_free(members);
    }
    json_object_set_null_member(peer_list, id);
    json_object_set_string_member(nick_from_id, id, id);
    json_object_set_string_member(id_from_nick, id, id);
    if (nick) broadcast_nick(NULL);
}

static void receive_broadcast(JsonNode *data) {
    JsonObject *message = message_new("broadcast");
    json_object_set_member(message, "data", json_node_copy(data));
    print_object(message);
    JsonObject *update = node_object(data);
    const char *id = member_string(update, "id"), *name = member_string(update, "nick");
    if (g_strcmp0(member_string(update, "type"), "update-nick") || !id || !name ||
        !json_object_has_member(peer_list, id) || !g_strcmp0(member_string(update, "previous"), name))
        return;
    const char *old = member_string(nick_from_id, id);
    if (old) json_object_remove_member(id_from_nick, old);
    json_object_set_string_member(nick_from_id, id, name);
    json_object_set_string_member(id_from_nick, name, id);
}

// ---------------------------------------------------------------- peers on webrtcbin

typedef void (*peer_task)(struct peer *peer, gpointer data);

struct task {
    struct peer *peer;
    peer_task run;
    gpointer data;
    GDestroyNotify free_data;
};

static void peer_clear(gpointer pointer) {
    struct peer *peer = pointer;
    g_free(peer->id);
    g_free(peer->channel_name);
    g_free(peer->nick);
    g_free(peer->frame);
    if (peer->source) gst_object_unref(peer->source);
    if (peer->channel) g_object_unref(peer->channel);
    gst_object_unref(peer->webrtc);
    gst_object_unref(peer->pipeline);
}

static struct peer *peer_acquire(struct peer *peer) { return g_atomic_rc_box_acquire(peer); }
static void peer_release(gpointer peer) { g_atomic_rc_box_release_full(peer, peer_clear); }

static gboolean run_task(gpointer pointer) {
    struct task *task = pointer;
    if (!task->peer->closed) task->run(task->peer, task->data);
    return G_SOURCE_REMOVE;
}

static void free_task(gpointer pointer) {
    struct task *task = pointer;
    if (task->free_data) task->free_data(task->data);
    peer_release(task->peer);
    g_free(task);
}

// Runs `run` on the main thread: webrtcbin and the streaming threads of GStreamer call from threads of their own.
static void schedule(struct peer *peer, peer_task run, gpointer data, GDestroyNotify free_data) {
    struct task *task = g_new(struct task, 1);
    *task = (struct task){ peer_acquire(peer), run, data, free_data };
    g_idle_add_full(G_PRIORITY_DEFAULT, run_task, task, free_task);
}

static void send_signal(struct peer *peer, gpointer signal) {
    JsonObject *message = json_object_new();
    json_object_set_string_member(message, "id", peer->id);
    json_object_set_member(message, "message", json_node_copy(signal));
    json_object_set_string_member(message, "type", "signal");
    emit_event("message", object_node(message), NULL);
}

static void schedule_signal(struct peer *peer, JsonObject *signal) {
    schedule(peer, send_signal, object_node(signal), (GDestroyNotify)json_node_unref);
}

static GstCaps *video_caps(int payload_type) {
    char *text = g_strdup_printf(VIDEO_CAPS, payload_type);
    GstCaps *caps = gst_caps_from_string(text);
    g_free(text);
    return caps;
}

static void close_connection(GSocketConnection **connection) {
    if (!*connection) return;
    g_socket_close(g_socket_connection_get_socket(*connection), NULL);
    g_clear_object(connection);
}

// simple-peer: a peer that answers an offer without video asks the initiator for a video transceiver. The web
// editor offers only the data channel, because simple-peer 9 ignores its offerConstraints.
static void request_video(struct peer *peer, gpointer data) {
    if (peer->source) return;
    JsonObject *request = json_object_new();
    json_object_set_string_member(request, "kind", "video");
    JsonObject *signal = message_new("transceiverRequest");
    json_object_set_object_member(signal, "transceiverRequest", request);
    JsonNode *node = object_node(signal);
    send_signal(peer, node);
    json_node_unref(node);
}

// An offer or an answer of webrtcbin: it becomes the local description and goes to the peer.
static void on_description_created(GstPromise *promise, gpointer data) {
    struct peer *peer = data;
    const GstStructure *reply = gst_promise_get_reply(promise);
    const char *field = reply && gst_structure_has_field(reply, "offer") ? "offer" : "answer";
    GstWebRTCSessionDescription *description = NULL;
    if (reply) gst_structure_get(reply, field, GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description, NULL);
    gst_promise_unref(promise);
    if (!description) {
        log_text(true, "webrtcbin made no %s", field);
        return;
    }
    g_signal_emit_by_name(peer->webrtc, "set-local-description", description, NULL);
    bool answer = description->type == GST_WEBRTC_SDP_TYPE_ANSWER;
    JsonObject *signal = message_new(gst_webrtc_sdp_type_to_string(description->type));
    char *sdp = gst_sdp_message_as_text(description->sdp);
    json_object_set_string_member(signal, "sdp", sdp);
    g_free(sdp);
    gst_webrtc_session_description_free(description);
    schedule_signal(peer, signal);
    if (answer) schedule(peer, request_video, NULL, NULL);
}

static GstPromise *description_promise(struct peer *peer) {
    return gst_promise_new_with_change_func(on_description_created, peer_acquire(peer), peer_release);
}

// simple-peer: an initiator queues a negotiation that comes during another one.
static void negotiate(struct peer *peer) {
    if (peer->negotiating) {
        peer->negotiation_queued = true;
        return;
    }
    peer->negotiating = true;
    g_signal_emit_by_name(peer->webrtc, "create-offer", NULL, description_promise(peer));
}

static void start_negotiation(struct peer *peer, gpointer data) {
    if (peer->initiator) negotiate(peer);
}

static void check_signaling_state(struct peer *peer, gpointer data) {
    GstWebRTCSignalingState state;
    g_object_get(peer->webrtc, "signaling-state", &state, NULL);
    if (state != GST_WEBRTC_SIGNALING_STATE_STABLE) return;
    peer->negotiating = false;
    if (!peer->negotiation_queued) return;
    peer->negotiation_queued = false;
    negotiate(peer);
}

static void check_ice_state(struct peer *peer, gpointer data) {
    GstWebRTCICEConnectionState state;
    g_object_get(peer->webrtc, "ice-connection-state", &state, NULL);
    if (state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED || state == GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED)
        peer_close(peer);
}

static void close_peer_task(struct peer *peer, gpointer data) { peer_close(peer); }
static void connected_task(struct peer *peer, gpointer data) { send_state(); }

static void on_negotiation_needed(GstElement *webrtc, gpointer peer) { schedule(peer, start_negotiation, NULL, NULL); }

static void on_signaling_state(GObject *webrtc, GParamSpec *property, gpointer peer) {
    schedule(peer, check_signaling_state, NULL, NULL);
}

static void on_ice_state(GObject *webrtc, GParamSpec *property, gpointer peer) {
    schedule(peer, check_ice_state, NULL, NULL);
}

static void on_ice_candidate(GstElement *webrtc, guint line, const char *text, gpointer peer) {
    JsonObject *candidate = json_object_new();
    json_object_set_string_member(candidate, "candidate", text);
    json_object_set_int_member(candidate, "sdpMLineIndex", line);
    GstWebRTCSessionDescription *local = NULL;
    g_object_get(webrtc, "local-description", &local, NULL);
    if (local && line < gst_sdp_message_medias_len(local->sdp)) {
        const char *mid = gst_sdp_media_get_attribute_val(gst_sdp_message_get_media(local->sdp, line), "mid");
        if (mid) json_object_set_string_member(candidate, "sdpMid", mid);
    }
    if (local) gst_webrtc_session_description_free(local);
    JsonObject *signal = message_new("candidate");
    json_object_set_object_member(signal, "candidate", candidate);
    schedule_signal(peer, signal);
}

static void on_channel_open(GstWebRTCDataChannel *channel, gpointer peer) {
    schedule(peer, connected_task, NULL, NULL);
}

static void on_channel_close(GstWebRTCDataChannel *channel, gpointer peer) {
    schedule(peer, close_peer_task, NULL, NULL);
}

static void watch_channel(struct peer *peer) {
    g_signal_connect(peer->channel, "on-open", G_CALLBACK(on_channel_open), peer);
    g_signal_connect(peer->channel, "on-close", G_CALLBACK(on_channel_close), peer);
}

// The data channel of the initiator (simple-peer) arrives open: it counts as the connection.
static void adopt_channel(struct peer *peer, gpointer channel) {
    if (peer->channel) return;
    peer->channel = g_object_ref(channel);
    watch_channel(peer);
    send_state();
}

static void on_data_channel(GstElement *webrtc, GstWebRTCDataChannel *channel, gpointer peer) {
    schedule(peer, adopt_channel, g_object_ref(channel), g_object_unref);
}

static void on_frame_written(GObject *source, GAsyncResult *result, gpointer data) {
    struct peer *peer = data;
    bool written = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);
    peer->writing = false;
    if (!written && peer->connection &&
        source == G_OBJECT(g_io_stream_get_output_stream(G_IO_STREAM(peer->connection))))
        close_connection(&peer->connection);
    peer_release(peer);
}

// The first frame of a stream announces it as 'got video' of pb-live.js; a new frame size opens a new channel.
static void open_channel(struct peer *peer, int width, int height) {
    close_connection(&peer->connection);
    if (!peer->number) peer->number = ++received_streams;
    peer->width = width;
    peer->height = height;
    g_free(peer->channel_name);
    peer->channel_name = g_strdup_printf("stream-%d-%dx%d", peer->number, width, height);
    peer->frame = g_realloc(peer->frame, (gsize)width * height * RGBA_BYTES);
    JsonObject *message = message_new("video");
    if (peer->nick) json_object_set_boolean_member(message, "resized", TRUE);
    else peer->nick = g_strdup(json_object_get_string_member_with_default(nick_from_id, peer->id, peer->id));
    json_object_set_string_member(message, "nick", peer->nick);
    json_object_set_string_member(message, "channel", peer->channel_name);
    json_object_set_int_member(message, "width", width);
    json_object_set_int_member(message, "height", height);
    print_object(message);
}

static void receive_frame(struct peer *peer, gpointer sample) {
    const GstStructure *format = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    int width = 0, height = 0;
    gst_structure_get_int(format, "width", &width);
    gst_structure_get_int(format, "height", &height);
    if (width <= 0 || height <= 0) return;
    if (!peer->channel_name || width != peer->width || height != peer->height) open_channel(peer, width, height);
    // Not connected (yet), or hydra-native reads slower than frames come: drop this frame.
    if (!peer->connection || peer->writing) return;
    gsize size = (gsize)width * height * RGBA_BYTES;
    if (gst_buffer_extract(gst_sample_get_buffer(sample), 0, peer->frame, size) != size) return;
    peer->writing = true;
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(peer->connection)), peer->frame, size,
                                    G_PRIORITY_DEFAULT, NULL, on_frame_written, peer_acquire(peer));
}

static GstFlowReturn on_sample(GstAppSink *sink, gpointer peer) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample) schedule(peer, receive_frame, sample, (GDestroyNotify)gst_sample_unref);
    return GST_FLOW_OK;
}

static void on_pad_added(GstElement *webrtc, GstPad *pad, gpointer data) {
    struct peer *peer = data;
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    const char *encoding = caps ? gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name") : NULL;
    bool video = encoding && !g_ascii_strcasecmp(encoding, "VP8");
    if (caps) gst_caps_unref(caps);
    GError *error = NULL;
    GstElement *receiver = gst_parse_bin_from_description(video ? RECEIVER_PIPELINE : "fakesink", TRUE, &error);
    if (!receiver) {
        log_text(true, "cannot receive the stream: %s", error->message);
        g_error_free(error);
        return;
    }
    if (video) {
        GstElement *sink = gst_bin_get_by_name(GST_BIN(receiver), "sink");
        GstAppSinkCallbacks callbacks = { .new_sample = on_sample };
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, peer, NULL);
        gst_object_unref(sink);
    }
    gst_bin_add(GST_BIN(peer->pipeline), receiver);
    gst_element_sync_state_with_parent(receiver);
    GstPad *sink_pad = gst_element_get_static_pad(receiver, "sink");
    if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK) log_text(true, "cannot link the received stream");
    gst_object_unref(sink_pad);
}

static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer data) {
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR) return G_SOURCE_CONTINUE;
    GError *error = NULL;
    gst_message_parse_error(message, &error, NULL);
    log_text(true, "%s: %s", GST_OBJECT_NAME(message->src), error->message);
    g_error_free(error);
    return G_SOURCE_CONTINUE;
}

static void set_codec(GstWebRTCRTPTransceiver *transceiver, int payload_type) {
    GstCaps *caps = video_caps(payload_type);
    g_object_set(transceiver, "codec-preferences", caps, NULL);
    gst_caps_unref(caps);
}

static bool has_transceiver(struct peer *peer) {
    GstWebRTCRTPTransceiver *transceiver = NULL;
    g_signal_emit_by_name(peer->webrtc, "get-transceiver", 0, &transceiver);
    if (transceiver) gst_object_unref(transceiver);
    return transceiver != NULL;
}

static void add_receiver(struct peer *peer) {
    GstCaps *caps = video_caps(DEFAULT_PAYLOAD_TYPE);
    GstWebRTCRTPTransceiver *transceiver = NULL;
    g_signal_emit_by_name(peer->webrtc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, caps,
                          &transceiver);
    gst_caps_unref(caps);
    if (transceiver) gst_object_unref(transceiver);
}

// The output of hydra-native to the peer, with the payload type of VP8 in the offer of the peer.
static void add_sender(struct peer *peer, int payload_type) {
    char *description = g_strdup_printf(SENDER_PIPELINE, payload_type);
    GError *error = NULL;
    GstElement *sender = gst_parse_bin_from_description(description, TRUE, &error);
    g_free(description);
    if (!sender) {
        log_text(true, "cannot send the output: %s", error->message);
        g_error_free(error);
        return;
    }
    peer->source = gst_bin_get_by_name(GST_BIN(sender), "source");
    if (output.caps) gst_app_src_set_caps(GST_APP_SRC(peer->source), output.caps);
    gst_bin_add(GST_BIN(peer->pipeline), sender);
    GstPad *pad = gst_element_request_pad_simple(peer->webrtc, "sink_%u");
    GstPad *source_pad = gst_element_get_static_pad(sender, "src");
    if (gst_pad_link(source_pad, pad) != GST_PAD_LINK_OK) log_text(true, "cannot link the output");
    GstWebRTCRTPTransceiver *transceiver = NULL;
    g_object_get(pad, "transceiver", &transceiver, NULL);
    set_codec(transceiver, payload_type);
    if (peer->initiator) g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV, NULL);
    gst_object_unref(transceiver);
    gst_object_unref(source_pad);
    gst_object_unref(pad);
    gst_element_sync_state_with_parent(sender);
}

// The payload type of VP8 in a description, DEFAULT_PAYLOAD_TYPE for video without VP8, or NO_VIDEO.
static int vp8_payload_type(const GstSDPMessage *sdp) {
    int found = NO_VIDEO;
    for (guint index = 0; index < gst_sdp_message_medias_len(sdp); index++) {
        const GstSDPMedia *media = gst_sdp_message_get_media(sdp, index);
        if (g_strcmp0(gst_sdp_media_get_media(media), "video")) continue;
        found = DEFAULT_PAYLOAD_TYPE;
        for (guint attribute = 0; attribute < gst_sdp_media_attributes_len(media); attribute++) {
            const GstSDPAttribute *map = gst_sdp_media_get_attribute(media, attribute);
            int payload_type;
            char encoding[16];
            if (!g_strcmp0(map->key, "rtpmap") && map->value &&
                sscanf(map->value, "%d %15[^/]", &payload_type, encoding) == 2 && payload_type >= 0 &&
                payload_type <= MAX_PAYLOAD_TYPE && !g_ascii_strcasecmp(encoding, "VP8"))
                return payload_type;
        }
    }
    return found;
}

static void on_remote_offer_set(GstPromise *promise, gpointer peer) {
    const GstStructure *reply = gst_promise_get_reply(promise);
    bool failed = reply && gst_structure_has_field(reply, "error");
    gst_promise_unref(promise);
    if (failed) log_text(true, "webrtcbin refused the offer of the peer");
    else g_signal_emit_by_name(((struct peer *)peer)->webrtc, "create-answer", NULL, description_promise(peer));
}

static void set_remote_description(struct peer *peer, const char *type, const char *text) {
    GstWebRTCSDPType sdp_type = !g_strcmp0(type, "offer")    ? GST_WEBRTC_SDP_TYPE_OFFER
                                : !g_strcmp0(type, "answer") ? GST_WEBRTC_SDP_TYPE_ANSWER
                                                             : 0;
    GstSDPMessage *sdp = NULL;
    if (!sdp_type || gst_sdp_message_new_from_text(text, &sdp) != GST_SDP_OK) {
        log_text(true, "a signal with an invalid description");
        if (sdp) gst_sdp_message_free(sdp);
        return;
    }
    int payload_type = sdp_type == GST_WEBRTC_SDP_TYPE_OFFER ? vp8_payload_type(sdp) : NO_VIDEO;
    if (payload_type != NO_VIDEO && !peer->initiator && !peer->source) add_sender(peer, payload_type);
    GstWebRTCSessionDescription *description = gst_webrtc_session_description_new(sdp_type, sdp);
    GstPromise *promise = sdp_type == GST_WEBRTC_SDP_TYPE_OFFER
                              ? gst_promise_new_with_change_func(on_remote_offer_set, peer_acquire(peer), peer_release)
                              : NULL;
    g_signal_emit_by_name(peer->webrtc, "set-remote-description", description, promise);
    gst_webrtc_session_description_free(description);
}

// A signal of simple-peer: a description, a candidate, or a request of the non-initiator to negotiate again.
static void peer_signal(struct peer *peer, JsonObject *signal) {
    JsonNode *renegotiate = json_object_get_member(signal, "renegotiate");
    if (peer->initiator && renegotiate && JSON_NODE_HOLDS_VALUE(renegotiate) && json_node_get_boolean(renegotiate))
        negotiate(peer);
    JsonObject *request = node_object(json_object_get_member(signal, "transceiverRequest"));
    // An initiator has its video transceiver from the start: a request adds one only if that failed.
    if (peer->initiator && request) {
        if (g_strcmp0(member_string(request, "kind"), "video")) log_text(false, "the patch bay receives only video");
        else if (!has_transceiver(peer)) add_receiver(peer); // webrtcbin then negotiates
    }
    JsonObject *candidate = node_object(json_object_get_member(signal, "candidate"));
    const char *text = member_string(candidate, "candidate");
    if (text && text[0]) {
        guint line = (guint)json_object_get_int_member_with_default(candidate, "sdpMLineIndex", 0);
        g_signal_emit_by_name(peer->webrtc, "add-ice-candidate", line, text);
    }
    const char *sdp = member_string(signal, "sdp");
    if (sdp) set_remote_description(peer, member_string(signal, "type"), sdp);
}

static struct peer *peer_create(const char *id, bool initiator, bool send) {
    if (g_hash_table_size(peers) >= MAX_CONNECTIONS) {
        log_text(true, "no connection to %s: the patch bay has %d connections", id, MAX_CONNECTIONS);
        return NULL;
    }
    GstElement *webrtc = gst_element_factory_make("webrtcbin", NULL);
    if (!webrtc) {
        log_text(true, "no webrtcbin: install gstreamer1.0-plugins-bad and gstreamer1.0-nice");
        return NULL;
    }
    struct peer *peer = g_atomic_rc_box_new0(struct peer);
    peer->id = g_strdup(id);
    peer->initiator = initiator;
    peer->pipeline = gst_pipeline_new(NULL);
    peer->webrtc = gst_object_ref(webrtc);
    g_object_set(webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, "stun-server", STUN_SERVER, NULL);
    gst_bin_add(GST_BIN(peer->pipeline), webrtc);
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), peer);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), peer);
    g_signal_connect(webrtc, "notify::signaling-state", G_CALLBACK(on_signaling_state), peer);
    g_signal_connect(webrtc, "notify::ice-connection-state", G_CALLBACK(on_ice_state), peer);
    g_signal_connect(webrtc, "on-data-channel", G_CALLBACK(on_data_channel), peer);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(on_pad_added), peer);
    GstBus *bus = gst_element_get_bus(peer->pipeline);
    peer->bus_watch = gst_bus_add_watch(bus, on_bus_message, peer);
    gst_object_unref(bus);
    g_hash_table_insert(peers, peer->id, peer);
    // A non-initiator adds its output when an offer with video comes, with the payload type of the offer.
    if (initiator && send) add_sender(peer, DEFAULT_PAYLOAD_TYPE);
    else if (initiator) add_receiver(peer);
    gst_element_set_state(peer->pipeline, GST_STATE_PLAYING);
    if (initiator) {
        g_signal_emit_by_name(webrtc, "create-data-channel", DATA_CHANNEL_LABEL, NULL, &peer->channel);
        if (peer->channel) watch_channel(peer);
    }
    send_state();
    return peer;
}

static void peer_close(struct peer *peer) {
    if (peer->closed) return;
    peer->closed = true;
    g_source_remove(peer->bus_watch);
    gst_element_set_state(peer->pipeline, GST_STATE_NULL);
    g_signal_handlers_disconnect_by_data(peer->webrtc, peer);
    if (peer->channel) g_signal_handlers_disconnect_by_data(peer->channel, peer);
    close_connection(&peer->connection);
    if (peer->nick) {
        JsonObject *message = message_new("video closed");
        json_object_set_string_member(message, "nick", peer->nick);
        print_object(message);
    }
    if (g_hash_table_lookup(peers, peer->id) == peer) g_hash_table_remove(peers, peer->id); // releases the peer
    send_state();
}

static void init_source(const char *name) {
    const char *id = member_string(id_from_nick, name);
    if (!id) return;
    struct peer *peer = g_hash_table_lookup(peers, id);
    if (!peer) {
        peer_create(id, true, false);
    } else if (!peer->initiator) {
        // rtc-patch-bay.js: a connection that only sends starts again in both directions.
        peer_close(peer);
        peer_create(id, true, true);
    }
}

// ---------------------------------------------------------------- the events of the signalling server

static void receive_message(JsonNode *data) {
    JsonObject *message = node_object(data);
    if (g_strcmp0(member_string(message, "type"), "signal")) {
        JsonObject *line = message_new("message");
        json_object_set_member(line, "data", json_node_copy(data));
        print_object(line);
        return;
    }
    const char *id = member_string(message, "id");
    JsonObject *signal = node_object(json_object_get_member(message, "message"));
    if (!id || !signal) return;
    struct peer *peer = g_hash_table_lookup(peers, id);
    if (!peer && !json_object_has_member(peer_list, id)) return; // not a peer of the room
    // simple-peer sends offers only as the initiator: an offer to an initiator comes from a new connection of the
    // other peer, which asked for our stream (init_source).
    if (peer && peer->initiator && !g_strcmp0(member_string(signal, "type"), "offer")) {
        peer_close(peer);
        peer = NULL;
    }
    if (!peer) peer = peer_create(id, false, true);
    if (peer) peer_signal(peer, signal);
}

static void receive_event(JsonArray *event) {
    if (json_array_get_length(event) < 2) return;
    const char *name = node_string(json_array_get_element(event, 0));
    JsonNode *argument = json_array_get_element(event, 1);
    if (!g_strcmp0(name, "ready")) {
        JsonObject *ready = node_object(argument);
        JsonNode *list = ready ? json_object_get_member(ready, "peers") : NULL;
        JsonArray *ids = list && JSON_NODE_HOLDS_ARRAY(list) ? json_node_get_array(list) : NULL;
        for (guint index = 0; ids && index < json_array_get_length(ids); index++) {
            const char *id = node_string(json_array_get_element(ids, index));
            if (id) new_peer(id);
        }
        if (!nick) set_name(own_id);
    } else if (!g_strcmp0(name, "new peer")) {
        const char *id = node_string(argument);
        if (id) new_peer(id);
    } else if (!g_strcmp0(name, "broadcast")) {
        receive_broadcast(argument);
    } else if (!g_strcmp0(name, "message")) {
        receive_message(argument);
        return;
    } else {
        return;
    }
    send_state();
}

static gboolean send_ping(gpointer data) {
    if (soup_websocket_connection_get_state(signaller) != SOUP_WEBSOCKET_STATE_OPEN) {
        ping_timer = 0;
        return G_SOURCE_REMOVE;
    }
    soup_websocket_connection_send_text(signaller, ENGINE_PING);
    return G_SOURCE_CONTINUE;
}

static void open_engine(const char *text) {
    JsonNode *node = parse_json(text);
    JsonObject *handshake = node_object(node);
    g_free(stream_id);
    stream_id = g_strdup(member_string(handshake, "sid"));
    gint64 interval = handshake ? json_object_get_int_member_with_default(handshake, "pingInterval", 0) : 0;
    if (node) json_node_unref(node);
    if (interval <= 0) return;
    g_clear_handle_id(&ping_timer, g_source_remove);
    ping_timer = g_timeout_add((guint)CLAMP(interval, MIN_PING_MS, G_MAXINT), send_ping, NULL);
}

static void on_signalling_message(SoupWebsocketConnection *connection, gint type, GBytes *bytes, gpointer data) {
    gsize size;
    const char *bytes_data = g_bytes_get_data(bytes, &size);
    char *text = g_strndup(bytes_data, size);
    if (text[0] == ENGINE_OPEN) {
        open_engine(text + 1);
    } else if (text[0] == ENGINE_MESSAGE && text[1] == SOCKET_CONNECT) {
        connected = true;
        for (guint index = 0; index < unsent->len; index++)
            soup_websocket_connection_send_text(signaller, g_ptr_array_index(unsent, index));
        g_ptr_array_set_size(unsent, 0);
    } else if (text[0] == ENGINE_MESSAGE && text[1] == SOCKET_EVENT) {
        JsonNode *node = parse_json(text + 2);
        if (node && JSON_NODE_HOLDS_ARRAY(node)) receive_event(json_node_get_array(node));
        if (node) json_node_unref(node);
    }
    g_free(text);
}

static void on_signalling_closed(SoupWebsocketConnection *connection, gpointer data) {
    connected = false;
    if (stopping) g_main_loop_quit(loop);
    else log_text(true, "the signalling server %s closed the connection", server);
}

static void on_signalling_connected(GObject *source, GAsyncResult *result, gpointer data) {
    GError *error = NULL;
    signaller = soup_session_websocket_connect_finish(session, result, &error);
    if (!signaller) {
        log_text(true, "cannot connect to the signalling server %s: %s", server, error->message);
        g_error_free(error);
        if (stopping) g_main_loop_quit(loop);
        return;
    }
    g_signal_connect(signaller, "message", G_CALLBACK(on_signalling_message), NULL);
    g_signal_connect(signaller, "closed", G_CALLBACK(on_signalling_closed), NULL);
    if (stopping) soup_websocket_connection_close(signaller, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
}

static bool connect_signalling(void) {
    size_t length = strlen(server);
    while (length && server[length - 1] == '/') length--;
    char *url = g_strdup_printf("%.*s%s", (int)length, server, SIGNALLING_PATH);
    SoupMessage *message = soup_message_new(SOUP_METHOD_GET, url);
    g_free(url);
    if (!message) return false;
    session = soup_session_new();
    soup_session_websocket_connect_async(session, message, NULL, NULL, G_PRIORITY_DEFAULT, NULL,
                                         on_signalling_connected, NULL);
    g_object_unref(message);
    return true;
}

// ---------------------------------------------------------------- the local socket

static void stop_output(void) {
    if (output.cancellable) g_cancellable_cancel(output.cancellable);
    g_clear_object(&output.cancellable);
    g_clear_object(&output.connection);
    g_clear_object(&output.input);
}

static void on_output_frame(GObject *source, GAsyncResult *result, gpointer data);

static void read_output_frame(void) {
    guint8 *frame = g_malloc(output.frame_size);
    g_input_stream_read_all_async(G_INPUT_STREAM(output.input), frame, output.frame_size, G_PRIORITY_DEFAULT,
                                  output.cancellable, on_output_frame, frame);
}

static void on_output_frame(GObject *source, GAsyncResult *result, gpointer data) {
    gsize count = 0;
    bool read = g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &count, NULL);
    guint8 *frame = data;
    if (source != G_OBJECT(output.input)) return g_free(frame); // the stream of a closed output
    if (!read || count < output.frame_size) {
        g_free(frame);
        return stop_output();
    }
    GstBuffer *buffer = gst_buffer_new_wrapped(frame, output.frame_size);
    GHashTableIter iterator;
    struct peer *peer;
    g_hash_table_iter_init(&iterator, peers);
    while (g_hash_table_iter_next(&iterator, NULL, (gpointer *)&peer))
        if (peer->source) gst_app_src_push_buffer(GST_APP_SRC(peer->source), gst_buffer_ref(buffer));
    gst_buffer_unref(buffer);
    read_output_frame();
}

// One connection per publishing period of hydra-native, with the frame size in its first line.
static void start_output(GSocketConnection *connection, GDataInputStream *stream, const char *size) {
    int width = 0, height = 0;
    if (!size || sscanf(size, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0 ||
        width > MAX_FRAME_SIDE || height > MAX_FRAME_SIDE) {
        close_connection(&connection);
        g_object_unref(stream);
        return;
    }
    stop_output();
    output.connection = connection;
    output.input = stream;
    output.cancellable = g_cancellable_new();
    output.frame_size = (gsize)width * height * RGBA_BYTES;
    if (output.caps) gst_caps_unref(output.caps);
    output.caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT, width,
                                      "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
    GHashTableIter iterator;
    struct peer *peer;
    g_hash_table_iter_init(&iterator, peers);
    while (g_hash_table_iter_next(&iterator, NULL, (gpointer *)&peer))
        if (peer->source) gst_app_src_set_caps(GST_APP_SRC(peer->source), output.caps);
    read_output_frame();
}

static struct peer *peer_with_channel(const char *name) {
    GHashTableIter iterator;
    struct peer *peer;
    g_hash_table_iter_init(&iterator, peers);
    while (g_hash_table_iter_next(&iterator, NULL, (gpointer *)&peer))
        if (!g_strcmp0(peer->channel_name, name)) return peer;
    return NULL;
}

static void on_channel_line(GObject *source, GAsyncResult *result, gpointer data) {
    GDataInputStream *stream = G_DATA_INPUT_STREAM(source);
    GSocketConnection *connection = data;
    char *line = g_data_input_stream_read_line_finish(stream, result, NULL, NULL);
    char *size = line ? strchr(line, ' ') : NULL;
    if (size) *size++ = '\0';
    struct peer *peer = line ? peer_with_channel(line) : NULL;
    if (line && !strcmp(line, "output")) {
        start_output(connection, stream, size);
    } else if (peer) {
        close_connection(&peer->connection);
        peer->connection = connection;
        g_object_unref(stream);
    } else {
        close_connection(&connection);
        g_object_unref(stream);
    }
    g_free(line);
}

static gboolean on_incoming(GSocketService *service, GSocketConnection *connection, GObject *listener, gpointer data) {
    GDataInputStream *stream = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_filter_input_stream_set_close_base_stream(G_FILTER_INPUT_STREAM(stream), FALSE);
    g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT, NULL, on_channel_line, g_object_ref(connection));
    return TRUE;
}

static bool listen_local(void) {
    g_unlink(socket_path);
    GSocketService *service = g_socket_service_new();
    GSocketAddress *address = g_unix_socket_address_new(socket_path);
    GError *error = NULL;
    bool listening = g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
                                                   G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error);
    g_object_unref(address);
    if (!listening) {
        fprintf(stderr, "hydra-native-patch-bay: cannot listen on %s: %s\n", socket_path, error->message);
        g_error_free(error);
        return false;
    }
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(service);
    return true;
}

// ---------------------------------------------------------------- stdin and the end

static gboolean quit(gpointer data) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

// The end of the input, SIGTERM or SIGINT: leave the room as pb._destroy() does. Closed data channels tell the peers
// at once; the pipelines end with the process, so that the close of the WebSocket gives the channels the time to send.
static void shut_down(void) {
    if (stopping) return;
    stopping = true;
    GHashTableIter iterator;
    struct peer *peer;
    g_hash_table_iter_init(&iterator, peers);
    while (g_hash_table_iter_next(&iterator, NULL, (gpointer *)&peer))
        if (peer->channel) gst_webrtc_data_channel_close(peer->channel);
    stop_output();
    if (signaller && soup_websocket_connection_get_state(signaller) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(signaller, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    else if (signaller) g_main_loop_quit(loop);
    g_timeout_add(CLOSE_WAIT_MS, quit, NULL);
}

static gboolean on_quit_signal(gpointer data) {
    shut_down();
    return G_SOURCE_CONTINUE;
}

static void call(const char *method, JsonArray *arguments) {
    JsonNode *first = arguments && json_array_get_length(arguments) ? json_array_get_element(arguments, 0) : NULL;
    if (!g_strcmp0(method, "initSource") && node_string(first)) init_source(node_string(first));
    else if (!g_strcmp0(method, "setName") && node_string(first)) set_name(node_string(first));
    else if (!g_strcmp0(method, "broadcast") && first) emit_event("broadcast", json_node_copy(first), NULL);
    else return;
    send_state();
}

static void read_input(void);

static void on_input_line(GObject *source, GAsyncResult *result, gpointer data) {
    char *line = g_data_input_stream_read_line_finish(input, result, NULL, NULL);
    if (!line) return shut_down();
    JsonNode *node = parse_json(line);
    JsonObject *message = node_object(node);
    if (!g_strcmp0(member_string(message, "type"), "call")) {
        JsonNode *arguments = json_object_get_member(message, "args");
        call(member_string(message, "method"),
             arguments && JSON_NODE_HOLDS_ARRAY(arguments) ? json_node_get_array(arguments) : NULL);
    }
    if (node) json_node_unref(node);
    g_free(line);
    read_input();
}

static void read_input(void) {
    g_data_input_stream_read_line_async(input, G_PRIORITY_DEFAULT, NULL, on_input_line, NULL);
}

static gboolean check_publish(gpointer data) {
    bool on = g_hash_table_size(peers) > 0;
    if (on == publishing) return G_SOURCE_CONTINUE;
    publishing = on;
    JsonObject *message = message_new("publish");
    json_object_set_boolean_member(message, "on", on);
    print_object(message);
    return G_SOURCE_CONTINUE;
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
    gst_init(NULL, NULL);
    if (!listen_local()) return 1;
    char *uuid = g_uuid_string_random();
    g_strlcpy(own_id, uuid, sizeof own_id);
    g_free(uuid);
    nick_from_id = json_object_new();
    id_from_nick = json_object_new();
    peer_list = json_object_new();
    peers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, peer_release);
    unsent = g_ptr_array_new_with_free_func(g_free);
    loop = g_main_loop_new(NULL, FALSE);

    JsonObject *user = json_object_new();
    json_object_set_string_member(user, "uuid", own_id);
    JsonNode *room_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(room_node, room);
    emit_event("join", room_node, object_node(user));
    if (!connect_signalling()) log_text(true, "invalid signalling server %s", server);

    GInputStream *standard_input = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    input = g_data_input_stream_new(standard_input);
    g_object_unref(standard_input);
    read_input();
    g_timeout_add(PUBLISH_CHECK_MS, check_publish, NULL);
    g_unix_signal_add(SIGTERM, on_quit_signal, NULL);
    g_unix_signal_add(SIGINT, on_quit_signal, NULL);
    g_main_loop_run(loop);
    g_unlink(socket_path);
    return 0;
}
