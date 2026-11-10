// hydra-native: runs hydra-synth (bundled with a regl shim) in QuickJS and renders with GLES in an SDL window.
#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

// stb_image_write.h 1.16 (public domain or MIT, see THIRD_PARTY.md) writes the PNG files.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_image.h"

#include "canvas2d.h"
#include "webgl.h"
#include "midi.h"
#include "native.h"
#include "overlay.h"
#include "thumbnails.h"
#include "settings.h"
#include "ui.h"
#include "quickjs-libc.h"
#include "quickjs.h"

#define BUNDLE_FILE_NAME "hydra-bundle.js"
#define APP_ID "hydra-native" // the Wayland app id and the X11 class, the name of assets/hydra-native.desktop
#define WINDOW_ICON_FILE "../assets/icon/hicolor/48x48/apps/hydra-native.png"
#define SOCKET_FILE_NAME "hydra-native.sock"
#define WATCH_INTERVAL_MS 250
#define BENCH_WARMUP_MS 3000
#define BENCH_FINISH_FRAMES 30
#define CLIENT_TIMEOUT_SECONDS 2
#define MAX_REPLY_SIZE 4096
#define SCREENSHOT_DELAY_MS 1500

// The options of one run; the options that are also settings go straight into the settings of this run.
struct options {
    const char *files[32];
    int file_count;
    const char *screenshot_path;
    double bench_seconds;
    bool hidden;
    bool send;
};

// The settings of the file (`stored`) and of this run (`active`: the file with the command line on top).
static struct settings stored_settings, active_settings;

#define SWAP_MARGIN_MS 3.0 // time left for the swap after the timers
#define MIN_FRAME_PERIOD_MS 4.0
#define MAX_FRAME_PERIOD_MS 50.0
static double frame_period_ms, last_swap;
static JSRuntime *runtime;
static JSContext *js;
static char executable_path_directory[PATH_MAX];
static SDL_Window *window;
static volatile sig_atomic_t quit_requested; // also set by SIGINT and SIGTERM
static int exit_code;

// ---------------------------------------------------------------- helpers

static double now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000.0 + time.tv_nsec / 1e6;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';
    if (length) *length = got;
    return text;
}

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

static void run_pending_jobs(void) {
    JSContext *job_context;
    int status;
    while ((status = JS_ExecutePendingJob(runtime, &job_context)) != 0)
        if (status < 0) js_std_dump_error(job_context);
}

static char *error_message(JSValue error) {
    const char *text = JS_ToCString(js, error);
    const char *stack = NULL;
    JSValue stack_value = JS_UNDEFINED;
    if (JS_IsError(js, error)) {
        stack_value = JS_GetPropertyStr(js, error, "stack");
        if (!JS_IsUndefined(stack_value)) stack = JS_ToCString(js, stack_value);
    }
    char *message;
    if (asprintf(&message, "%s\n%s", text ? text : "error", stack ? stack : "") < 0) message = strdup("error");
    JS_FreeCString(js, text);
    if (stack) JS_FreeCString(js, stack);
    JS_FreeValue(js, stack_value);
    return message;
}

// Evaluations that wait for a promise (top-level await); their errors arrive later.
#define MAX_PENDING_EVALUATIONS 32
static JSValue pending_evaluations[MAX_PENDING_EVALUATIONS];
static int pending_evaluation_count;

// Evaluates code in the global scope with top-level await, like the web editor, but `var` and functions
// stay global as in the browser console. Returns NULL on success, else a malloc'd message with the stack.
static char *evaluate(const char *code, size_t length, const char *name) {
    JSValue result = JS_Eval(js, code, length, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC);
    char *message = NULL;
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(js);
        message = error_message(exception);
        JS_FreeValue(js, exception);
        JS_FreeValue(js, result);
        run_pending_jobs();
        return message;
    }
    run_pending_jobs();
    switch (JS_PromiseState(js, result)) {
    case JS_PROMISE_REJECTED: {
        JSValue reason = JS_PromiseResult(js, result);
        message = error_message(reason);
        JS_FreeValue(js, reason);
        break;
    }
    case JS_PROMISE_PENDING:
        if (pending_evaluation_count < MAX_PENDING_EVALUATIONS) {
            pending_evaluations[pending_evaluation_count++] = result;
            return NULL;
        }
        break;
    default: break;
    }
    JS_FreeValue(js, result);
    return message;
}

static void check_pending_evaluations(void) {
    for (int i = 0; i < pending_evaluation_count;) {
        JSValue promise = pending_evaluations[i];
        JSPromiseStateEnum state = JS_PromiseState(js, promise);
        if (state == JS_PROMISE_PENDING) {
            i++;
            continue;
        }
        if (state == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(js, promise);
            char *message = error_message(reason);
            fprintf(stderr, "%s\n", message);
            ui_show_message(message, true);
            free(message);
            JS_FreeValue(js, reason);
        }
        JS_FreeValue(js, promise);
        pending_evaluations[i] = pending_evaluations[--pending_evaluation_count];
    }
}

static char *evaluate_file(const char *path) {
    size_t length;
    char *code = read_file(path, &length);
    if (!code) {
        char *message;
        if (asprintf(&message, "cannot read %s: %s", path, strerror(errno)) < 0) return strdup("cannot read file");
        return message;
    }
    char *message = evaluate(code, length, path);
    free(code);
    return message;
}

// Calls a global JS function by name, used for the frame callback and input events.
static void call_global(const char *name, int argc, JSValue *argv) {
    JSValue global = JS_GetGlobalObject(js);
    JSValue function = JS_GetPropertyStr(js, global, name);
    if (JS_IsFunction(js, function)) {
        JSValue result = JS_Call(js, function, global, argc, argv);
        if (JS_IsException(result)) js_std_dump_error(js);
        JS_FreeValue(js, result);
    }
    for (int i = 0; i < argc; i++) JS_FreeValue(js, argv[i]);
    JS_FreeValue(js, function);
    JS_FreeValue(js, global);
}

// ---------------------------------------------------------------- GL bindings

static GLuint compile_shader(JSContext *ctx, GLenum type, const char *source, JSValue *error) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        *error = JS_ThrowSyntaxError(ctx, "%s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static JSValue js_create_program(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *vertex_source = JS_ToCString(ctx, argv[0]);
    const char *fragment_source = JS_ToCString(ctx, argv[1]);
    JSValue error = JS_UNDEFINED;
    GLuint vertex = compile_shader(ctx, GL_VERTEX_SHADER, vertex_source, &error);
    GLuint fragment = vertex ? compile_shader(ctx, GL_FRAGMENT_SHADER, fragment_source, &error) : 0;
    JS_FreeCString(ctx, vertex_source);
    JS_FreeCString(ctx, fragment_source);
    if (!fragment) {
        if (vertex) glDeleteShader(vertex);
        return error;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glDeleteProgram(program);
        return JS_ThrowSyntaxError(ctx, "link: %s", log);
    }
    return JS_NewInt32(ctx, (int32_t)program);
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
    webgl_release();
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

static JSValue js_video_open(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *command = JS_ToCString(ctx, argv[0]);
    int handle = video_open(command, to_int(ctx, argv[1]), to_int(ctx, argv[2]));
    JS_FreeCString(ctx, command);
    if (handle < 0) return JS_ThrowInternalError(ctx, "cannot start video source");
    return JS_NewInt32(ctx, handle);
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

static const char *audio_device_option;

static JSValue js_audio_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, audio_start(audio_device_option));
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

static const char *gallery_server; // NULL: no gallery and no patch bay
static char gallery_server_url[SETTINGS_TEXT];

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
    snprintf(gallery_server_url, sizeof gallery_server_url, "%s", url ? url : "");
    gallery_server = gallery_server_url[0] ? gallery_server_url : NULL;
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

static void patch_bay_line(const char *text) {
    JSValue arguments[1] = { JS_NewString(js, text) };
    call_global("__patchBay", 1, arguments);
}

// Sharing: the screenshot comes from the next frame, then __shareSketch (js/editor-actions.js) sends the
// requests of the web editor with fetch() and answers through native.shareDone().
static struct {
    bool waiting_for_frame, sending;
    char *code;
    char name[256];
} share;

static bool start_share(const char *code, const char *name) {
    if (!gallery_server || share.sending || share.waiting_for_frame) return false;
    free(share.code);
    share.code = strdup(code);
    snprintf(share.name, sizeof share.name, "%s", name);
    share.waiting_for_frame = true;
    return true;
}

static JSValue js_share_done(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *text = JS_ToCString(ctx, argv[1]);
    share.sending = false;
    ui_share_done(JS_ToBool(ctx, argv[0]), text ? text : "");
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static unsigned char *read_window_pixels(int width, int height);

static void set_window_title(const char *url) { SDL_SetWindowTitle(window, url); }

// Tells the input method where the cursor is, so that its candidate window opens there, as in the browser.
static void update_text_input_rect(int drawable_width) {
    static SDL_Rect last;
    static int last_cursor;
    int x, y, width, height, window_width, window_height;
    if (!ui_text_input_rect(&x, &y, &width, &height)) return;
    SDL_GetWindowSize(window, &window_width, &window_height);
    float scale = drawable_width > 0 ? (float)window_width / (float)drawable_width : 1;
    SDL_Rect rect = { (int)((float)x * scale), (int)((float)y * scale), (int)((float)width * scale), (int)((float)height * scale) };
    if (rect.x == last.x && rect.y == last.y && rect.w == last.w && rect.h == last.h && last_cursor == rect.w) return;
    last = rect;
    last_cursor = rect.w;
    SDL_SetTextInputArea(window, &rect, rect.w);
}

struct png_buffer {
    unsigned char *data;
    size_t length, capacity;
};

static void append_png(void *context, void *data, int size) {
    struct png_buffer *buffer = context;
    if (buffer->length + (size_t)size > buffer->capacity) {
        buffer->capacity = (buffer->length + (size_t)size) * 2;
        buffer->data = realloc(buffer->data, buffer->capacity);
    }
    memcpy(buffer->data + buffer->length, data, (size_t)size);
    buffer->length += (size_t)size;
}

static void continue_share(int width, int height) {
    share.waiting_for_frame = false;
    unsigned char *pixels = read_window_pixels(width, height);
    stbi_flip_vertically_on_write(1);
    struct png_buffer png = { 0 };
    bool ok = stbi_write_png_to_func(append_png, &png, width, height, 4, pixels, width * 4) != 0;
    free(pixels);
    if (!ok) {
        free(png.data);
        return ui_share_done(false, "cannot encode the screenshot");
    }
    share.sending = true;
    JSValue arguments[4] = { JS_NewString(js, gallery_server), JS_NewString(js, share.code),
                             JS_NewArrayBufferCopy(js, png.data, png.length), JS_NewString(js, share.name) };
    free(png.data);
    call_global("__shareSketch", 4, arguments);
}

static void fetch_done(int id, bool ok, const char *data, size_t length) {
    if (thumbnails_fetch_done(id, ok, data, length) || canvas2d_fetch_done(id, ok, data, length)) return;
    // The text for scripts and JSON, the bytes for binary files such as the fonts of p5.js loadFont.
    JSValue arguments[4] = { JS_NewInt32(js, id), JS_NewBool(js, ok), JS_NewStringLen(js, data, length),
                             JS_NewArrayBufferCopy(js, (const uint8_t *)data, length) };
    call_global("__fetchDone", 4, arguments);
}

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
    midi_poll(add_midi_message);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, midi_inputs_changed()));
    JS_SetPropertyStr(ctx, result, "messages", midi_messages);
    return result;
}

static char *read_all(int fd, size_t *length);

// fetchSync(url) waits for curl; only for loading a sketch by URL, as the start waits for it anyway.
static JSValue js_fetch_sync(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    char *argv_curl[] = { "curl", "-fsSL", "--max-time", "15", "--", (char *)url, NULL };
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        JS_FreeCString(ctx, url);
        return JS_ThrowInternalError(ctx, "cannot start curl");
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        execvp(argv_curl[0], argv_curl);
        _exit(127);
    }
    close(pipe_fds[1]);
    JS_FreeCString(ctx, url);
    size_t length;
    char *data = read_all(pipe_fds[0], &length);
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    JSValue result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? JS_NewStringLen(ctx, data, length)
                                                                     : JS_ThrowInternalError(ctx, "the request failed");
    free(data);
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
    if (path && text) {
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
    size_t length;
    char *text = read_file(path, &length);
    bool binary = argc > 1 && JS_ToBool(ctx, argv[1]);
    JSValue result = !text ? JS_ThrowReferenceError(ctx, "cannot read %s: %s", path, strerror(errno))
                     : binary ? JS_NewArrayBufferCopy(ctx, (const uint8_t *)text, length) : JS_NewStringLen(ctx, text, length);
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

static JSValue js_exit(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    exit_code = argc > 0 ? to_int(ctx, argv[0]) : 0;
    quit_requested = true;
    return JS_UNDEFINED;
}

static JSValue js_screencap(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv);
static JSValue js_record_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv);
static JSValue js_record_stop(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv);

static const JSCFunctionListEntry native_functions[] = {
    JS_CFUNC_DEF("createProgram", 2, js_create_program),
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
    JS_CFUNC_DEF("videoOpen", 3, js_video_open),
    JS_CFUNC_DEF("videoUpload", 2, js_video_upload),
    JS_CFUNC_DEF("videoRunning", 1, js_video_running),
    JS_CFUNC_DEF("videoClose", 1, js_video_close),
    JS_CFUNC_DEF("audioStart", 0, js_audio_start),
    JS_CFUNC_DEF("audioFeatures", 0, js_audio_features),
    JS_CFUNC_DEF("fetchStart", 1, js_fetch_start),
    JS_CFUNC_DEF("shareDone", 2, js_share_done),
    JS_CFUNC_DEF("patchBayStart", 2, js_patch_bay_start),
    JS_CFUNC_DEF("patchBaySend", 1, js_patch_bay_send),
    JS_CFUNC_DEF("patchBayStop", 0, js_patch_bay_stop),
    JS_CFUNC_DEF("setGalleryServer", 1, js_set_gallery_server),
    JS_CFUNC_DEF("patchBayPublish", 1, js_patch_bay_publish),
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
    JS_CFUNC_DEF("exit", 1, js_exit),
};

// ---------------------------------------------------------------- input events

static const char *dom_key_name(SDL_Keycode key) {
    switch (key) {
    case SDLK_RETURN: return "Enter";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_SPACE: return " ";
    case SDLK_TAB: return "Tab";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_UP: return "ArrowUp";
    case SDLK_DOWN: return "ArrowDown";
    case SDLK_LEFT: return "ArrowLeft";
    case SDLK_RIGHT: return "ArrowRight";
    default: return NULL;
    }
}

static void dispatch_key(const SDL_KeyboardEvent *event) {
    char key[32];
    const char *special = dom_key_name(event->key);
    if (special) {
        snprintf(key, sizeof key, "%s", special);
    } else {
        snprintf(key, sizeof key, "%s", SDL_GetKeyName(event->key));
        // DOM KeyboardEvent.key uses lower case letters without shift.
        if (strlen(key) == 1 && !(event->mod & SDL_KMOD_SHIFT)) key[0] = (char)SDL_tolower(key[0]);
    }
    JSValue object = JS_NewObject(js);
    JS_SetPropertyStr(js, object, "key", JS_NewString(js, key));
    JS_SetPropertyStr(js, object, "code", JS_NewString(js, SDL_GetScancodeName(event->scancode)));
    JS_SetPropertyStr(js, object, "ctrlKey", JS_NewBool(js, (event->mod & SDL_KMOD_CTRL) != 0));
    JS_SetPropertyStr(js, object, "shiftKey", JS_NewBool(js, (event->mod & SDL_KMOD_SHIFT) != 0));
    JS_SetPropertyStr(js, object, "altKey", JS_NewBool(js, (event->mod & SDL_KMOD_ALT) != 0));
    JS_SetPropertyStr(js, object, "metaKey", JS_NewBool(js, (event->mod & SDL_KMOD_GUI) != 0));
    JS_SetPropertyStr(js, object, "repeat", JS_NewBool(js, event->repeat != 0));
    JSValue arguments[2] = { JS_NewString(js, event->type == SDL_EVENT_KEY_DOWN ? "keydown" : "keyup"), object };
    call_global("__dispatchEvent", 2, arguments);
}

static void dispatch_mouse(const char *type, int x, int y) {
    JSValue object = JS_NewObject(js);
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(NULL, NULL);
    // SDL button masks are left 1, middle 2, right 4; DOM buttons are left 1, right 2, middle 4.
    int dom_buttons = (buttons & SDL_BUTTON_LMASK ? 1 : 0) | (buttons & SDL_BUTTON_RMASK ? 2 : 0) |
                      (buttons & SDL_BUTTON_MMASK ? 4 : 0);
    JS_SetPropertyStr(js, object, "pageX", JS_NewInt32(js, x));
    JS_SetPropertyStr(js, object, "pageY", JS_NewInt32(js, y));
    JS_SetPropertyStr(js, object, "clientX", JS_NewInt32(js, x));
    JS_SetPropertyStr(js, object, "clientY", JS_NewInt32(js, y));
    JS_SetPropertyStr(js, object, "buttons", JS_NewInt32(js, dom_buttons));
    JSValue arguments[2] = { JS_NewString(js, type), object };
    call_global("__dispatchEvent", 2, arguments);
}

// Window coordinates to drawable pixels (they differ with display scaling).
static float pixel_scale(void) {
    int window_width, drawable_width;
    SDL_GetWindowSize(window, &window_width, NULL);
    SDL_GetWindowSizeInPixels(window, &drawable_width, NULL);
    return window_width > 0 ? (float)drawable_width / (float)window_width : 1.0f;
}

// The overlay gets keys and mouse first; sketches get them too, as window listeners do in the browser.
static void handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        float scale = pixel_scale();
        switch (event.type) {
        case SDL_EVENT_QUIT: quit_requested = true; break;
        case SDL_EVENT_KEY_DOWN:
            ui_key(&event.key);
            dispatch_key(&event.key);
            break;
        case SDL_EVENT_KEY_UP: dispatch_key(&event.key); break;
        case SDL_EVENT_TEXT_INPUT: ui_text_input(event.text.text); break;
        case SDL_EVENT_TEXT_EDITING: ui_text_editing(event.edit.text); break;
        case SDL_EVENT_MOUSE_WHEEL: ui_wheel(event.wheel.integer_y); break;
        case SDL_EVENT_MOUSE_MOTION:
            ui_mouse_motion(event.motion.x * scale, event.motion.y * scale);
            dispatch_mouse("mousemove", (int)event.motion.x, (int)event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT)
                ui_mouse_button(event.button.x * scale, event.button.y * scale, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                                event.button.clicks, SDL_GetModState());
            dispatch_mouse(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mousedown" : "mouseup", (int)event.button.x,
                           (int)event.button.y);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            JSValue arguments[2] = { JS_NewString(js, "resize"), JS_NewObject(js) };
            call_global("__dispatchEvent", 2, arguments);
            break;
        }
        }
    }
}

// ---------------------------------------------------------------- code input: socket and file watch

static void default_socket_path(char *path, size_t size) {
    const char *directory = getenv("XDG_RUNTIME_DIR");
    snprintf(path, size, "%s/%s", directory ? directory : "/tmp", SOCKET_FILE_NAME);
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_socket(const char *path) {
    int existing = connect_socket(path);
    if (existing >= 0) {
        close(existing);
        fprintf(stderr, "another hydra-native listens on %s\n", path);
        return -1;
    }
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 4) != 0) {
        fprintf(stderr, "cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static char *read_all(int fd, size_t *length) {
    size_t capacity = 65536, used = 0;
    char *buffer = malloc(capacity);
    ssize_t count;
    while ((count = read(fd, buffer + used, capacity - used - 1)) > 0) {
        used += (size_t)count;
        if (used + 1 == capacity) buffer = realloc(buffer, capacity *= 2);
    }
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

// ponytail: one client at a time, read synchronously with a timeout; editors send small blocks and close.
static void serve_client(int listener) {
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) return;
    struct timeval timeout = { .tv_sec = CLIENT_TIMEOUT_SECONDS };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    size_t length;
    char *code = read_all(client, &length);
    char *error = ui_evaluate_socket(code, length);
    char reply[MAX_REPLY_SIZE];
    snprintf(reply, sizeof reply, "%s\n", error ? error : "ok");
    if (error) fprintf(stderr, "%s", reply);
    if (write(client, reply, strlen(reply)) < 0) fprintf(stderr, "socket reply failed: %s\n", strerror(errno));
    free(error);
    free(code);
    close(client);
}

static int send_code(const char *path) {
    int fd = connect_socket(path);
    if (fd < 0) {
        fprintf(stderr, "no hydra-native listens on %s\n", path);
        return 2;
    }
    size_t length;
    char *code = read_all(STDIN_FILENO, &length);
    bool written = write(fd, code, length) == (ssize_t)length;
    free(code);
    shutdown(fd, SHUT_WR);
    char *reply = read_all(fd, &length);
    close(fd);
    fputs(reply, written && strncmp(reply, "ok\n", 3) == 0 ? stdout : stderr);
    int status = written && strncmp(reply, "ok\n", 3) == 0 ? 0 : 1;
    free(reply);
    return status;
}

static long long modification_time(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 ? info.st_mtim.tv_sec * 1000000000LL + info.st_mtim.tv_nsec : 0;
}

// ---------------------------------------------------------------- setup and main loop

static void usage(void) {
    fprintf(stderr,
            "usage: hydra-native [options] [file.js | https://hydra.ojack.xyz/?code=... ...]\n"
            "       hydra-native --send < code.js\n"
            "  The last file opens in the code overlay; Ctrl+S saves it. See src/keymap.c for all keys.\n"
            "  --watch            load and evaluate the last file again when it changes on disk\n"
            "  --font PATH        monospace TrueType font of the overlay (default %s)\n"
            "  --font-size PIXELS font size of the overlay (default %.0f)\n"
            "  --theme NAME       color theme of the overlay: hydra (default), contrast, solarized, paper\n"
            "                     Options override ~/.config/hydra-native/settings.json for one run.\n"
            "  --hide-code        start with the code overlay hidden (Ctrl+Shift+H shows it)\n"
            "  --lang CODE        language of the info panel (ja es ar id pt-br ch ko fr de; default: $LANG)\n"
            "  --l10n-url URL     load the language of --lang from this editor.json\n"
            "  --gallery-server URL  the hydra gallery server for Ctrl+Shift+G (VITE_SERVER_URL of the web editor)\n"
            "  --size WxH         window size (default %dx%d)\n"
            "  --fullscreen       start in fullscreen\n"
            "  --audio SOURCE     PulseAudio/PipeWire source for `a`, e.g. @DEFAULT_MONITOR@\n"
            "  --no-analog-agc    leave the volume of the audio source alone (Chrome adjusts it)\n"
            "  --socket PATH      code socket (default $XDG_RUNTIME_DIR/%s)\n"
            "  --no-vsync         do not wait for the display refresh\n"
            "  --hidden           do not show the window (checks)\n"
            "  --bench SECONDS    measure frame rate after a warm-up, print JSON and exit\n"
            "  --screenshot FILE  write the window content with the code as PNG after %d ms and exit\n",
            SETTINGS_DEFAULT_FONT, (double)SETTINGS_DEFAULT_FONT_SIZE, SETTINGS_DEFAULT_WIDTH, SETTINGS_DEFAULT_HEIGHT, SOCKET_FILE_NAME,
            SCREENSHOT_DELAY_MS);
}

// Fills `options`, and writes the options that are settings into `settings` (they override the settings file).
static bool parse_options(int argc, char **argv, struct options *options, struct settings *settings) {
    *options = (struct options){ 0 };
#define TEXT_OPTION(name, field) \
    else if (!strcmp(argument, name) && has_value) snprintf(settings->field, sizeof settings->field, "%s", argv[++i]);
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        bool has_value = i + 1 < argc;
        if (!strcmp(argument, "--watch")) settings->watch = true;
        else if (!strcmp(argument, "--no-vsync")) settings->vsync = false;
        else if (!strcmp(argument, "--hidden")) options->hidden = true;
        else if (!strcmp(argument, "--fullscreen")) settings->fullscreen = true;
        else if (!strcmp(argument, "--hide-code")) settings->code_visible = false;
        else if (!strcmp(argument, "--no-analog-agc")) settings->analog_agc = false;
        TEXT_OPTION("--lang", language)
        TEXT_OPTION("--gallery-server", gallery_server)
        TEXT_OPTION("--l10n-url", l10n_url)
        TEXT_OPTION("--font", font)
        TEXT_OPTION("--theme", theme)
        TEXT_OPTION("--audio", audio_source)
        TEXT_OPTION("--socket", socket)
        else if (!strcmp(argument, "--screenshot") && has_value) options->screenshot_path = argv[++i];
        else if (!strcmp(argument, "--font-size") && has_value) settings->font_size = (float)atof(argv[++i]);
        else if (!strcmp(argument, "--send")) options->send = true;
        else if (!strcmp(argument, "--bench") && has_value) options->bench_seconds = atof(argv[++i]);
        else if (!strcmp(argument, "--size") && has_value) {
            if (sscanf(argv[++i], "%dx%d", &settings->window_width, &settings->window_height) != 2) return false;
        } else if (argument[0] == '-') return false;
        else if (options->file_count < (int)(sizeof options->files / sizeof options->files[0]))
            options->files[options->file_count++] = argument;
    }
#undef TEXT_OPTION
    return true;
}

static bool create_window(const struct options *options) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
    SDL_SetAppMetadata("hydra-native", NULL, APP_ID);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options->hidden) flags |= SDL_WINDOW_HIDDEN;
    if (active_settings.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window = SDL_CreateWindow("hydra-native", active_settings.window_width, active_settings.window_height, flags);
    if (!window || !SDL_GL_CreateContext(window)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(active_settings.vsync ? 1 : 0);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    if (mode && mode->refresh_rate > 0) frame_period_ms = 1000.0 / mode->refresh_rate;
    SDL_StartTextInput(window);

    // Every hydra pass draws the same triangle, so its buffer stays bound at attribute 0.
    static const GLfloat triangle[] = { -2, 0, 0, -2, 2, 2 };
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    return true;
}

// Without the icon file (a build moved without its assets) the window keeps the default icon.
static void set_window_icon(const char *directory) {
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", directory, WINDOW_ICON_FILE);
    int width, height, channels;
    unsigned char *pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels) return;
    SDL_Surface *icon = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    if (icon) SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
    stbi_image_free(pixels);
}

static bool executable_directory(char *directory, size_t size) {
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length < 0) return false;
    executable[length] = '\0';
    snprintf(directory, size, "%s", dirname(executable));
    return true;
}

static bool load_bundle(const char *directory) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%s", directory, BUNDLE_FILE_NAME);
    char *error = evaluate_file(path);
    if (error) {
        fprintf(stderr, "%s\n", error);
        free(error);
        return false;
    }
    return true;
}

// Writes the default framebuffer as PNG (rows flipped, because GL starts at the bottom).
// The window as opaque RGBA rows from the bottom (malloc'd), as glReadPixels gives them.
static unsigned char *read_window_pixels(int width, int height) {
    unsigned char *pixels = malloc((size_t)width * (size_t)height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (size_t i = 3; i < (size_t)width * (size_t)height * 4; i += 4) pixels[i] = 255;
    return pixels;
}

static bool write_png(const char *path, int width, int height) {
    unsigned char *pixels = read_window_pixels(width, height);
    stbi_flip_vertically_on_write(1);
    bool ok = stbi_write_png(path, width, height, 4, pixels, width * 4) != 0;
    free(pixels);
    return ok;
}

static bool screencap_requested;

static void request_screencap(void) { screencap_requested = true; }

static JSValue js_screencap(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    request_screencap();
    return JS_UNDEFINED;
}

static void download_base(char *base, size_t size);

static char recording_path[PATH_MAX + 80];

static JSValue js_record_start(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv) {
    char base[PATH_MAX + 64];
    download_base(base, sizeof base);
    snprintf(recording_path, sizeof recording_path, "%s.webm", base);
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
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

// Like screencap() of the web editor: the visuals without the code as PNG, and the code as .js,
// named hydra-Y-M-D-H.M.S, in ~/Downloads (or the current directory).
// The base name of downloads, like in the web editor: hydra-Y-M-D-H.M.S in ~/Downloads (or the current directory).
static void download_base(char *base, size_t size) {
    time_t seconds = time(NULL);
    struct tm *local = localtime(&seconds);
    const char *home = getenv("HOME");
    char directory[PATH_MAX];
    snprintf(directory, sizeof directory, "%s/Downloads", home ? home : ".");
    if (access(directory, W_OK) != 0) snprintf(directory, sizeof directory, ".");
    snprintf(base, size, "%s/hydra-%d-%d-%d-%d.%d.%d", directory, local->tm_year + 1900, local->tm_mon + 1,
             local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
}

static void save_screencap(int width, int height) {
    char base[PATH_MAX + 64];
    download_base(base, sizeof base);
    char image_path[PATH_MAX + 80], code_path[PATH_MAX + 80], message[PATH_MAX + 128];
    snprintf(image_path, sizeof image_path, "%s.png", base);
    snprintf(code_path, sizeof code_path, "%s.js", base);
    bool ok = write_png(image_path, width, height);
    FILE *file = ok ? fopen(code_path, "wb") : NULL;
    if (file) {
        fputs(ui_code(), file);
        ok = fclose(file) == 0;
    } else {
        ok = false;
    }
    snprintf(message, sizeof message, ok ? "saved %s.png and .js" : "cannot save %s.png", base);
    ui_show_message(message, !ok);
}

static char *call_text_function(const char *function, const char *argument) {
    JSValue global = JS_GetGlobalObject(js);
    JSValue callee = JS_GetPropertyStr(js, global, function);
    JSValue value = JS_NewString(js, argument);
    JSValue result = JS_Call(js, callee, global, 1, &value);
    char *text = NULL;
    if (JS_IsException(result)) {
        js_std_dump_error(js);
    } else {
        const char *string = JS_ToCString(js, result);
        if (string) text = strdup(string);
        JS_FreeCString(js, string);
    }
    JS_FreeValue(js, result);
    JS_FreeValue(js, value);
    JS_FreeValue(js, callee);
    JS_FreeValue(js, global);
    run_pending_jobs();
    return text;
}

#define SHOW_CODE_FALSE "// show-code: false"

static bool is_sketch_url(const char *argument) {
    bool url = !strncmp(argument, "http://", 7) || !strncmp(argument, "https://", 8) || argument[0] == '?';
    return url && (strstr(argument, "code=") || strstr(argument, "sketch_id="));
}


static double run_timers(void) {
    JSValue global = JS_GetGlobalObject(js);
    JSValue function = JS_GetPropertyStr(js, global, "__runTimers");
    JSValue now = JS_NewFloat64(js, now_ms());
    JSValue result = JS_Call(js, function, global, 1, &now);
    double next = -1;
    if (JS_IsException(result)) js_std_dump_error(js);
    else JS_ToFloat64(js, &next, result);
    JS_FreeValue(js, result);
    JS_FreeValue(js, function);
    JS_FreeValue(js, global);
    run_pending_jobs();
    return next;
}

static void serve_client(int listener);

// Waits until `deadline` (ms of now_ms()), runs each timer at its time, and handles input and socket code meanwhile.
static void wait_for_frame(double deadline, int listener) {
    for (;;) {
        double next = run_timers(), now = now_ms();
        if (now >= deadline || quit_requested) return;
        double until = next >= 0 && next < deadline ? next : deadline;
        int wait = (int)(until - now);
        if (wait > 0 && SDL_WaitEventTimeout(NULL, wait)) handle_events();
        struct pollfd poll_listener = { .fd = listener, .events = POLLIN };
        if (listener >= 0 && poll(&poll_listener, 1, 0) > 0) serve_client(listener);
    }
}

static bool is_fullscreen(void) { return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0; }

static void toggle_fullscreen(void) { SDL_SetWindowFullscreen(window, !is_fullscreen()); }



static void request_quit(void) { quit_requested = true; }

static void handle_quit_signal(int signal_number) { quit_requested = true; }

int main(int argc, char **argv) {
    // The settings file first: the command line overrides it for this run.
    char settings_file[PATH_MAX], settings_error[SETTINGS_TEXT] = "";
    settings_defaults(&stored_settings);
    if (!settings_path(settings_file, sizeof settings_file)) settings_file[0] = '\0';
    else if (!settings_load(&stored_settings, settings_file, settings_error, sizeof settings_error)) settings_defaults(&stored_settings);
    active_settings = stored_settings;
    struct options options;
    if (!parse_options(argc, argv, &options, &active_settings)) {
        usage();
        return 2;
    }
    const struct settings *settings = &active_settings;
    char socket_path[PATH_MAX];
    if (settings->socket[0]) snprintf(socket_path, sizeof socket_path, "%.*s", (int)sizeof socket_path - 1, settings->socket);
    else default_socket_path(socket_path, sizeof socket_path);
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", socket_path);
        return 2;
    }
    if (options.send) return send_code(socket_path);

    signal(SIGPIPE, SIG_IGN);
    audio_device_option = settings->audio_source[0] ? settings->audio_source : NULL;
    audio_set_analog_agc(settings->analog_agc);
    // The analog gain control changes the volume of the audio source; every way out restores it.
    atexit(audio_stop);
    snprintf(gallery_server_url, sizeof gallery_server_url, "%.*s", (int)sizeof gallery_server_url - 1, settings->gallery_server);
    gallery_server = gallery_server_url[0] ? gallery_server_url : NULL;
    if (!create_window(&options)) return 1;
    // After SDL, which installs handlers of its own: the first SIGINT or SIGTERM ends the loop, so that the audio
    // source gets its volume back; a second one ends the process at once.
    struct sigaction quit_action = { .sa_handler = handle_quit_signal, .sa_flags = SA_RESETHAND };
    sigaction(SIGINT, &quit_action, NULL);
    sigaction(SIGTERM, &quit_action, NULL);

    runtime = JS_NewRuntime();
    js = JS_NewContext(runtime);
    js_std_add_helpers(js, argc, argv);
    JSValue global = JS_GetGlobalObject(js);
    JSValue native = JS_NewObject(js);
    JS_SetPropertyFunctionList(js, native, native_functions, sizeof native_functions / sizeof native_functions[0]);
    canvas2d_install(js, native);
    webgl_install(js, native);
    char directory[PATH_MAX];
    if (!executable_directory(directory, sizeof directory)) return 1;
    snprintf(executable_path_directory, sizeof executable_path_directory, "%s", directory);
    JS_SetPropertyStr(js, native, "directory", JS_NewString(js, directory));
    JS_SetPropertyStr(js, native, "language", settings->language[0] ? JS_NewString(js, settings->language) : JS_NULL);
    JS_SetPropertyStr(js, native, "galleryServer", gallery_server ? JS_NewString(js, gallery_server) : JS_NULL);
    JS_SetPropertyStr(js, native, "l10nUrl", settings->l10n_url[0] ? JS_NewString(js, settings->l10n_url) : JS_NULL);
    JS_SetPropertyStr(js, global, "native", native);
    JS_FreeValue(js, global);
    if (!load_bundle(directory)) return 1;
    set_window_icon(directory);

    // The last file is the sketch of the overlay. It may not exist yet; Ctrl+S creates it.
    // A URL of the web editor (https://hydra.ojack.xyz/?code=...) or its "?code=..." part works in place of a file.
    const char *sketch = options.file_count ? options.files[options.file_count - 1] : NULL;
    char *url_code = NULL;
    for (int i = 0; i < options.file_count; i++) {
        const char *argument = options.files[i];
        char *error = NULL;
        if (is_sketch_url(argument)) {
            char *code = call_text_function("__codeFromUrl", argument);
            if (!code) {
                fprintf(stderr, "cannot open %s\n", argument);
                continue;
            }
            error = evaluate(code, strlen(code), argument);
            if (argument == sketch) url_code = code;
            else free(code);
        } else if (argument != sketch || access(sketch, F_OK) == 0) {
            error = evaluate_file(argument);
        }
        if (error) fprintf(stderr, "%s\n", error);
        free(error);
    }
    if (url_code) {
        sketch = NULL;
        set_window_title(options.files[options.file_count - 1]);
    }
    static const struct ui_host host = {
        .evaluate = evaluate,
        .toggle_fullscreen = toggle_fullscreen,
        .is_fullscreen = is_fullscreen,
        .quit = request_quit,
        .call = call_text_function,
        .screencap = request_screencap,
        .share = start_share,
        .set_title = set_window_title,
    };
    char emoji_font[PATH_MAX + 64];
    snprintf(emoji_font, sizeof emoji_font, "%s/../vendor/fonts/NotoEmoji.ttf", directory);
    if (!ui_init(&stored_settings, &active_settings, settings_file[0] ? settings_file : NULL, emoji_font, sketch, &host)) return 1;
    if (settings_error[0]) ui_show_message(settings_error, true); // the defaults apply, and the file stays as it is
    if (url_code) {
        ui_set_code(url_code);
        if (!strncmp(url_code, SHOW_CODE_FALSE, strlen(SHOW_CODE_FALSE))) ui_set_visible(false);
        free(url_code);
    }
    // Without a sketch the web editor shows a random example and its info window.
    if (options.file_count == 0 && !options.bench_seconds) {
        ui_show_random_example();
        ui_show_info();
    }
    if (gallery_server) ui_enable_gallery();
    if (!settings->code_visible) ui_set_visible(false);
    const char *watched = settings->watch ? sketch : NULL;
    long long watched_time = watched ? modification_time(watched) : 0;
    double next_watch = 0;

    int listener = listen_socket(socket_path);
    if (listener >= 0) fprintf(stderr, "hydra-native: send code with `hydra-native --send < file.js` (%s)\n", socket_path);

    double start = now_ms(), bench_start = 0, frame_js_ms = 0, frame_overlay_ms = 0;
    long bench_frames = 0;
    while (!quit_requested) {
        handle_events();
        struct pollfd poll_listener = { .fd = listener, .events = POLLIN };
        if (listener >= 0 && poll(&poll_listener, 1, 0) > 0) serve_client(listener);

        double now = now_ms();
        if (watched && now >= next_watch) {
            next_watch = now + WATCH_INTERVAL_MS;
            long long current = modification_time(watched);
            if (current != watched_time) {
                watched_time = current;
                ui_file_changed();
            }
        }

        JSValue arguments[1] = { JS_NewFloat64(js, now) };
        call_global("__frame", 1, arguments);
        fetch_poll(fetch_done);
        patch_bay_poll(patch_bay_line);
        run_pending_jobs();
        check_pending_evaluations();
        double after_js = now_ms();
        webgl_release();
        int drawable_width, drawable_height;
        SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
        recorder_frame(now);
        if (share.waiting_for_frame) continue_share(drawable_width, drawable_height);
        if (screencap_requested) {
            screencap_requested = false;
            save_screencap(drawable_width, drawable_height);
        }
        ui_draw(drawable_width, drawable_height, now);
        update_text_input_rect(drawable_width);
        double after_overlay = now_ms();
        if (options.screenshot_path && now - start >= SCREENSHOT_DELAY_MS) {
            if (!write_png(options.screenshot_path, drawable_width, drawable_height)) exit_code = 1;
            quit_requested = true;
        }
        // A hidden window does not swap, so nothing limits the queue of GPU commands. Waiting for the GPU
        // every BENCH_FINISH_FRAMES frames makes the benchmark count rendered frames, not submitted ones.
        if (options.bench_seconds > 0 && (bench_frames % BENCH_FINISH_FRAMES == 0)) glFinish();
        // A hidden Wayland surface never releases its buffers, so a swap could block for ever.
        if (!options.hidden) {
            // Until shortly before the next display refresh, run timers and events instead of a blocking swap.
            if (settings->vsync && frame_period_ms > 0) wait_for_frame(last_swap + frame_period_ms - SWAP_MARGIN_MS, listener);
            SDL_GL_SwapWindow(window);
            double swapped = now_ms();
            if (last_swap > 0) {
                double period = swapped - last_swap;
                if (period > MIN_FRAME_PERIOD_MS && period < MAX_FRAME_PERIOD_MS)
                    frame_period_ms = frame_period_ms > 0 ? frame_period_ms * 0.9 + period * 0.1 : period;
            }
            last_swap = swapped;
        }

        if (options.bench_seconds > 0 && now - start >= BENCH_WARMUP_MS) {
            if (bench_frames == 0) bench_start = now;
            bench_frames++;
            frame_js_ms += after_js - now;
            frame_overlay_ms += after_overlay - after_js;
            double elapsed = now - bench_start;
            if (elapsed >= options.bench_seconds * 1000) {
                glFinish();
                elapsed = now_ms() - bench_start;
                int width, height;
                SDL_GetWindowSizeInPixels(window, &width, &height);
                printf("{\"width\":%d,\"height\":%d,\"vsync\":%s,\"fps\":%.1f,\"jsFrameMeanMs\":%.3f,"
                       "\"overlayFrameMeanMs\":%.3f,\"renderer\":\"%s\",\"differenceBlend\":\"%s\"}\n",
                       width, height, settings->vsync ? "true" : "false", (bench_frames - 1) / (elapsed / 1000),
                       frame_js_ms / bench_frames, frame_overlay_ms / bench_frames,
                       (const char *)glGetString(GL_RENDERER), overlay_difference_method());
                quit_requested = true;
            }
        }
    }

    if (recorder_active()) recorder_stop();
    audio_stop();
    patch_bay_stop();
    fetch_stop_all();
    video_close_all();
    if (listener >= 0) {
        close(listener);
        unlink(socket_path);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
