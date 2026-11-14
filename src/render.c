#include "render.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "app.h"
#include "audio_processing.h"
#include "engine.h"
#include "native.h"
#include "process.h"
#include "ui.h"

#define DISPLAY_INTERVAL_MS 100.0 // the window shows the progress this often
#define DEFAULT_FILE_NAME "hydra-native-%Y%m%d-%H%M%S.mp4" // strftime, in the videos folder
#define MAX_ARGUMENTS 48

// The codecs of each file extension; `audio_codec` encodes the audio file.
static const struct container {
    const char *extension;
    const char *video_options[10];
    const char *audio_codec;
} containers[] = {
    { ".mp4", { "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", NULL }, "aac" },
    { ".mov", { "-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le", NULL }, "pcm_s16le" }, // 422 HQ
    { ".webm", { "-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "0", "-row-mt", "1", NULL }, "libopus" },
};

enum render_end { RENDER_DONE, RENDER_CANCELED, RENDER_FAILED };

static struct {
    bool active;
    char path[PATH_MAX];
    int width, height, fps;
    long frame, frame_count;
    double start_ms; // the clock of the first frame
    GLuint framebuffer, renderbuffer;
    struct process *encoder, *decoder; // ffmpeg, and the ffmpeg that decodes the audio file (or NULL)
    int samples; // the output of the decoder: mono float samples at AUDIO_SAMPLE_RATE
    long long samples_fed;
    bool muxed; // the video has the sound of the audio file, so it ends with that file
    bool sources_muted; // "sources" without a video source with sound
    double last_display;
} render;

static const struct container *find_container(const char *path) {
    size_t length = strlen(path);
    for (size_t i = 0; i < SDL_arraysize(containers); i++) {
        size_t extension = strlen(containers[i].extension);
        if (length > extension && !SDL_strcasecmp(path + length - extension, containers[i].extension))
            return &containers[i];
    }
    return NULL;
}

static void default_path(char *path, size_t size) {
    const char *videos = SDL_GetUserFolder(SDL_FOLDER_VIDEOS); // ends with a separator
    if (videos) SDL_CreateDirectory(videos);
    char name[64];
    time_t seconds = time(NULL);
    strftime(name, sizeof name, DEFAULT_FILE_NAME, localtime(&seconds));
    snprintf(path, size, "%s%s", videos ? videos : "", name);
}

// ---------------------------------------------------------------- the sound of the render
// All sound of the render goes through the next three functions: the mono samples that drive `a` in the time span
// of each frame, and the sound that ffmpeg puts into the video. Only the audio file gives sound now; the choice
// "sources" (the sound of the video sources of the sketch, mixed in equal shares, in the time of the picture) goes
// here when the video sources have sound.

// The file whose sound goes into the video, or NULL for a video without sound.
static const char *muxed_audio(const struct settings *settings) {
    return settings->render_audio == RENDER_AUDIO_FILE ? settings->render_audio_file : NULL;
}

static bool start_decoder(const char *audio) {
    char rate[16];
    snprintf(rate, sizeof rate, "%d", AUDIO_SAMPLE_RATE);
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", audio, "-vn", "-f", "f32le", "-ac",
                           "1", "-ar", rate, "-", NULL };
    render.decoder = process_start(argv, NULL, &render.samples);
    return render.decoder != NULL;
}

// The next `count` samples that drive `a`: of the audio file, then silence. False after the end of the file.
static bool analysis_samples(float *samples, size_t count) {
    if (stream_read_all(render.samples, samples, count * sizeof samples[0])) return true;
    memset(samples, 0, count * sizeof samples[0]);
    return false;
}

// The frames come as raw RGBA on the standard input, with the rows from the bottom; the audio file is the second
// input, and the video ends with the shorter of the two.
static int start_encoder(const struct container *container, const char *audio) {
    char size[32], rate[16];
    snprintf(size, sizeof size, "%dx%d", render.width, render.height);
    snprintf(rate, sizeof rate, "%d", render.fps);
    const char *argv[MAX_ARGUMENTS] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                                        "-pix_fmt", "rgba", "-s", size, "-r", rate, "-i", "-" };
    int count = 15;
    if (audio) {
        const char *audio_options[] = { "-i", audio, "-map", "0:v", "-map", "1:a", "-c:a", container->audio_codec,
                                        "-shortest" };
        for (size_t i = 0; i < SDL_arraysize(audio_options); i++) argv[count++] = audio_options[i];
    }
    argv[count++] = "-vf";
    argv[count++] = "vflip";
    for (const char *const *option = container->video_options; *option; option++) argv[count++] = *option;
    argv[count++] = render.path;
    argv[count] = NULL;
    int input;
    render.encoder = process_start(argv, &input, NULL);
    return render.encoder ? input : -1;
}

static void stop_decoder(void) {
    if (!render.decoder) return;
    stream_close(render.samples);
    process_stop(render.decoder);
    process_free(render.decoder);
    render.decoder = NULL;
}

static void dispatch_resize(void) {
    JSValue arguments[2] = { JS_NewString(js, "resize"), JS_NewObject(js) };
    engine_call("__dispatchEvent", 2, arguments);
}

static void set_clock(JSValue clock) { engine_call("__setRenderClock", 1, &clock); }

void render_start(const struct settings *settings, double clock_ms) {
    if (render.active) return;
    char message[PATH_MAX + 64];
    if (settings->render_file[0]) snprintf(render.path, sizeof render.path, "%s", settings->render_file);
    else default_path(render.path, sizeof render.path);
    const char *audio = settings->render_audio_file[0] ? settings->render_audio_file : NULL;
    const char *muxed = muxed_audio(settings);
    const struct container *container = find_container(render.path);
    GLint max_size;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    if (!container) {
        snprintf(message, sizeof message, "render: %s needs the extension .mp4, .mov or .webm", render.path);
        return ui_show_message(message, true);
    }
    if (!process_find_program("ffmpeg")) return ui_show_message("render to file needs ffmpeg in PATH", true);
    if (settings->render_width > max_size || settings->render_height > max_size) {
        snprintf(message, sizeof message, "render: the GPU renders at most %dx%d", max_size, max_size);
        return ui_show_message(message, true);
    }
    if (settings->render_audio == RENDER_AUDIO_FILE && !audio)
        return ui_show_message("render audio: \"file\" needs an audio file (settings menu, render)", true);
    if (audio && access(audio, R_OK) != 0) {
        snprintf(message, sizeof message, "render: cannot read the audio file %s", audio);
        return ui_show_message(message, true);
    }
    render.width = settings->render_width;
    render.height = settings->render_height;
    render.fps = settings->render_fps;
    render.frame = 0;
    render.frame_count = (long)settings->render_seconds * render.fps;
    render.start_ms = clock_ms;
    render.samples_fed = 0;
    render.muxed = muxed != NULL;
    render.sources_muted = settings->render_audio == RENDER_AUDIO_SOURCES;
    render.last_display = 0;
    if (audio && !start_decoder(audio)) return ui_show_message("render: cannot start ffmpeg", true);
    int input = start_encoder(container, muxed);
    if (input < 0) {
        stop_decoder();
        return ui_show_message("render: cannot start ffmpeg", true);
    }
    glGenRenderbuffers(1, &render.renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, render.renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, render.width, render.height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &render.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, render.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, render.renderbuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    encoder_start(render.encoder, input, render.framebuffer, render.width, render.height);
    render.active = true;
    SDL_GL_SetSwapInterval(0); // the window shows the progress only, and a swap must not wait for the display
    audio_feed_start();
    dispatch_resize(); // the sketch and hydra take the render size
}

bool render_active(void) { return render.active; }

static void finish(enum render_end end) {
    if (end == RENDER_CANCELED) process_kill(render.encoder);
    bool ok = encoder_stop() && end != RENDER_FAILED; // it frees the encoder
    render.encoder = NULL;
    stop_decoder();
    glDeleteFramebuffers(1, &render.framebuffer);
    glDeleteRenderbuffers(1, &render.renderbuffer);
    render.framebuffer = render.renderbuffer = 0;
    render.active = false;
    audio_feed_stop();
    set_clock(JS_NULL);
    dispatch_resize();
    SDL_GL_SetSwapInterval(ui_settings()->vsync ? 1 : 0);
    char message[PATH_MAX + 128];
    if (end == RENDER_CANCELED) {
        remove(render.path);
        snprintf(message, sizeof message, "render canceled");
    } else {
        snprintf(message, sizeof message, ok ? "rendered %s%s" : "render failed: %s (see the ffmpeg output)%s",
                 render.path,
                 render.sources_muted ? "; render audio: sources needs a video with sound; the file is muted" : "");
    }
    ui_show_message(message, end != RENDER_CANCELED && !ok);
}

bool render_frame_begin(double *clock_ms) {
    long long wanted = (long long)render.frame * AUDIO_SAMPLE_RATE / render.fps;
    float samples[AUDIO_BUFFER_SIZE];
    while (render.decoder && render.samples_fed < wanted) {
        size_t count = (size_t)SDL_min(wanted - render.samples_fed, (long long)AUDIO_BUFFER_SIZE);
        if (!analysis_samples(samples, count) && render.muxed) {
            finish(RENDER_DONE); // the video ends with its sound
            return false;
        }
        audio_feed(samples, count);
        render.samples_fed += (long long)count;
    }
    *clock_ms = render.start_ms + (double)render.frame * 1000.0 / render.fps;
    set_clock(JS_NewFloat64(js, *clock_ms));
    return true;
}

// The frame in the window, stretched as the sketch shows in it, with the progress in the bar.
static void display(void) {
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, render.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, render.width, render.height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    char text[128];
    snprintf(text, sizeof text, "render %ld / %ld frames   (Escape: cancel)", render.frame, render.frame_count);
    ui_draw_status(width, height, text);
    // A hidden Wayland surface never releases its buffers, so a swap could block for ever.
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)) SDL_GL_SwapWindow(window);
}

void render_frame_end(void) {
    encoder_frame();
    render.frame++;
    int status = process_wait(render.encoder, 0); // ffmpeg ends early after an error, or at the end of the audio
    if (status != PROCESS_RUNNING) return finish(status == 0 ? RENDER_DONE : RENDER_FAILED);
    if (render.frame >= render.frame_count) return finish(RENDER_DONE);
    double now = now_ms();
    if (now - render.last_display < DISPLAY_INTERVAL_MS) return;
    render.last_display = now;
    display();
}

void render_cancel(void) {
    if (render.active) finish(RENDER_CANCELED);
}

GLuint render_framebuffer(void) { return render.framebuffer; }

bool render_size(int *width, int *height) {
    if (!render.active) return false;
    *width = render.width;
    *height = render.height;
    return true;
}
