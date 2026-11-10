// Thumbnails of the extensions panel: downloaded with curl (src/fetch.c), decoded with stb_image.h 2.30
// (public domain or MIT, see THIRD_PARTY.md) and kept as GL textures.
#include <stdio.h>
#include <string.h>

#include <GLES3/gl3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "native.h"
#include "thumbnails.h"

#define MAX_THUMBNAILS 128
#define MAX_URL 512

enum thumbnail_state { THUMBNAIL_LOADING, THUMBNAIL_READY, THUMBNAIL_FAILED };

static struct {
    char url[MAX_URL];
    int fetch;
    enum thumbnail_state state;
    GLuint texture;
} thumbnails[MAX_THUMBNAILS];
static int thumbnail_count;

unsigned thumbnail_texture(const char *url) {
    for (int i = 0; i < thumbnail_count; i++)
        if (!strcmp(thumbnails[i].url, url)) return thumbnails[i].state == THUMBNAIL_READY ? thumbnails[i].texture : 0;
    if (thumbnail_count == MAX_THUMBNAILS || strlen(url) >= MAX_URL) return 0;
    int fetch = fetch_start(url);
    if (fetch < 0) return 0;
    snprintf(thumbnails[thumbnail_count].url, MAX_URL, "%s", url);
    thumbnails[thumbnail_count].fetch = fetch;
    thumbnails[thumbnail_count++].state = THUMBNAIL_LOADING;
    return 0;
}

bool thumbnails_fetch_done(int fetch, bool ok, const char *data, size_t length) {
    for (int i = 0; i < thumbnail_count; i++) {
        if (thumbnails[i].state != THUMBNAIL_LOADING || thumbnails[i].fetch != fetch) continue;
        int width, height, channels;
        unsigned char *pixels = ok ? stbi_load_from_memory((const unsigned char *)data, (int)length, &width, &height,
                                                           &channels, 4)
                                   : NULL;
        thumbnails[i].state = pixels ? THUMBNAIL_READY : THUMBNAIL_FAILED;
        if (pixels) {
            glGenTextures(1, &thumbnails[i].texture);
            glBindTexture(GL_TEXTURE_2D, thumbnails[i].texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
        }
        return true;
    }
    return false;
}
