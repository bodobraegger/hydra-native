#include "snapshot.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

// stb_image_write.h 1.16 (public domain or MIT, see THIRD_PARTY.md) writes the PNG files.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "engine.h"
#include "settings.h"
#include "ui.h"

// The window as opaque RGBA rows from the bottom (malloc'd), as glReadPixels gives them.
static unsigned char *read_window_pixels(int width, int height) {
    unsigned char *pixels = malloc((size_t)width * (size_t)height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (size_t i = 3; i < (size_t)width * (size_t)height * 4; i += 4) pixels[i] = 255;
    return pixels;
}

bool snapshot_write_png(const char *path, int width, int height) {
    unsigned char *pixels = read_window_pixels(width, height);
    stbi_flip_vertically_on_write(1);
    bool ok = stbi_write_png(path, width, height, 4, pixels, width * 4) != 0;
    free(pixels);
    return ok;
}

void snapshot_download_base(char *base, size_t size) {
    time_t seconds = time(NULL);
    struct tm *local = localtime(&seconds);
    const char *downloads = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS); // ends with a slash
    char directory[PATH_MAX];
    if (downloads && access(downloads, W_OK) == 0)
        snprintf(directory, sizeof directory, "%.*s", (int)strlen(downloads) - 1, downloads);
    else snprintf(directory, sizeof directory, ".");
    snprintf(base, size, "%s/hydra-%d-%d-%d-%d.%d.%d", directory, local->tm_year + 1900, local->tm_mon + 1,
             local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
}

static bool screencap_requested;

void snapshot_request_screencap(void) { screencap_requested = true; }

static void save_screencap(int width, int height) {
    char base[PATH_MAX + 64];
    snapshot_download_base(base, sizeof base);
    char image_path[PATH_MAX + 80], code_path[PATH_MAX + 80], message[PATH_MAX + 128];
    snprintf(image_path, sizeof image_path, "%s.png", base);
    snprintf(code_path, sizeof code_path, "%s.js", base);
    bool ok = snapshot_write_png(image_path, width, height);
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

static char gallery_server_url[SETTINGS_TEXT];

void share_set_server(const char *url) { snprintf(gallery_server_url, sizeof gallery_server_url, "%s", url); }

const char *share_server(void) { return gallery_server_url; }

// Sharing: the screenshot comes from the next frame, then __shareSketch (js/editor-actions.js) sends the
// requests of the web editor with fetch() and answers through native.shareDone().
static struct {
    bool waiting_for_frame, sending;
    char *code;
    char name[256];
} share;

bool share_start(const char *code, const char *name) {
    if (!gallery_server_url[0] || share.sending || share.waiting_for_frame) return false;
    free(share.code);
    share.code = strdup(code);
    snprintf(share.name, sizeof share.name, "%s", name);
    share.waiting_for_frame = true;
    return true;
}

void share_done(bool ok, const char *text) {
    share.sending = false;
    ui_share_done(ok, text);
}

static void continue_share(int width, int height) {
    share.waiting_for_frame = false;
    unsigned char *pixels = read_window_pixels(width, height);
    stbi_flip_vertically_on_write(1);
    int length;
    unsigned char *png = stbi_write_png_to_mem(pixels, width * 4, width, height, 4, &length);
    free(pixels);
    if (!png) return ui_share_done(false, "cannot encode the screenshot");
    share.sending = true;
    JSValue arguments[4] = { JS_NewString(js, gallery_server_url), JS_NewString(js, share.code),
                             JS_NewArrayBufferCopy(js, png, (size_t)length), JS_NewString(js, share.name) };
    free(png);
    engine_call("__shareSketch", 4, arguments);
}

void snapshot_frame(int width, int height) {
    if (share.waiting_for_frame) continue_share(width, height);
    if (screencap_requested) {
        screencap_requested = false;
        save_screencap(width, height);
    }
}
