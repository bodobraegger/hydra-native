#include "render.h"

#include <limits.h>
#include <math.h>
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
#include "sandbox.h"
#include "ui.h"

#define DISPLAY_INTERVAL_MS 100.0 // the window shows the progress this often
#define PROGRESS_INTERVAL_MS 1000.0 // and stderr with `progress` of render_start
#define DEFAULT_FILE_NAME "hydra-native-%Y%m%d-%H%M%S.mp4" // strftime, in the videos folder
#define MAX_ARGUMENTS 48
#define VIDEO_SUFFIX "-video" // with "sources", the video without sound before the mux: <file>-video<extension>
#define MIX_SUFFIX "-sound.f32" // and the raw samples of the mix of the video sources
// The rows from the bottom, converted to YUV with BT.709 in limited range and tagged so, as players assume for HD
// video; without it, ffmpeg converts with BT.601 and tags nothing. ProRes in .mov keeps no range tag.
#define VIDEO_FILTER \
    "vflip,scale=out_color_matrix=bt709:out_range=tv," \
    "setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"

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
    const struct container *container;
    FILE *mix; // "sources": the mix of the video sources, which goes into the video at the end; else NULL
    char video_path[PATH_MAX + 16], mix_path[PATH_MAX + 16];
    struct {
        struct process *decoder;
        int samples;
    } sources[MAX_VIDEO_SOURCES]; // by the handle of the video source
    int shares; // the most video sources that sounded at the same time
    double last_display, last_progress;
    bool progress; // see render_start
    bool succeeded; // the last render wrote its file
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

static void tell(const char *text, bool error) {
    ui_show_message(text, error);
    if (render.progress) fprintf(stderr, "%s\n", text);
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
// All sound of the render goes through muxed_audio, start_decoder and analysis_samples: the mono samples that drive
// `a` in the time span of each frame, and the sound that ffmpeg puts into the video. With "sources", the video
// sources play their sound in the time of the render, from the frame of each mediaOpen to its videoClose (src/
// bindings.c), and analysis_samples writes their mix to a file that mux_sources puts into the video at the end.

// The file whose sound goes into the video, or NULL for a video without sound.
static const char *muxed_audio(const struct settings *settings) {
    return settings->render_audio == RENDER_AUDIO_FILE ? settings->render_audio_file : NULL;
}

// An ffmpeg that writes the sound of `input` as mono float samples at AUDIO_SAMPLE_RATE to *samples: an audio file
// (`playback` NULL), or a video source from its position, at its rate as on tape, so that the pitch follows the
// speed. A video without sound gives no samples; its ffmpeg would only report that, so it reports nothing.
static struct process *start_decoder(const char *input, const struct media_playback *playback, int *samples) {
    char rate[16], start[32], filter[64];
    snprintf(rate, sizeof rate, "%d", AUDIO_SAMPLE_RATE);
    const char *argv[MAX_ARGUMENTS] = { "ffmpeg", "-hide_banner", "-loglevel", playback ? "quiet" : "error" };
    int count = 4;
    if (playback) {
        snprintf(start, sizeof start, "%.3f", playback->start);
        snprintf(filter, sizeof filter, "aresample=%d,asetrate=%ld", AUDIO_SAMPLE_RATE,
                 lround(AUDIO_SAMPLE_RATE * playback->rate));
        if (sandbox_active()) {
            argv[count++] = "-protocol_whitelist";
            argv[count++] = SANDBOX_PROTOCOLS;
        }
        argv[count++] = "-ss";
        argv[count++] = start;
        if (playback->loop) {
            argv[count++] = "-stream_loop";
            argv[count++] = "-1";
        }
    }
    argv[count++] = "-i";
    argv[count++] = input;
    if (playback) {
        argv[count++] = "-af";
        argv[count++] = filter;
    }
    const char *output[] = { "-vn", "-f", "f32le", "-ac", "1", "-ar", rate, "-", NULL };
    for (size_t i = 0; i < SDL_arraysize(output); i++) argv[count++] = output[i];
    return process_start(argv, NULL, samples);
}

static void stop_decoder(struct process **decoder, int samples) {
    if (!*decoder) return;
    stream_close(samples);
    process_stop(*decoder);
    process_free(*decoder);
    *decoder = NULL;
}

// Mixes the next `count` samples of the video sources into `mixed`, in equal shares of the most sources that
// sounded at the same time so far, so that a source does not get louder when another one stops. False while no
// source has sounded.
static bool mix_sources(float *mixed, size_t count) {
    float samples[AUDIO_BUFFER_SIZE];
    int sounding = 0;
    memset(mixed, 0, count * sizeof mixed[0]);
    for (int handle = 0; handle < MAX_VIDEO_SOURCES; handle++) {
        if (!render.sources[handle].decoder) continue;
        // The partial span at the end of a video is lost.
        if (!stream_read_all(render.sources[handle].samples, samples, count * sizeof samples[0])) {
            stop_decoder(&render.sources[handle].decoder, render.sources[handle].samples);
            continue;
        }
        sounding++;
        for (size_t i = 0; i < count; i++) mixed[i] += samples[i];
    }
    render.shares = SDL_max(render.shares, sounding);
    for (size_t i = 0; i < count && render.shares > 0; i++) mixed[i] /= (float)render.shares;
    return render.shares > 0;
}

// The next `count` samples that drive `a`: of the audio file, then silence; with "sources", the mix of the video
// sources once one has sounded. False after the end of the audio file.
static bool analysis_samples(float *samples, size_t count) {
    bool more = render.decoder && stream_read_all(render.samples, samples, count * sizeof samples[0]);
    if (!more) memset(samples, 0, count * sizeof samples[0]);
    float mixed[AUDIO_BUFFER_SIZE];
    if (render.mix && mix_sources(mixed, count)) memcpy(samples, mixed, count * sizeof samples[0]);
    if (render.mix) fwrite(mixed, sizeof mixed[0], count, render.mix);
    return more;
}

void render_source_play(int handle, const char *input, const struct media_playback *playback) {
    if (!render.mix || handle < 0 || handle >= MAX_VIDEO_SOURCES) return;
    render.sources[handle].decoder = start_decoder(input, playback, &render.sources[handle].samples);
}

void render_source_stop(int handle) {
    if (handle >= 0 && handle < MAX_VIDEO_SOURCES)
        stop_decoder(&render.sources[handle].decoder, render.sources[handle].samples);
}

// The video of the encoder with the mix as its sound, into the file of the render. False when ffmpeg fails.
static bool mux_sources(void) {
    char rate[16];
    snprintf(rate, sizeof rate, "%d", AUDIO_SAMPLE_RATE);
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", render.video_path, "-f",
                           "f32le", "-ar", rate, "-ac", "1", "-i", render.mix_path, "-map", "0:v", "-map", "1:a",
                           "-c:v", "copy", "-c:a", render.container->audio_codec, render.path, NULL };
    struct process *muxer = process_start(argv, NULL, NULL);
    bool ok = muxer && process_wait(muxer, PROCESS_WAIT_FOREVER) == 0;
    process_free(muxer);
    return ok;
}

static void close_mix(void) {
    fclose(render.mix);
    render.mix = NULL;
    remove(render.mix_path);
}

// Ends "sources": with `mux` and a source that sounded, the video gets the mix; otherwise it stays without sound.
// False when the mux fails.
static bool finish_sources(bool mux) {
    for (int handle = 0; handle < MAX_VIDEO_SOURCES; handle++) render_source_stop(handle);
    if (!render.mix) return true;
    fflush(render.mix);
    bool sounded = mux && render.shares > 0, muxed = sounded && mux_sources();
    if (!muxed) {
        remove(render.path); // rename does not replace a file on Windows
        rename(render.video_path, render.path);
    }
    remove(render.video_path);
    close_mix();
    return muxed || !sounded;
}

// The frames come as raw RGBA on the standard input, with the rows from the bottom (VIDEO_FILTER); the audio file is
// the second input, and the video ends with the shorter of the two.
static int start_encoder(const struct container *container, const char *audio, const char *output) {
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
    argv[count++] = VIDEO_FILTER;
    for (const char *const *option = container->video_options; *option; option++) argv[count++] = *option;
    argv[count++] = output;
    argv[count] = NULL;
    int input;
    render.encoder = process_start(argv, &input, NULL);
    return render.encoder ? input : -1;
}

static void dispatch_resize(void) {
    JSValue arguments[2] = { JS_NewString(js, "resize"), JS_NewObject(js) };
    engine_call("__dispatchEvent", 2, arguments);
}

static void set_clock(JSValue clock) { engine_call("__setRenderClock", 1, &clock); }

void render_start(const struct settings *settings, double clock_ms, bool progress) {
    if (render.active) return;
    render.progress = progress;
    render.succeeded = false;
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
        return tell(message, true);
    }
    if (!process_find_program("ffmpeg")) return tell("render to file needs ffmpeg in PATH", true);
    if (settings->render_width > max_size || settings->render_height > max_size) {
        snprintf(message, sizeof message, "render: the GPU renders at most %dx%d", max_size, max_size);
        return tell(message, true);
    }
    if (settings->render_audio == RENDER_AUDIO_FILE && !audio)
        return tell("render audio: \"file\" needs an audio file (settings menu, render)", true);
    if (audio && access(audio, R_OK) != 0) {
        snprintf(message, sizeof message, "render: cannot read the audio file %s", audio);
        return tell(message, true);
    }
    render.width = settings->render_width;
    render.height = settings->render_height;
    render.fps = settings->render_fps;
    render.frame = 0;
    render.frame_count = lround(settings->render_seconds * render.fps);
    render.start_ms = clock_ms;
    render.samples_fed = 0;
    render.muxed = muxed != NULL;
    render.container = container;
    render.shares = 0;
    render.last_display = render.last_progress = 0;
    const char *output = render.path;
    if (settings->render_audio == RENDER_AUDIO_SOURCES) {
        snprintf(render.video_path, sizeof render.video_path, "%s" VIDEO_SUFFIX "%s", render.path,
                 container->extension);
        snprintf(render.mix_path, sizeof render.mix_path, "%s" MIX_SUFFIX, render.path);
        if (!(render.mix = fopen(render.mix_path, "wb"))) {
            snprintf(message, sizeof message, "render: cannot write %s", render.mix_path);
            return tell(message, true);
        }
        output = render.video_path;
    }
    if (audio && !(render.decoder = start_decoder(audio, NULL, &render.samples))) {
        if (render.mix) close_mix();
        return tell("render: cannot start ffmpeg", true);
    }
    int input = start_encoder(container, muxed, output);
    if (input < 0) {
        stop_decoder(&render.decoder, render.samples);
        if (render.mix) close_mix();
        return tell("render: cannot start ffmpeg", true);
    }
    glGenRenderbuffers(1, &render.renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, render.renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, render.width, render.height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &render.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, render.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, render.renderbuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!encoder_start(render.encoder, input, render.framebuffer, render.width, render.height)) {
        stream_close(input);
        process_kill(render.encoder);
        process_free(render.encoder);
        render.encoder = NULL;
        stop_decoder(&render.decoder, render.samples);
        if (render.mix) close_mix();
        glDeleteFramebuffers(1, &render.framebuffer);
        glDeleteRenderbuffers(1, &render.renderbuffer);
        render.framebuffer = render.renderbuffer = 0;
        remove(render.path);
        return tell("render: no memory for the frames", true);
    }
    render.active = true;
    SDL_GL_SetSwapInterval(0); // the window shows the progress only, and a swap must not wait for the display
    audio_feed_start();
    dispatch_resize(); // the sketch and hydra take the render size
}

bool render_active(void) { return render.active; }

bool render_succeeded(void) { return render.succeeded; }

static void finish(enum render_end end) {
    if (end == RENDER_CANCELED) process_kill(render.encoder);
    bool ok = encoder_stop() && end != RENDER_FAILED; // it frees the encoder
    render.encoder = NULL;
    stop_decoder(&render.decoder, render.samples);
    bool sources_muted = render.mix && render.shares == 0; // "sources" without a video source with sound
    ok = finish_sources(ok && end == RENDER_DONE) && ok;
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
                 sources_muted ? "; render audio: sources needs a video with sound; the file is muted" : "");
    }
    render.succeeded = ok && end == RENDER_DONE;
    tell(message, end != RENDER_CANCELED && !ok);
}

// Feeds the sound up to the time of `frame`. False when the render ended instead, because the audio file ended.
static bool feed(long frame) {
    long long wanted = (long long)frame * AUDIO_SAMPLE_RATE / render.fps;
    float samples[AUDIO_BUFFER_SIZE];
    while ((render.decoder || render.mix) && render.samples_fed < wanted) {
        size_t count = (size_t)SDL_min(wanted - render.samples_fed, (long long)AUDIO_BUFFER_SIZE);
        if (!analysis_samples(samples, count) && render.muxed) {
            finish(RENDER_DONE); // the video ends with its sound
            return false;
        }
        audio_feed(samples, count);
        render.samples_fed += (long long)count;
    }
    return true;
}

bool render_frame_begin(double *clock_ms) {
    if (!feed(render.frame)) return false;
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
    if (render.frame >= render.frame_count) {
        if (feed(render.frame)) finish(RENDER_DONE); // the sound of the last frame
        return;
    }
    double now = now_ms();
    if (render.progress && now - render.last_progress >= PROGRESS_INTERVAL_MS) {
        render.last_progress = now;
        fprintf(stderr, "render %ld / %ld frames\n", render.frame, render.frame_count);
    }
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
