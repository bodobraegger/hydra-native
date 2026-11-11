// hydra-native: runs hydra-synth (bundled with a regl shim) in QuickJS and renders with GLES in an SDL window.
#define _GNU_SOURCE
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "stb_image.h"

#include "app.h"
#include "bindings.h"
#include "code_socket.h"
#include "engine.h"
#include "native.h"
#include "overlay.h"
#include "settings.h"
#include "snapshot.h"
#include "thumbnails.h"
#include "ui.h"
#include "window.h"

#define BUNDLE_FILE_NAME "hydra-bundle.js"
#define WINDOW_ICON_FILE "../assets/icon/hicolor/48x48/apps/hydra-native.png"
#define WATCH_INTERVAL_MS 250
#define BENCH_WARMUP_MS 3000
#define BENCH_FINISH_FRAMES 30
#define SCREENSHOT_DELAY_MS 1500

#define DEFAULT_WINDOW_WIDTH 1280
#define DEFAULT_WINDOW_HEIGHT 720

struct options {
    const char *files[32];
    int file_count;
    int width, height;
    const char *screenshot_path;
    double bench_seconds;
    bool hidden;
    bool send;
};

static struct settings settings; // of the settings file, as they are at the start

#define SWAP_MARGIN_MS 3.0 // time left for the swap after the timers
#define MIN_FRAME_PERIOD_MS 4.0
#define MAX_FRAME_PERIOD_MS 50.0
static double frame_period_ms, last_swap;
SDL_Window *window;
char executable_path_directory[PATH_MAX];
static volatile sig_atomic_t quit_requested; // also set by SIGINT and SIGTERM
static int exit_code;

void app_quit(int code) {
    exit_code = code;
    quit_requested = true;
}

static void patch_bay_line(const char *text) {
    JSValue arguments[1] = { JS_NewString(js, text) };
    engine_call("__patchBay", 1, arguments);
}

// A Wayland request holds at most 4096 bytes, and the compositor disconnects a client that sends more. The URL
// of a long sketch is longer, so the title gets its start only (ASCII, so any byte is a boundary).
#define MAX_TITLE 1024

static void set_window_title(const char *title) {
    char text[MAX_TITLE];
    snprintf(text, sizeof text, "%s", title);
    SDL_SetWindowTitle(window, text);
}

// Tells the input method where the cursor is, so that its candidate window opens there, as in the browser.
static void update_text_input_rect(int drawable_width) {
    static SDL_Rect last;
    int x, y, width, height, window_width, window_height;
    if (!ui_text_input_rect(&x, &y, &width, &height)) return;
    SDL_GetWindowSize(window, &window_width, &window_height);
    float scale = drawable_width > 0 ? (float)window_width / (float)drawable_width : 1;
    SDL_Rect rect = { (int)((float)x * scale), (int)((float)y * scale), (int)((float)width * scale),
                      (int)((float)height * scale) };
    if (rect.x == last.x && rect.y == last.y && rect.w == last.w && rect.h == last.h) return;
    last = rect;
    SDL_SetTextInputArea(window, &rect, rect.w);
}

static void fetch_done(int id, bool ok, const char *data, size_t length) {
    if (thumbnails_fetch_done(id, ok, data, length)) return;
    bindings_fetch_done(js, id, ok, data, length);
}

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
    engine_call("__dispatchEvent", 2, arguments);
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
    engine_call("__dispatchEvent", 2, arguments);
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
                ui_mouse_button(event.button.x * scale, event.button.y * scale,
                                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN, event.button.clicks, SDL_GetModState());
            dispatch_mouse(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mousedown" : "mouseup", (int)event.button.x,
                           (int)event.button.y);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            JSValue arguments[2] = { JS_NewString(js, "resize"), JS_NewObject(js) };
            engine_call("__dispatchEvent", 2, arguments);
            break;
        }
        }
    }
}

#if defined(__APPLE__)
#define st_mtim st_mtimespec // the name of the same field on macOS
#endif

static long long modification_time(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 ? info.st_mtim.tv_sec * 1000000000LL + info.st_mtim.tv_nsec : 0;
}

// ---------------------------------------------------------------- setup and main loop

static void usage(void) {
    fprintf(stderr,
            "usage: hydra-native [options] [file.js | https://hydra.ojack.xyz/?code=... ...]\n"
            "       hydra-native --send < code.js\n"
            "  The last file opens in the code overlay; Ctrl+S saves it. F1 shows all keys.\n"
            "  The settings menu (Ctrl+,) sets everything else and saves it in settings.json.\n"
            "For scripts and tests:\n"
            "  --send             send the code on stdin to a running hydra-native and print its answer\n"
            "  --socket PATH      code socket (default $XDG_RUNTIME_DIR/%s; also \"socket\" in settings.json)\n"
            "  --hidden           do not show the window\n"
            "  --size WxH         window size (default %dx%d)\n"
            "  --bench SECONDS    measure frame rate after a warm-up, print JSON and exit\n"
            "  --screenshot FILE  write the window content with the code as PNG after %d ms and exit\n",
            CODE_SOCKET_FILE_NAME, DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT, SCREENSHOT_DELAY_MS);
}

static bool parse_options(int argc, char **argv, struct options *options) {
    *options = (struct options){ .width = DEFAULT_WINDOW_WIDTH, .height = DEFAULT_WINDOW_HEIGHT };
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        bool has_value = i + 1 < argc;
        if (!strcmp(argument, "--hidden")) options->hidden = true;
        else if (!strcmp(argument, "--socket") && has_value)
            snprintf(settings.socket, sizeof settings.socket, "%s", argv[++i]);
        else if (!strcmp(argument, "--screenshot") && has_value) options->screenshot_path = argv[++i];
        else if (!strcmp(argument, "--send")) options->send = true;
        else if (!strcmp(argument, "--bench") && has_value) options->bench_seconds = atof(argv[++i]);
        else if (!strcmp(argument, "--size") && has_value) {
            if (sscanf(argv[++i], "%dx%d", &options->width, &options->height) != 2) return false;
        } else if (argument[0] == '-') return false;
        else if (options->file_count < (int)(sizeof options->files / sizeof options->files[0]))
            options->files[options->file_count++] = argument;
    }
    return true;
}

static bool create_window(const struct options *options) {
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options->hidden) flags |= SDL_WINDOW_HIDDEN;
    if (settings.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window = window_create("hydra-native", options->width, options->height, flags, settings.vsync);
    if (!window) return false;
    programs_start();
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

// The folder of the program, which holds the bundle and the capture tool. In a macOS app, SDL_GetBasePath
// gives Contents/Resources, so macOS asks dyld for the program itself (Contents/MacOS).
static bool executable_directory(char *directory, size_t size) {
#if defined(__APPLE__)
    char path[PATH_MAX], resolved[PATH_MAX];
    uint32_t length = sizeof path;
    if (_NSGetExecutablePath(path, &length) != 0 || !realpath(path, resolved)) return false;
    *strrchr(resolved, '/') = '\0';
    snprintf(directory, size, "%s", resolved);
#else
    const char *base = SDL_GetBasePath(); // ends with a slash
    if (!base) return false;
    snprintf(directory, size, "%.*s", (int)strlen(base) - 1, base);
#endif
    return true;
}

static bool load_bundle(const char *directory) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%s", directory, BUNDLE_FILE_NAME);
    char *error = engine_evaluate_file(path);
    if (error) {
        fprintf(stderr, "%s\n", error);
        free(error);
        return false;
    }
    return true;
}

#define SHOW_CODE_FALSE "// show-code: false"

static bool is_sketch_url(const char *argument) {
    bool url = !strncmp(argument, "http://", 7) || !strncmp(argument, "https://", 8) || argument[0] == '?';
    return url && (strstr(argument, "code=") || strstr(argument, "sketch_id="));
}

// Waits until `deadline` (ms of now_ms()), runs each timer at its time, and handles input and socket code meanwhile.
static void wait_for_frame(double deadline, int listener) {
    for (;;) {
        double next = engine_run_timers(), now = now_ms();
        if (now >= deadline || quit_requested) return;
        double until = next >= 0 && next < deadline ? next : deadline;
        int wait = (int)(until - now);
        if (wait > 0 && SDL_WaitEventTimeout(NULL, wait)) handle_events();
        code_socket_poll(listener);
    }
}

static bool is_fullscreen(void) { return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0; }

static void set_vsync(bool vsync) { SDL_GL_SetSwapInterval(vsync ? 1 : 0); }

// The file dialogs of SDL (the xdg portal on Linux) answer on any thread; the main loop takes the answer.
static struct {
    SDL_Mutex *lock;
    char path[PATH_MAX];
    bool ready, save;
} chosen_file;

static void SDLCALL file_chosen(void *data, const char *const *files, int filter) {
    if (!files || !files[0]) return; // canceled, or an error
    SDL_LockMutex(chosen_file.lock);
    snprintf(chosen_file.path, sizeof chosen_file.path, "%s", files[0]);
    chosen_file.save = data != NULL;
    chosen_file.ready = true;
    SDL_UnlockMutex(chosen_file.lock);
}

static void choose_file(bool save) {
    static const SDL_DialogFileFilter filters[] = { { "JavaScript (hydra sketch)", "js" } };
    if (!chosen_file.lock) chosen_file.lock = SDL_CreateMutex();
    if (save) SDL_ShowSaveFileDialog(file_chosen, (void *)1, window, filters, 1, NULL);
    else SDL_ShowOpenFileDialog(file_chosen, NULL, window, filters, 1, NULL, false);
}

static void take_chosen_file(void) {
    if (!chosen_file.lock) return;
    SDL_LockMutex(chosen_file.lock);
    bool ready = chosen_file.ready, save = chosen_file.save;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", chosen_file.path);
    chosen_file.ready = false;
    SDL_UnlockMutex(chosen_file.lock);
    if (ready && save) ui_save_as(path);
    else if (ready) ui_open_file(path);
}

static void toggle_fullscreen(void) { SDL_SetWindowFullscreen(window, !is_fullscreen()); }

static void request_quit(void) { quit_requested = true; }

static void handle_quit_signal(int signal_number) { quit_requested = true; }

int main(int argc, char **argv) {
    // The settings file first: the socket and the window need it.
    char settings_file[PATH_MAX], settings_error[SETTINGS_TEXT] = "";
    settings_defaults(&settings);
    if (!settings_path(settings_file, sizeof settings_file)) settings_file[0] = '\0';
    else if (!settings_load(&settings, settings_file, settings_error, sizeof settings_error))
        settings_defaults(&settings);
    struct options options;
    if (!parse_options(argc, argv, &options)) {
        usage();
        return 2;
    }
    char socket_path[PATH_MAX];
    if (!code_socket_path(socket_path, sizeof socket_path, settings.socket)) return 2;
    if (options.send) return code_socket_send(socket_path);

    signal(SIGPIPE, SIG_IGN);
    audio_set_gain(settings.audio_gain);
    share_set_server(settings.gallery_server);
    if (!create_window(&options)) return 1;
    // After SDL, which installs handlers of its own: the first SIGINT or SIGTERM ends the loop, so that the child
    // processes stop; a second one ends the process at once.
    struct sigaction quit_action = { .sa_handler = handle_quit_signal, .sa_flags = SA_RESETHAND };
    sigaction(SIGINT, &quit_action, NULL);
    sigaction(SIGTERM, &quit_action, NULL);

    engine_start(argc, argv);
    JSValue global = JS_GetGlobalObject(js);
    JSValue native = bindings_create(js, settings.audio_source[0] ? settings.audio_source : NULL);
    char directory[PATH_MAX];
    if (!executable_directory(directory, sizeof directory)) return 1;
    snprintf(executable_path_directory, sizeof executable_path_directory, "%s", directory);
    JS_SetPropertyStr(js, native, "directory", JS_NewString(js, directory));
    JS_SetPropertyStr(js, native, "platform", JS_NewString(js, SDL_GetPlatform())); // Linux, macOS, Windows
    JS_SetPropertyStr(js, native, "shaderCompile", JS_NewString(js, programs_method()));
    JS_SetPropertyStr(js, native, "language", settings.language[0] ? JS_NewString(js, settings.language) : JS_NULL);
    JS_SetPropertyStr(js, native, "l10nUrl", settings.l10n_url[0] ? JS_NewString(js, settings.l10n_url) : JS_NULL);
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
            char *code = engine_call_text("__codeFromUrl", argument);
            if (!code) {
                fprintf(stderr, "cannot open %s\n", argument);
                continue;
            }
            error = engine_evaluate(code, strlen(code), argument);
            if (argument == sketch) url_code = code;
            else free(code);
        } else if (argument != sketch || access(sketch, F_OK) == 0) {
            error = engine_evaluate_file(argument);
        }
        if (error) fprintf(stderr, "%s\n", error);
        free(error);
    }
    if (url_code) {
        sketch = NULL;
        set_window_title(options.files[options.file_count - 1]);
    }
    static const struct ui_host host = {
        .evaluate = engine_evaluate,
        .toggle_fullscreen = toggle_fullscreen,
        .is_fullscreen = is_fullscreen,
        .quit = request_quit,
        .call = engine_call_text,
        .screencap = snapshot_request_screencap,
        .share = share_start,
        .set_title = set_window_title,
        .set_vsync = set_vsync,
        .audio_devices = audio_devices,
        .set_audio = audio_set_device,
        .set_audio_gain = audio_set_gain,
        .choose_file = choose_file,
        .set_server = share_set_server,
    };
    char emoji_font[PATH_MAX + 64];
    snprintf(emoji_font, sizeof emoji_font, "%s/../vendor/fonts/NotoEmoji.ttf", directory);
    if (!ui_init(&settings, settings_file[0] ? settings_file : NULL, emoji_font, sketch, &host)) return 1;
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
    if (share_server()[0]) ui_enable_gallery();
    if (!settings.code_visible) ui_set_visible(false);
    // "watch" of the settings menu applies at once.
    // The open file of the overlay can change through the "file" row and Ctrl+S, so the check follows it.
    char watched[PATH_MAX] = "";
    long long watched_time = 0;
    double next_watch = 0;

    int listener = code_socket_listen(socket_path);
    if (listener >= 0)
        fprintf(stderr, "hydra-native: send code with `hydra-native --send < file.js` (%s)\n", socket_path);

    double start = now_ms(), bench_start = 0, frame_js_ms = 0, frame_overlay_ms = 0;
    long bench_frames = 0;
    while (!quit_requested) {
        handle_events();
        code_socket_poll(listener);

        double now = now_ms();
        take_chosen_file();
        const char *open_file = ui_file_path();
        if (open_file && now >= next_watch) {
            next_watch = now + WATCH_INTERVAL_MS;
            long long current = modification_time(open_file);
            if (strcmp(watched, open_file) != 0) {
                snprintf(watched, sizeof watched, "%s", open_file);
                watched_time = current;
            } else if (current != watched_time) {
                watched_time = current;
                if (ui_settings()->watch) ui_file_changed();
            }
        }

        JSValue arguments[1] = { JS_NewFloat64(js, now) };
        engine_call("__frame", 1, arguments);
        fetch_poll(fetch_done);
        patch_bay_poll(patch_bay_line);
        engine_run_pending_jobs();
        engine_check_pending_evaluations();
        double after_js = now_ms();
        int drawable_width, drawable_height;
        SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
        recorder_frame(now);
        snapshot_frame(drawable_width, drawable_height);
        ui_draw(drawable_width, drawable_height, now);
        update_text_input_rect(drawable_width);
        double after_overlay = now_ms();
        if (options.screenshot_path && now - start >= SCREENSHOT_DELAY_MS) {
            if (!snapshot_write_png(options.screenshot_path, drawable_width, drawable_height)) exit_code = 1;
            quit_requested = true;
        }
        // A hidden window does not swap, so nothing limits the queue of GPU commands. Waiting for the GPU
        // every BENCH_FINISH_FRAMES frames makes the benchmark count rendered frames, not submitted ones.
        if (options.bench_seconds > 0 && (bench_frames % BENCH_FINISH_FRAMES == 0)) glFinish();
        // A hidden Wayland surface never releases its buffers, so a swap could block for ever.
        if (!options.hidden) {
            // Until shortly before the next display refresh, run timers and events instead of a blocking swap.
            if (settings.vsync && frame_period_ms > 0)
                wait_for_frame(last_swap + frame_period_ms - SWAP_MARGIN_MS, listener);
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
                       width, height, settings.vsync ? "true" : "false", (bench_frames - 1) / (elapsed / 1000),
                       frame_js_ms / bench_frames, frame_overlay_ms / bench_frames,
                       (const char *)glGetString(GL_RENDERER), overlay_difference_method());
                quit_requested = true;
            }
        }
    }

    if (recorder_active()) recorder_stop();
    patch_bay_stop();
    fetch_stop_all();
    video_close_all();
    if (listener >= 0) {
        close(listener);
        unlink(socket_path);
    }
    programs_stop();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
