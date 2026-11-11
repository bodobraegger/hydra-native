#include "bindings.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include "app.h"
#include "engine.h"
#include "midi.h"
#include "native.h"
#include "process.h"
#include "sandbox.h"
#include "snapshot.h"
#include "ui.h"

#define MAX_COMMAND 8192
#define MAX_CAPTURE_NUMBERS 8 // the longest usage, of "area", has six

static int to_int(JSContext *ctx, JSValueConst value) {
    int32_t result = 0;
    JS_ToInt32(ctx, &result, value);
    return result;
}

static float to_float(JSContext *ctx, JSValueConst value) {
    double result = 0;
    JS_ToFloat64(ctx, &result, value);
    return (float)result;
}

// ---------------------------------------------------------------- GL bindings

// createProgram(vertex, fragment) starts to compile and link, and returns the program before it is ready.
static JSValue js_create_program(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *vertex_source = JS_ToCString(ctx, argv[0]);
    const char *fragment_source = JS_ToCString(ctx, argv[1]);
    JSValue result = vertex_source && fragment_source
                         ? JS_NewInt32(ctx, (int32_t)program_create(vertex_source, fragment_source)) : JS_EXCEPTION;
    JS_FreeCString(ctx, vertex_source);
    JS_FreeCString(ctx, fragment_source);
    return result;
}

// programStatus(program, wait): true when the program is ready, false while it compiles and `wait` is false.
// A failed program throws its log as a SyntaxError.
static JSValue js_program_status(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    char *error;
    switch (program_status((GLuint)to_int(ctx, argv[0]), JS_ToBool(ctx, argv[1]), &error)) {
    case PROGRAM_COMPILING: return JS_FALSE;
    case PROGRAM_LINKED: return JS_TRUE;
    default: break;
    }
    JSValue exception = JS_ThrowSyntaxError(ctx, "%s", error);
    free(error);
    return exception;
}

static JSValue js_uniform_location(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[1]);
    GLint location = glGetUniformLocation((GLuint)to_int(ctx, argv[0]), name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, location);
}

static JSValue js_use_program(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    glUseProgram((GLuint)to_int(ctx, argv[0]));
    return JS_UNDEFINED;
}

// uniform(location, x[, y[, z[, w]]]): the argument count picks glUniform1f to glUniform4f.
static JSValue js_uniform(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    GLint location = to_int(ctx, argv[0]);
    float v[4] = { 0 };
    for (int i = 1; i < argc && i <= 4; i++) v[i - 1] = to_float(ctx, argv[i]);
    switch (argc - 1) {
    case 1: glUniform1f(location, v[0]); break;
    case 2: glUniform2f(location, v[0], v[1]); break;
    case 3: glUniform3f(location, v[0], v[1], v[2]); break;
    default: glUniform4f(location, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue js_uniform_texture(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    int unit = to_int(ctx, argv[1]);
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, (GLuint)to_int(ctx, argv[2]));
    glUniform1i(to_int(ctx, argv[0]), unit);
    return JS_UNDEFINED;
}

static GLint wrap_mode(int code) {
    static const GLint modes[] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT };
    return code >= 0 && code < 3 ? modes[code] : GL_CLAMP_TO_EDGE;
}

// createTexture(width, height, magLinear, minLinear, wrapS, wrapT) with wrap codes 0 clamp, 1 repeat, 2 mirror.
static JSValue js_create_texture(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, JS_ToBool(ctx, argv[2]) ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, JS_ToBool(ctx, argv[3]) ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(to_int(ctx, argv[4])));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(to_int(ctx, argv[5])));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, to_int(ctx, argv[0]), to_int(ctx, argv[1]), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
    return JS_NewInt32(ctx, (int32_t)texture);
}

static JSValue js_resize_texture(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    glBindTexture(GL_TEXTURE_2D, (GLuint)to_int(ctx, argv[0]));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, to_int(ctx, argv[1]), to_int(ctx, argv[2]), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
    return JS_UNDEFINED;
}

static JSValue js_delete_texture(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    GLuint texture = (GLuint)to_int(ctx, argv[0]);
    glDeleteTextures(1, &texture);
    return JS_UNDEFINED;
}

static JSValue js_create_framebuffer(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)to_int(ctx, argv[0]), 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) return JS_ThrowInternalError(ctx, "framebuffer incomplete: 0x%x", status);
    return JS_NewInt32(ctx, (int32_t)framebuffer);
}

static JSValue js_bind_framebuffer(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)to_int(ctx, argv[0]));
    glViewport(0, 0, to_int(ctx, argv[1]), to_int(ctx, argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_draw_triangle(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return JS_UNDEFINED;
}

static JSValue js_clear(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    glClearColor(to_float(ctx, argv[0]), to_float(ctx, argv[1]), to_float(ctx, argv[2]), to_float(ctx, argv[3]));
    glClear(GL_COLOR_BUFFER_BIT);
    return JS_UNDEFINED;
}

// fillRects([x, y, width, height, r, g, b, a, ...]) in window pixels from the top left, for overlays.
static JSValue js_fill_rects(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    int drawable_height;
    SDL_GetWindowSizeInPixels(window, NULL, &drawable_height);
    JSValue length_value = JS_GetPropertyStr(ctx, argv[0], "length");
    int64_t length = to_int(ctx, length_value);
    JS_FreeValue(ctx, length_value);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
    for (int64_t i = 0; i + 8 <= length; i += 8) {
        float v[8];
        for (int k = 0; k < 8; k++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)(i + k));
            v[k] = to_float(ctx, item);
            JS_FreeValue(ctx, item);
        }
        glScissor((GLint)v[0], drawable_height - (GLint)(v[1] + v[3]), (GLsizei)v[2], (GLsizei)v[3]);
        glClearColor(v[4], v[5], v[6], v[7]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    return JS_UNDEFINED;
}

static JSValue js_read_pixel(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    unsigned char pixel[4] = { 0 };
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)to_int(ctx, argv[0]));
    glReadPixels(to_int(ctx, argv[1]), to_int(ctx, argv[2]), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 4; i++) JS_SetPropertyUint32(ctx, array, i, JS_NewInt32(ctx, pixel[i]));
    return array;
}

// ---------------------------------------------------------------- media and system bindings

static JSValue js_sandbox(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    sandbox_enter();
    return JS_UNDEFINED;
}

static JSValue video_handle(JSContext *ctx, int handle, const char *error) {
    return handle < 0 ? JS_ThrowInternalError(ctx, "%s", error) : JS_NewInt32(ctx, handle);
}

// videoOpen(command, width, height): initCommand, a shell command that writes frames.
static JSValue js_video_open(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    if (sandbox_active()) return JS_ThrowTypeError(ctx, "initCommand is off: " SANDBOX_REASON);
    const char *command = JS_ToCString(ctx, argv[0]);
    int handle = video_open(command, to_int(ctx, argv[1]), to_int(ctx, argv[2]));
    JS_FreeCString(ctx, command);
    return video_handle(ctx, handle, "cannot start video source");
}

// mediaOpen(url, width, height, kind): initVideo ("video", "youtube") and initImage ("image"), see
// sandbox_media_command. In the sandbox, only HTTP and HTTPS URLs work.
static JSValue js_media_open(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]), *kind = JS_ToCString(ctx, argv[3]);
    int width = to_int(ctx, argv[1]), height = to_int(ctx, argv[2]);
    char command[MAX_COMMAND];
    JSValue result = JS_EXCEPTION;
    if (url && kind) {
        switch (sandbox_media_command(command, sizeof command, url, kind, width, height)) {
        case COMMAND_OK:
            result = video_handle(ctx, video_open(command, width, height), "cannot start video source");
            break;
        case COMMAND_SANDBOXED: result = JS_ThrowTypeError(ctx, "local file %s is off: " SANDBOX_REASON, url); break;
        default: result = JS_ThrowTypeError(ctx, "cannot use %s as a media URL", url); break;
        }
    }
    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, kind);
    return result;
}

// captureOpen(mode, ...numbers, width, height): the screen capture as a video source, see
// sandbox_capture_command.
static JSValue js_capture_open(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *mode = JS_ToCString(ctx, argv[0]);
    if (!mode) return JS_EXCEPTION;
    int numbers[MAX_CAPTURE_NUMBERS], count = 0;
    for (int i = 1; i < argc && count < MAX_CAPTURE_NUMBERS; i++) numbers[count++] = to_int(ctx, argv[i]);
    char command[MAX_COMMAND];
    enum command_status status = argc < 3 ? COMMAND_INVALID
                                          : sandbox_capture_command(command, sizeof command, executable_path_directory,
                                                                    mode, numbers, count);
    JSValue result;
    if (status == COMMAND_INVALID) {
        result = JS_ThrowTypeError(ctx, "unknown capture %s", mode);
    } else {
        int handle = status == COMMAND_OK ? video_open(command, numbers[count - 2], numbers[count - 1]) : -1;
        result = video_handle(ctx, handle, "cannot start the screen capture");
    }
    JS_FreeCString(ctx, mode);
    return result;
}

// textWidth(family, bold, italic, line)
static JSValue js_text_width(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *family = JS_ToCString(ctx, argv[0]), *line = JS_ToCString(ctx, argv[3]);
    double width = family && line ? text_width(family, JS_ToBool(ctx, argv[1]), JS_ToBool(ctx, argv[2]), line) : 0;
    JS_FreeCString(ctx, family);
    JS_FreeCString(ctx, line);
    return JS_NewFloat64(ctx, width);
}

// textTexture(texture, text, family, bold, italic, width, height, size, x, align, fill, stroke, lineWidth, passes):
// the arguments are the fields of struct text_options.
static JSValue js_text_texture(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *text = JS_ToCString(ctx, argv[1]), *family = JS_ToCString(ctx, argv[2]);
    struct text_options options = {
        .text = text, .family = family, .bold = JS_ToBool(ctx, argv[3]), .italic = JS_ToBool(ctx, argv[4]),
        .width = to_int(ctx, argv[5]), .height = to_int(ctx, argv[6]), .size = to_float(ctx, argv[7]),
        .x = to_float(ctx, argv[8]), .align = to_int(ctx, argv[9]), .line_width = to_float(ctx, argv[12]),
        .passes = to_int(ctx, argv[13]),
    };
    JS_ToUint32(ctx, &options.fill, argv[10]);
    JS_ToUint32(ctx, &options.stroke, argv[11]);
    bool ok = text && family && text_draw((GLuint)to_int(ctx, argv[0]), &options);
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, family);
    return JS_NewBool(ctx, ok);
}

static JSValue js_video_upload(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, video_upload(to_int(ctx, argv[0]), (GLuint)to_int(ctx, argv[1])));
}

static JSValue js_video_running(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, video_is_running(to_int(ctx, argv[0])));
}

static JSValue js_video_close(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    video_close(to_int(ctx, argv[0]));
    return JS_UNDEFINED;
}

#define MAX_CAMERAS 64

// cameraOpen(index, width, height): a video source of camera `index` of cameras().
static JSValue js_camera_open(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    int handle = video_open_camera(to_int(ctx, argv[0]), to_int(ctx, argv[1]), to_int(ctx, argv[2]));
    if (handle < 0) return JS_ThrowInternalError(ctx, "cannot open the camera");
    return JS_NewInt32(ctx, handle);
}

// cameras(): the names of the cameras (the /dev/video* nodes on Linux), in the order of initCam(index).
static JSValue js_cameras(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *names[MAX_CAMERAS];
    int count = video_camera_names(names, MAX_CAMERAS);
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) JS_SetPropertyUint32(ctx, array, (uint32_t)i, JS_NewString(ctx, names[i]));
    return array;
}

static JSValue js_find_program(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    bool found = name && process_find_program(name);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, found);
}

static const char *audio_device;

static JSValue js_audio_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, audio_start(audio_device, executable_path_directory));
}

// Returns the shape of Meyda's analyzer.get(): { loudness: { specific, total } }, or null before the first buffer.
static JSValue js_audio_features(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    float specific[BARK_BAND_COUNT], total;
    if (!audio_features(specific, &total)) return JS_NULL;
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < BARK_BAND_COUNT; i++) JS_SetPropertyUint32(ctx, array, i, JS_NewFloat64(ctx, specific[i]));
    JSValue loudness = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loudness, "specific", array);
    JS_SetPropertyStr(ctx, loudness, "total", JS_NewFloat64(ctx, total));
    JSValue features = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, features, "loudness", loudness);
    return features;
}

static JSValue js_fetch_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    int id = fetch_start(url);
    JS_FreeCString(ctx, url);
    if (id < 0) return JS_ThrowInternalError(ctx, "cannot start curl");
    return JS_NewInt32(ctx, id);
}

// httpRequest(url, method, ["Name: value", ...], body): body is null, a string, an ArrayBuffer or a typed array.
static JSValue js_http_request(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]), *method = JS_ToCString(ctx, argv[1]);
    int64_t header_count = 0;
    JSValue length = JS_GetPropertyStr(ctx, argv[2], "length");
    JS_ToInt64(ctx, &header_count, length);
    JS_FreeValue(ctx, length);
    if (header_count < 0) header_count = 0;
    const char **headers = calloc((size_t)header_count + 1, sizeof *headers);
    for (int64_t i = 0; i < header_count; i++) {
        JSValue header = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
        headers[i] = JS_ToCString(ctx, header);
        JS_FreeValue(ctx, header);
    }
    const char *text = NULL;
    const void *body = NULL;
    size_t body_length = 0;
    if (JS_IsString(argv[3])) body = text = JS_ToCStringLen(ctx, &body_length, argv[3]);
    else if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
        body = JS_GetArrayBuffer(ctx, &body_length, argv[3]);
        if (!body) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            size_t offset, element_size;
            JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[3], &offset, &body_length, &element_size);
            if (!JS_IsException(buffer)) {
                size_t buffer_length;
                const uint8_t *bytes = JS_GetArrayBuffer(ctx, &buffer_length, buffer);
                body = bytes ? bytes + offset : NULL;
                JS_FreeValue(ctx, buffer); // the typed array keeps the buffer alive
            }
        }
    }
    int id = url && method ? http_start(url, method, headers, (size_t)header_count, body, body_length) : -1;
    for (int64_t i = 0; i < header_count; i++) JS_FreeCString(ctx, headers[i]);
    free(headers);
    if (text) JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, method);
    if (id < 0) return JS_ThrowInternalError(ctx, "cannot start curl");
    return JS_NewInt32(ctx, id);
}

// ---------------------------------------------------------------- webRTC patch bay (tools/patch-bay.mjs)

static JSValue js_patch_bay_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *server = JS_ToCString(ctx, argv[0]), *room = JS_ToCString(ctx, argv[1]);
    char script[PATH_MAX + 64];
    snprintf(script, sizeof script, "%s/../tools/patch-bay.mjs", executable_path_directory);
    bool ok = server && room && patch_bay_start(script, server, room);
    JS_FreeCString(ctx, server);
    JS_FreeCString(ctx, room);
    return JS_NewBool(ctx, ok);
}

static JSValue js_patch_bay_stop(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    patch_bay_stop();
    return JS_UNDEFINED;
}

// setGalleryServer(url or null): the server of sharing and of the patch bay changes while the program runs.
static JSValue js_set_gallery_server(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *url = JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
    share_set_server(url ? url : "");
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

static JSValue js_patch_bay_send(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *line = JS_ToCString(ctx, argv[0]);
    bool ok = line && patch_bay_send(line);
    JS_FreeCString(ctx, line);
    return JS_NewBool(ctx, ok);
}

// patchBayPublish(on): the output frames go to the helper (at the size of the drawable, even) while `on`.
static JSValue js_patch_bay_publish(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    return JS_NewBool(ctx, patch_bay_publish(JS_ToBool(ctx, argv[0]), width & ~1, height & ~1));
}

// patchBayVideo(channel, width, height): a video source for the frames of a stream that the helper receives.
static JSValue js_patch_bay_video(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *channel = JS_ToCString(ctx, argv[0]);
    int stream = channel ? patch_bay_connect(channel) : -1;
    JS_FreeCString(ctx, channel);
    int handle = stream < 0 ? -1 : video_open_stream(stream, to_int(ctx, argv[1]), to_int(ctx, argv[2]));
    if (handle < 0) return JS_ThrowInternalError(ctx, "cannot receive the stream");
    return JS_NewInt32(ctx, handle);
}

static JSValue js_share_done(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *text = JS_ToCString(ctx, argv[1]);
    share_done(JS_ToBool(ctx, argv[0]), text ? text : "");
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- MIDI

#define MAX_MIDI_PORTS 64

static JSValue js_midi_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, midi_start());
}

static JSValue midi_port_list(JSContext *ctx, int (*list)(struct midi_port *ports, int capacity)) {
    struct midi_port ports[MAX_MIDI_PORTS];
    int count = list(ports, MAX_MIDI_PORTS);
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        char id[32];
        snprintf(id, sizeof id, "%d:%d", ports[i].client, ports[i].port);
        JSValue entry = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, entry, 0, JS_NewString(ctx, id));
        JS_SetPropertyUint32(ctx, entry, 1, JS_NewString(ctx, ports[i].name));
        JS_SetPropertyUint32(ctx, entry, 2, JS_NewString(ctx, ports[i].manufacturer));
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, entry);
    }
    return array;
}

// midiInputs() and midiOutputs() return [[id, name, manufacturer], ...] with id "client:port".
static JSValue js_midi_inputs(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return midi_port_list(ctx, midi_inputs);
}

static JSValue js_midi_outputs(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return midi_port_list(ctx, midi_outputs);
}

// midiSend(id, [bytes]) sends one or more MIDI messages to the port "client:port".
static JSValue js_midi_send(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *id = JS_ToCString(ctx, argv[0]);
    int client = -1, port = -1;
    if (id) sscanf(id, "%d:%d", &client, &port);
    JS_FreeCString(ctx, id);
    JSValue length_value = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    unsigned char bytes[1024];
    if (length < 0 || length > (int32_t)sizeof bytes) return JS_ThrowRangeError(ctx, "MIDI data too long");
    for (int32_t i = 0; i < length; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        bytes[i] = (unsigned char)to_int(ctx, item);
        JS_FreeValue(ctx, item);
    }
    if (client < 0 || !midi_send(client, port, bytes, length)) return JS_ThrowInternalError(ctx, "cannot send MIDI");
    return JS_UNDEFINED;
}

static JSValue midi_messages;

static void add_midi_message(int client, int port, double time, const unsigned char *bytes, int length) {
    char id[32];
    snprintf(id, sizeof id, "%d:%d", client, port);
    JSValue entry = JS_NewArray(js);
    JS_SetPropertyUint32(js, entry, 0, JS_NewString(js, id));
    JS_SetPropertyUint32(js, entry, 1, JS_NewFloat64(js, time));
    for (int i = 0; i < length; i++) JS_SetPropertyUint32(js, entry, (uint32_t)i + 2, JS_NewInt32(js, bytes[i]));
    JSValue length_value = JS_GetPropertyStr(js, midi_messages, "length");
    int32_t index = 0;
    JS_ToInt32(js, &index, length_value);
    JS_FreeValue(js, length_value);
    JS_SetPropertyUint32(js, midi_messages, (uint32_t)index, entry);
}

// midiPoll() returns { changed, messages: [[id, time, byte, ...], ...] } since the last call.
static JSValue js_midi_poll(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    midi_messages = JS_NewArray(ctx);
    midi_poll(now_ms(), add_midi_message);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, midi_inputs_changed()));
    JS_SetPropertyStr(ctx, result, "messages", midi_messages);
    return result;
}

// ---------------------------------------------------------------- files, console and program

// fetchSync(url) waits for curl; only for loading a sketch by URL, as the start waits for it anyway.
static JSValue js_fetch_sync(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    const char *argv_curl[] = { "curl", "-fsSL", CURL_PROTOCOL_OPTIONS, "--max-time", "15", "--", url, NULL };
    SDL_Process *process = SDL_CreateProcess(argv_curl, true);
    JS_FreeCString(ctx, url);
    if (!process) return JS_ThrowInternalError(ctx, "cannot start curl: %s", SDL_GetError());
    size_t length = 0;
    int status = -1;
    char *data = SDL_ReadProcess(process, &length, &status);
    SDL_DestroyProcess(process);
    JSValue result = data && status == 0 ? JS_NewStringLen(ctx, data, length)
                                         : JS_ThrowInternalError(ctx, "the request failed");
    SDL_free(data);
    return result;
}

static JSValue js_environment(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = name ? getenv(name) : NULL;
    JS_FreeCString(ctx, name);
    return value ? JS_NewString(ctx, value) : JS_NULL;
}

// writeFile(path, text) creates missing parent directories, like mkdir -p.
static JSValue js_write_file(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *path = JS_ToCString(ctx, argv[0]);
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, argv[1]);
    bool ok = false;
    if (path && text && sandbox_allows_path(path)) {
        char directory[PATH_MAX];
        snprintf(directory, sizeof directory, "%s", path);
        for (char *slash = strchr(directory + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
            *slash = '\0';
            mkdir(directory, 0755);
            *slash = '/';
        }
        FILE *file = fopen(path, "wb");
        ok = file && fwrite(text, 1, length, file) == length;
        if (file && fclose(file) != 0) ok = false;
    }
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

static JSValue js_now(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, now_ms());
}

// readFile(path, binary): the text of a file, or its bytes as an ArrayBuffer when `binary` is true.
static JSValue js_read_file(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    if (!sandbox_allows_path(path)) {
        JSValue error = JS_ThrowReferenceError(ctx, "cannot read %s: " SANDBOX_REASON, path);
        JS_FreeCString(ctx, path);
        return error;
    }
    size_t length;
    char *text = read_file(path, &length);
    bool binary = argc > 1 && JS_ToBool(ctx, argv[1]);
    JSValue result = !text ? JS_ThrowReferenceError(ctx, "cannot read %s: %s", path, strerror(errno))
                     : binary ? JS_NewArrayBufferCopy(ctx, (const uint8_t *)text, length)
                              : JS_NewStringLen(ctx, text, length);
    free(text);
    JS_FreeCString(ctx, path);
    return result;
}

static JSValue js_drawable_size(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    JSValue array = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, array, 0, JS_NewInt32(ctx, width));
    JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx, height));
    return array;
}

// console.warn and console.error (js/dom-shim.js) also report here, so that the overlay can show shader errors.
static JSValue js_console_message(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *text = JS_ToCString(ctx, argv[0]);
    if (text) {
        fprintf(stderr, "%s\n", text);
        ui_console_message(text);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

// showError(text): an error that comes after its evaluation, for example a shader that the driver compiled later.
static JSValue js_show_error(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *text = JS_ToCString(ctx, argv[0]);
    if (text) {
        fprintf(stderr, "%s\n", text);
        ui_show_message(text, true);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

static JSValue js_exit(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    app_quit(argc > 0 ? to_int(ctx, argv[0]) : 0);
    return JS_UNDEFINED;
}

static JSValue js_screencap(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    snapshot_request_screencap();
    return JS_UNDEFINED;
}

static char recording_path[PATH_MAX + 80];

static JSValue js_record_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    char base[PATH_MAX + 64];
    snapshot_download_base(base, sizeof base);
    snprintf(recording_path, sizeof recording_path, "%s.webm", base);
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (!process_find_program("ffmpeg")) return JS_ThrowInternalError(ctx, "vidRecorder needs ffmpeg in PATH");
    if (!recorder_start(recording_path, width & ~1, height & ~1)) return JS_ThrowInternalError(ctx, "cannot record");
    return JS_UNDEFINED;
}

static JSValue js_record_stop(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    if (!recorder_active()) return JS_UNDEFINED;
    bool ok = recorder_stop();
    char message[PATH_MAX + 128];
    snprintf(message, sizeof message, ok ? "saved %s" : "cannot save %s (see the ffmpeg output)", recording_path);
    ui_show_message(message, !ok);
    return JS_NewString(ctx, recording_path);
}

static const JSCFunctionListEntry native_functions[] = {
    JS_CFUNC_DEF("createProgram", 2, js_create_program),
    JS_CFUNC_DEF("programStatus", 2, js_program_status),
    JS_CFUNC_DEF("uniformLocation", 2, js_uniform_location),
    JS_CFUNC_DEF("useProgram", 1, js_use_program),
    JS_CFUNC_DEF("uniform", 5, js_uniform),
    JS_CFUNC_DEF("uniformTexture", 3, js_uniform_texture),
    JS_CFUNC_DEF("createTexture", 6, js_create_texture),
    JS_CFUNC_DEF("resizeTexture", 3, js_resize_texture),
    JS_CFUNC_DEF("deleteTexture", 1, js_delete_texture),
    JS_CFUNC_DEF("createFramebuffer", 1, js_create_framebuffer),
    JS_CFUNC_DEF("bindFramebuffer", 3, js_bind_framebuffer),
    JS_CFUNC_DEF("drawTriangle", 0, js_draw_triangle),
    JS_CFUNC_DEF("clear", 4, js_clear),
    JS_CFUNC_DEF("fillRects", 1, js_fill_rects),
    JS_CFUNC_DEF("readPixel", 3, js_read_pixel),
    JS_CFUNC_DEF("textWidth", 4, js_text_width),
    JS_CFUNC_DEF("textTexture", 14, js_text_texture),
    JS_CFUNC_DEF("videoOpen", 3, js_video_open),
    JS_CFUNC_DEF("mediaOpen", 4, js_media_open),
    JS_CFUNC_DEF("captureOpen", 3, js_capture_open),
    JS_CFUNC_DEF("sandbox", 0, js_sandbox),
    JS_CFUNC_DEF("videoUpload", 2, js_video_upload),
    JS_CFUNC_DEF("videoRunning", 1, js_video_running),
    JS_CFUNC_DEF("videoClose", 1, js_video_close),
    JS_CFUNC_DEF("cameraOpen", 3, js_camera_open),
    JS_CFUNC_DEF("cameras", 0, js_cameras),
    JS_CFUNC_DEF("findProgram", 1, js_find_program),
    JS_CFUNC_DEF("audioStart", 0, js_audio_start),
    JS_CFUNC_DEF("audioFeatures", 0, js_audio_features),
    JS_CFUNC_DEF("fetchStart", 1, js_fetch_start),
    JS_CFUNC_DEF("shareDone", 2, js_share_done),
    JS_CFUNC_DEF("patchBayStart", 2, js_patch_bay_start),
    JS_CFUNC_DEF("patchBaySend", 1, js_patch_bay_send),
    JS_CFUNC_DEF("patchBayStop", 0, js_patch_bay_stop),
    JS_CFUNC_DEF("setGalleryServer", 1, js_set_gallery_server),
    JS_CFUNC_DEF("patchBayPublish", 1, js_patch_bay_publish),
    JS_CFUNC_DEF("patchBayVideo", 3, js_patch_bay_video),
    JS_CFUNC_DEF("httpRequest", 4, js_http_request),
    JS_CFUNC_DEF("midiStart", 0, js_midi_start),
    JS_CFUNC_DEF("midiInputs", 0, js_midi_inputs),
    JS_CFUNC_DEF("midiOutputs", 0, js_midi_outputs),
    JS_CFUNC_DEF("midiSend", 2, js_midi_send),
    JS_CFUNC_DEF("midiPoll", 0, js_midi_poll),
    JS_CFUNC_DEF("environment", 1, js_environment),
    JS_CFUNC_DEF("fetchSync", 1, js_fetch_sync),
    JS_CFUNC_DEF("writeFile", 2, js_write_file),
    JS_CFUNC_DEF("now", 0, js_now),
    JS_CFUNC_DEF("screencap", 0, js_screencap),
    JS_CFUNC_DEF("recordStart", 0, js_record_start),
    JS_CFUNC_DEF("recordStop", 0, js_record_stop),
    JS_CFUNC_DEF("readFile", 2, js_read_file),
    JS_CFUNC_DEF("drawableSize", 0, js_drawable_size),
    JS_CFUNC_DEF("consoleMessage", 1, js_console_message),
    JS_CFUNC_DEF("showError", 1, js_show_error),
    JS_CFUNC_DEF("exit", 1, js_exit),
};

JSValue bindings_create(JSContext *ctx, const char *device) {
    audio_device = device;
    JSValue native = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, native, native_functions, sizeof native_functions / sizeof native_functions[0]);
    return native;
}
