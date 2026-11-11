// The Linux backend of src/capture.h. The D-Bus calls go through GDBus (Gio): xdg-desktop-portal for the picker,
// the ScreenCast API of GNOME Mutter for monitors, areas and windows without a dialog. A Mutter session lives as
// long as the D-Bus connection of this process. gst-launch-1.0 reads the PipeWire stream and writes the frames.
#define _GNU_SOURCE
#include "capture.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#define MUTTER_BUS_NAME "org.gnome.Mutter.ScreenCast"
#define MUTTER_PATH "/org/gnome/Mutter/ScreenCast"
#define MUTTER_INTERFACE "org.gnome.Mutter.ScreenCast"
#define MUTTER_SESSION_INTERFACE "org.gnome.Mutter.ScreenCast.Session"
#define MUTTER_STREAM_INTERFACE "org.gnome.Mutter.ScreenCast.Stream"
#define DISPLAY_CONFIG_BUS_NAME "org.gnome.Mutter.DisplayConfig"
#define DISPLAY_CONFIG_PATH "/org/gnome/Mutter/DisplayConfig"
#define MUTTER_CURSOR_EMBEDDED 1u

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_INTERFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_INTERFACE "org.freedesktop.portal.Request"
#define PORTAL_SOURCES_MONITOR_AND_WINDOW (1u | 2u)
#define PORTAL_CURSOR_EMBEDDED 2u
#define PORTAL_RESPONSE_SUCCESS 0u
#define PORTAL_CLOSE_TIMEOUT_MS 1000
#define DRY_RUN_NODE 42u

static GDBusConnection *bus;
static GMainLoop *loop;
static GPid pipeline; // gst-launch-1.0, or 0
static guint pipeline_watch;
// While a portal request waits for its answer, its own loop runs and its dialog (the picker) may be open.
static GMainLoop *request_loop;
static char request_path[256];
static bool stopping;

// SIGTERM, SIGINT or the end of the program that reads our frames: close an open picker, then end every loop.
// Without the request loop, a stop during the picker did nothing, and the program waiting for us blocked.
static gboolean quit_loop(gpointer data) {
    stopping = true;
    if (bus && request_path[0]) {
        GVariant *reply = g_dbus_connection_call_sync(bus, PORTAL_BUS_NAME, request_path, PORTAL_REQUEST_INTERFACE, "Close",
                                                      NULL, NULL, G_DBUS_CALL_FLAGS_NONE, PORTAL_CLOSE_TIMEOUT_MS, NULL, NULL);
        if (reply) g_variant_unref(reply);
    }
    if (request_loop) g_main_loop_quit(request_loop);
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}

static gboolean output_closed(gint fd, GIOCondition condition, gpointer data) {
    quit_loop(NULL);
    return G_SOURCE_REMOVE;
}

static void pipeline_ended(GPid pid, gint status, gpointer data) {
    pipeline = 0;
    g_main_loop_quit(loop);
}

// gst-launch-1.0 with pipewiresrc for `node` (through the PipeWire remote `fd` of the portal, or -1 for the
// default remote) writes RGBA frames of the requested size to `output`. Both descriptors pass to the child. The
// main loop ends with the child. False when it did not start, and after a dry run.
static bool start_pipeline(int output, int fd, uint32_t node, int width, int height, bool dry_run) {
    char fd_argument[32] = "", path[32], caps[96], sink[32];
    if (fd >= 0) snprintf(fd_argument, sizeof fd_argument, "fd=%d", fd);
    snprintf(path, sizeof path, "path=%u", node);
    snprintf(caps, sizeof caps, "video/x-raw,format=RGBA,width=%d,height=%d", width, height);
    snprintf(sink, sizeof sink, "fd=%d", output);
    const char *argv[] = { "gst-launch-1.0", "-q", "pipewiresrc", path, "always-copy=true", "!", "videoconvert", "!", "videoscale", "!",
                           caps, "!", "fdsink", sink, "sync=false", NULL, NULL };
    if (fd >= 0) { // the fd goes right after pipewiresrc
        memmove(argv + 4, argv + 3, sizeof(char *) * 13); // the words from pipewiresrc on, with the NULL
        argv[3] = fd_argument;
    }
    if (dry_run) {
        printf("run");
        for (const char **word = argv; *word; word++) printf(" %s", *word);
        printf("\n");
        return false;
    }
    if (fd >= 0) fcntl(fd, F_SETFD, 0); // GDBus received it with close-on-exec
    fcntl(output, F_SETFD, 0);
    // The child stays in the process group of this program: src/video.c stops the group, and a child in a group
    // of its own would live on and keep the frame pipe open when this program dies first.
    GError *error = NULL;
    GSpawnFlags flags = G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_DO_NOT_REAP_CHILD |
                        G_SPAWN_CHILD_INHERITS_STDIN;
    if (!g_spawn_async(NULL, (char **)argv, NULL, flags, NULL, NULL, &pipeline, &error)) {
        fprintf(stderr, "capture: cannot start gst-launch-1.0: %s\n", error->message);
        g_error_free(error);
        pipeline = 0;
        return false;
    }
    pipeline_watch = g_child_watch_add(pipeline, pipeline_ended, NULL);
    return true;
}

static void stop_pipeline(void) {
    if (!pipeline) return;
    g_source_remove(pipeline_watch);
    kill(pipeline, SIGTERM);
    waitpid(pipeline, NULL, 0);
    pipeline = 0;
}

static GVariant *call(const char *name, const char *path, const char *interface, const char *method, GVariant *arguments,
                      const char *reply_type) {
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(bus, name, path, interface, method, arguments,
                                                  reply_type ? G_VARIANT_TYPE(reply_type) : NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                  &error);
    if (!reply) {
        fprintf(stderr, "capture: %s: %s\n", method, error->message);
        g_error_free(error);
    }
    return reply;
}

// ---------------------------------------------------------------- Mutter ScreenCast (no dialog)

static struct {
    int output, width, height;
} mutter;

static void mutter_stream_added(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                                const char *signal, GVariant *parameters, gpointer data) {
    uint32_t node;
    g_variant_get(parameters, "(u)", &node);
    fprintf(stderr, "capture: pipewire node %u\n", node);
    if (!pipeline && !start_pipeline(mutter.output, -1, node, mutter.width, mutter.height, false))
        g_main_loop_quit(loop);
}

static void mutter_closed(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                          const char *signal, GVariant *parameters, gpointer data) {
    g_main_loop_quit(loop);
}

// GetCurrentState of DisplayConfig; NULL without such a monitor.
static char *monitor_connector(int index) {
    GVariant *state = call(DISPLAY_CONFIG_BUS_NAME, DISPLAY_CONFIG_PATH, "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL, NULL);
    if (!state) return NULL;
    GVariant *monitors = g_variant_get_child_value(state, 1);
    char *connector = NULL;
    if (index >= 0 && (size_t)index < g_variant_n_children(monitors)) {
        GVariant *monitor = g_variant_get_child_value(monitors, (size_t)index);
        GVariant *spec = g_variant_get_child_value(monitor, 0);
        g_variant_get_child(spec, 0, "s", &connector);
        g_variant_unref(spec);
        g_variant_unref(monitor);
    } else {
        fprintf(stderr, "capture: no monitor %d, there are %zu\n", index, g_variant_n_children(monitors));
    }
    g_variant_unref(monitors);
    g_variant_unref(state);
    return connector;
}

static GVariant *mutter_properties(void) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "cursor-mode", g_variant_new_uint32(MUTTER_CURSOR_EMBEDDED));
    return g_variant_builder_end(&builder);
}

static int run_mutter(const struct capture_request *request, int output) {
    mutter.output = output;
    mutter.width = request->width;
    mutter.height = request->height;
    GVariant *reply = call(MUTTER_BUS_NAME, MUTTER_PATH, MUTTER_INTERFACE, "CreateSession", g_variant_new("(a{sv})", NULL), "(o)");
    if (!reply) return 1;
    char *session;
    g_variant_get(reply, "(o)", &session);
    g_variant_unref(reply);
    reply = NULL;
    if (request->source == CAPTURE_MONITOR) {
        char *connector = monitor_connector(request->monitor);
        if (connector)
            reply = call(MUTTER_BUS_NAME, session, MUTTER_SESSION_INTERFACE, "RecordMonitor",
                         g_variant_new("(s@a{sv})", connector, mutter_properties()), "(o)");
        g_free(connector);
    } else if (request->source == CAPTURE_AREA) {
        reply = call(MUTTER_BUS_NAME, session, MUTTER_SESSION_INTERFACE, "RecordArea",
                     g_variant_new("(iiii@a{sv})", request->x, request->y, request->area_width, request->area_height, mutter_properties()),
                     "(o)");
    } else {
        g_usleep((gulong)(request->delay_seconds * G_USEC_PER_SEC));
        reply = call(MUTTER_BUS_NAME, session, MUTTER_SESSION_INTERFACE, "RecordWindow", g_variant_new("(@a{sv})", mutter_properties()),
                     "(o)");
    }
    if (!reply) {
        g_free(session);
        return 1;
    }
    char *stream;
    g_variant_get(reply, "(o)", &stream);
    g_variant_unref(reply);
    g_dbus_connection_signal_subscribe(bus, MUTTER_BUS_NAME, MUTTER_STREAM_INTERFACE, "PipeWireStreamAdded", stream, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, mutter_stream_added, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, MUTTER_BUS_NAME, MUTTER_SESSION_INTERFACE, "Closed", session, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, mutter_closed, NULL, NULL);
    reply = call(MUTTER_BUS_NAME, session, MUTTER_SESSION_INTERFACE, "Start", NULL, NULL);
    if (reply) {
        g_variant_unref(reply);
        g_main_loop_run(loop);
    }
    stop_pipeline();
    GVariant *stopped = call(MUTTER_BUS_NAME, session, MUTTER_SESSION_INTERFACE, "Stop", NULL, NULL);
    if (stopped) g_variant_unref(stopped);
    g_free(stream);
    g_free(session);
    return reply ? 0 : 1;
}

// ---------------------------------------------------------------- xdg-desktop-portal (the picker)

struct portal_answer {
    GMainLoop *loop;
    uint32_t response;
    GVariant *results;
};

static void portal_response(GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                            const char *signal, GVariant *parameters, gpointer data) {
    struct portal_answer *answer = data;
    g_variant_get(parameters, "(u@a{sv})", &answer->response, &answer->results);
    g_main_loop_quit(answer->loop);
}

// A portal method that answers through the Response signal of its Request object. `options` is the last argument
// (a{sv}, with the handle token added), after the session handle if there is one.
static uint32_t portal_request(const char *method, const char *session, GVariantBuilder *options, bool dry_run, GVariant **results) {
    static unsigned counter;
    char token[64], path[256];
    snprintf(token, sizeof token, "hydra_native_%d_%u", (int)getpid(), ++counter);
    g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token));
    GVariant *arguments = !strcmp(method, "CreateSession") ? g_variant_new("(a{sv})", options)
                          : !strcmp(method, "Start")       ? g_variant_new("(osa{sv})", session, "", options)
                                                           : g_variant_new("(oa{sv})", session, options);
    g_variant_ref_sink(arguments);
    if (dry_run) {
        char *text = g_variant_print(arguments, FALSE);
        printf("call %s %s\n", method, text);
        g_free(text);
        g_variant_unref(arguments);
        GVariantBuilder answer;
        g_variant_builder_init(&answer, G_VARIANT_TYPE("a{sv}"));
        if (!strcmp(method, "CreateSession"))
            g_variant_builder_add(&answer, "{sv}", "session_handle", g_variant_new_string(PORTAL_PATH "/session/dry/run"));
        if (!strcmp(method, "Start")) {
            GVariantBuilder properties;
            g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&properties, "{sv}", "source_type", g_variant_new_uint32(2));
            GVariant *stream = g_variant_new("(u@a{sv})", DRY_RUN_NODE, g_variant_builder_end(&properties));
            g_variant_builder_add(&answer, "{sv}", "streams", g_variant_new_array(NULL, &stream, 1));
        }
        *results = g_variant_ref_sink(g_variant_builder_end(&answer));
        return PORTAL_RESPONSE_SUCCESS;
    }
    char *sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
    for (char *c = sender; *c; c++)
        if (*c == '.') *c = '_';
    snprintf(path, sizeof path, PORTAL_PATH "/request/%s/%s", sender, token);
    g_free(sender);
    // Subscribe before the call, so that a fast response does not get lost.
    struct portal_answer answer = { g_main_loop_new(NULL, FALSE), 1, NULL };
    guint subscription = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS_NAME, PORTAL_REQUEST_INTERFACE, "Response", path, NULL,
                                                            G_DBUS_SIGNAL_FLAGS_NONE, portal_response, &answer, NULL);
    GVariant *reply = stopping ? NULL : call(PORTAL_BUS_NAME, PORTAL_PATH, PORTAL_SCREENCAST_INTERFACE, method, arguments, "(o)");
    g_variant_unref(arguments);
    if (reply) {
        g_variant_unref(reply);
        snprintf(request_path, sizeof request_path, "%s", path);
        request_loop = answer.loop;
        if (!stopping) g_main_loop_run(answer.loop);
        request_loop = NULL;
        request_path[0] = '\0';
    }
    g_dbus_connection_signal_unsubscribe(bus, subscription);
    g_main_loop_unref(answer.loop);
    *results = answer.results;
    return answer.results ? answer.response : 1;
}

static int open_pipewire_remote(const char *session, bool dry_run) {
    if (dry_run) {
        printf("call OpenPipeWireRemote %s\n", session);
        return open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    GUnixFDList *fds = NULL;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(bus, PORTAL_BUS_NAME, PORTAL_PATH, PORTAL_SCREENCAST_INTERFACE,
                                                                     "OpenPipeWireRemote", g_variant_new("(oa{sv})", session, NULL),
                                                                     G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds,
                                                                     NULL, &error);
    if (!reply) {
        fprintf(stderr, "capture: OpenPipeWireRemote: %s\n", error->message);
        g_error_free(error);
        return -1;
    }
    int handle;
    g_variant_get(reply, "(h)", &handle);
    int fd = g_unix_fd_list_get(fds, handle, NULL);
    g_variant_unref(reply);
    g_object_unref(fds);
    return fd;
}

static GVariantBuilder *new_options(void) { return g_variant_builder_new(G_VARIANT_TYPE("a{sv}")); }

// CreateSession, SelectSources (monitor or window), Start, OpenPipeWireRemote, then pipewiresrc on the fd.
static int run_portal(const struct capture_request *request, int output) {
    GVariant *results = NULL;
    GVariantBuilder *options = new_options();
    char token[64];
    snprintf(token, sizeof token, "hydra_native_session_%d", (int)getpid());
    g_variant_builder_add(options, "{sv}", "session_handle_token", g_variant_new_string(token));
    uint32_t response = portal_request("CreateSession", NULL, options, request->dry_run, &results);
    g_variant_builder_unref(options);
    char *session = NULL;
    if (response != PORTAL_RESPONSE_SUCCESS || !g_variant_lookup(results, "session_handle", "s", &session)) {
        fprintf(stderr, "capture: the portal refused the session\n");
        return 1;
    }
    g_variant_unref(results);
    options = new_options();
    g_variant_builder_add(options, "{sv}", "types", g_variant_new_uint32(PORTAL_SOURCES_MONITOR_AND_WINDOW));
    g_variant_builder_add(options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(options, "{sv}", "cursor_mode", g_variant_new_uint32(PORTAL_CURSOR_EMBEDDED));
    response = portal_request("SelectSources", session, options, request->dry_run, &results);
    g_variant_builder_unref(options);
    if (results) g_variant_unref(results);
    if (response != PORTAL_RESPONSE_SUCCESS) {
        fprintf(stderr, "capture: no source selected\n");
        g_free(session);
        return 1;
    }
    options = new_options();
    response = portal_request("Start", session, options, request->dry_run, &results);
    g_variant_builder_unref(options);
    GVariant *streams = results ? g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})")) : NULL;
    if (response != PORTAL_RESPONSE_SUCCESS || !streams || g_variant_n_children(streams) == 0) {
        fprintf(stderr, "capture: the picker was closed\n");
        g_free(session);
        return 1;
    }
    uint32_t node;
    g_variant_get_child(streams, 0, "(u@a{sv})", &node, NULL);
    g_variant_unref(streams);
    g_variant_unref(results);
    int fd = open_pipewire_remote(session, request->dry_run);
    g_free(session);
    if (fd < 0) return 1;
    bool started = start_pipeline(output, fd, node, request->width, request->height, request->dry_run);
    if (request->dry_run) return 0;
    if (!started) return 1;
    g_main_loop_run(loop);
    stop_pipeline();
    close(fd);
    return 0;
}

int capture_run(const struct capture_request *request, int output) {
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, quit_loop, NULL);
    g_unix_signal_add(SIGINT, quit_loop, NULL);
    // Also when the reader dies without a signal to us, for example after a second Ctrl+C.
    g_unix_fd_add(output, G_IO_HUP | G_IO_ERR, output_closed, NULL);
    if (!request->dry_run) {
        GError *error = NULL;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
        if (!bus) {
            fprintf(stderr, "capture: no session bus: %s\n", error->message);
            g_error_free(error);
            return 1;
        }
    }
    int status = request->source == CAPTURE_PICKER ? run_portal(request, output) : run_mutter(request, output);
    if (bus) g_object_unref(bus);
    g_main_loop_unref(loop);
    return status;
}
