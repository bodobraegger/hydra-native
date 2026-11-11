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
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#define PUBLISH_CHECK_MS 500
#define ID_LENGTH 8 // pb.id, the start of a random UUID as in rtc-patch-bay.js
#define MAX_FRAME_SIDE 16384
#define RGBA_BYTES 4

static const char *server, *room, *socket_path;
static GMainLoop *loop;
static char own_id[ID_LENGTH + 1];
static char *nick, *stream_id;
// The tables of pb-live.js and rtc-patch-bay.js: JSON objects keep the order of their members, as JavaScript does.
static JsonObject *id_from_nick, *peer_list;
static bool publishing;
static GDataInputStream *input;

static struct {
    GSocketConnection *connection;
    GDataInputStream *input;
    guint8 *frame;
    gsize frame_size;
} output;

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

// ---------------------------------------------------------------- the local socket

static void close_connection(GSocketConnection **connection) {
    if (!*connection) return;
    g_socket_close(g_socket_connection_get_socket(*connection), NULL);
    g_clear_object(connection);
}

static void stop_output(void) {
    close_connection(&output.connection);
    g_clear_object(&output.input);
}

static void on_output_frame(GObject *source, GAsyncResult *result, gpointer data);

static void read_output_frame(void) {
    g_input_stream_read_all_async(G_INPUT_STREAM(output.input), output.frame, output.frame_size, G_PRIORITY_DEFAULT,
                                  NULL, on_output_frame, NULL);
}

static void on_output_frame(GObject *source, GAsyncResult *result, gpointer data) {
    gsize count = 0;
    bool read = g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &count, NULL);
    if (source != G_OBJECT(output.input)) return; // the stream of a closed output
    if (!read || count < output.frame_size) return stop_output();
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
    output.frame_size = (gsize)width * height * RGBA_BYTES;
    output.frame = g_realloc(output.frame, output.frame_size);
    read_output_frame();
}

static void on_channel_line(GObject *source, GAsyncResult *result, gpointer data) {
    GDataInputStream *stream = G_DATA_INPUT_STREAM(source);
    GSocketConnection *connection = data;
    char *line = g_data_input_stream_read_line_finish(stream, result, NULL, NULL);
    char *size = line ? strchr(line, ' ') : NULL;
    if (size) *size++ = '\0';
    if (line && !strcmp(line, "output")) {
        start_output(connection, stream, size);
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

static void call(const char *method, JsonArray *arguments) {
    if (g_strcmp0(method, "initSource") && g_strcmp0(method, "setName") && g_strcmp0(method, "broadcast")) return;
    send_state();
}

static void read_input(void);

static void on_input_line(GObject *source, GAsyncResult *result, gpointer data) {
    char *line = g_data_input_stream_read_line_finish(input, result, NULL, NULL);
    if (!line) return g_main_loop_quit(loop);
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
    bool on = false;
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
    signal(SIGPIPE, SIG_IGN); // hydra-native may close our stdout first
    if (!listen_local()) return 1;
    char *uuid = g_uuid_string_random();
    g_strlcpy(own_id, uuid, sizeof own_id);
    g_free(uuid);
    id_from_nick = json_object_new();
    peer_list = json_object_new();
    loop = g_main_loop_new(NULL, FALSE);
    GInputStream *standard_input = g_unix_input_stream_new(STDIN_FILENO, FALSE);
    input = g_data_input_stream_new(standard_input);
    g_object_unref(standard_input);
    read_input();
    g_timeout_add(PUBLISH_CHECK_MS, check_publish, NULL);
    g_main_loop_run(loop);
    g_unlink(socket_path);
    return 0;
}
