// The text sources of js/text.js (text(), strokeText() and the like, with the interface of the hydra-text
// extension): lines in one font, filled, outlined or both, drawn into an RGBA texture. stb_truetype (compiled in
// src/overlay.c) gives the glyphs, and fontconfig finds the font file of a CSS family.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "stb_truetype.h"

#include "native.h"
#include "overlay.h"
#include "unicode.h"

#define MAX_TEXT_FONTS 16
#define FONT_KEY_SIZE 160
#define DEFAULT_FAMILY "sans-serif"
#define SDF_ON_EDGE 128
#define SDF_MARGIN 2 // pixels around the outline in the distance field of a glyph
#define SDF_FONT_SIZE 128.0f // the largest font size of a distance field, in pixels
// A sketch gives the sizes. These limits keep the allocations and the time of one call in bounds.
#define MAX_TEXTURE_DIMENSION 16384
#define MAX_LINE_WIDTH 256.0f

static struct text_font {
    char key[FONT_KEY_SIZE];
    void *data; // NULL when no font file was found
    stbtt_fontinfo info;
} fonts[MAX_TEXT_FONTS];
static int font_count;

static bool load_font(struct text_font *font, const char *path, int index) {
    font->data = SDL_LoadFile(path, NULL);
    int offset = font->data ? stbtt_GetFontOffsetForIndex(font->data, index) : -1;
    if (offset >= 0 && stbtt_InitFont(&font->info, font->data, offset)) return true;
    SDL_free(font->data);
    font->data = NULL;
    return false;
}

// The font of the first family of a CSS font list, from fc-match (generic names such as serif work too), or the
// code font where fontconfig is missing. A family is a name, never a path, so it cannot hold "/" or ":".
static const stbtt_fontinfo *find_font(const char *family, bool bold, bool italic) {
    char name[FONT_KEY_SIZE - 8];
    size_t length = strcspn(family, ",");
    while (length && strchr(" '\"", family[0])) family++, length--;
    while (length && strchr(" '\"", family[length - 1])) length--;
    snprintf(name, sizeof name, "%.*s", (int)length, family);
    if (!name[0] || strpbrk(name, "/:")) snprintf(name, sizeof name, DEFAULT_FAMILY);
    char key[FONT_KEY_SIZE];
    snprintf(key, sizeof key, "%s|%d|%d", name, bold, italic);
    for (int i = 0; i < font_count; i++)
        if (!strcmp(fonts[i].key, key)) return fonts[i].data ? &fonts[i].info : NULL;
    // ponytail: the cache keeps the first MAX_TEXT_FONTS fonts; later ones use the first font. Evict when a
    // sketch needs more.
    if (font_count == MAX_TEXT_FONTS) return fonts[0].data ? &fonts[0].info : NULL;
    struct text_font *font = &fonts[font_count++];
    snprintf(font->key, sizeof font->key, "%s", key);
    char pattern[FONT_KEY_SIZE + 48];
    snprintf(pattern, sizeof pattern, "%s:weight=%s:slant=%s", name, bold ? "bold" : "regular",
             italic ? "italic" : "roman");
    const char *arguments[] = { "fc-match", "-f", "%{index} %{file}", pattern, NULL };
    SDL_Process *process = SDL_CreateProcess(arguments, true);
    char *found = process ? SDL_ReadProcess(process, NULL, NULL) : NULL;
    SDL_DestroyProcess(process);
    char *path = found ? strchr(found, ' ') : NULL;
    if (!(path && load_font(font, path + 1, atoi(found))) && !load_font(font, overlay_font_path(), 0))
        fprintf(stderr, "text: no font for %s\n", name);
    SDL_free(found);
    return font->data ? &font->info : NULL;
}

// Calls `glyph(pen, index, data)` for each glyph of `line`, with the pen position in font units; returns the
// width of the line in font units.
static float walk_line(const stbtt_fontinfo *font, const char *line, size_t length,
                       void (*glyph)(float pen, int index, void *data), void *data) {
    float pen = 0;
    int previous = 0;
    for (size_t offset = 0, size; offset < length; offset += size) {
        int codepoint = unicode_decode((const unsigned char *)line + offset, length - offset, &size);
        int index = stbtt_FindGlyphIndex(font, codepoint);
        int advance;
        stbtt_GetGlyphHMetrics(font, index, &advance, NULL);
        if (previous) pen += (float)stbtt_GetGlyphKernAdvance(font, previous, index);
        if (glyph) glyph(pen, index, data);
        pen += (float)advance;
        previous = index;
    }
    return pen;
}

float text_width(const char *family, bool bold, bool italic, const char *line) {
    const stbtt_fontinfo *font = find_font(family, bold, italic);
    return font ? walk_line(font, line, strlen(line), NULL, NULL) * stbtt_ScaleForMappingEmToPixels(font, 1) : 0;
}

struct pass {
    const struct text_options *options;
    const stbtt_fontinfo *font;
    float size, scale, left, baseline, line_width;
    float color[4];
    bool outline;
    unsigned char *pixels; // premultiplied RGBA
};

// The value of `bitmap` at (x, y) in its pixels, between the four nearest pixels.
static float sample(const unsigned char *bitmap, int width, int height, float x, float y) {
    x = fminf(fmaxf(x, 0), (float)(width - 1));
    y = fminf(fmaxf(y, 0), (float)(height - 1));
    int x0 = (int)x, y0 = (int)y, x1 = x0 + 1 < width ? x0 + 1 : x0, y1 = y0 + 1 < height ? y0 + 1 : y0;
    const unsigned char *upper = bitmap + y0 * width, *lower = bitmap + y1 * width;
    float top = (float)upper[x0] + (float)(upper[x1] - upper[x0]) * (x - (float)x0);
    float bottom = (float)lower[x0] + (float)(lower[x1] - lower[x0]) * (x - (float)x0);
    return top + (bottom - top) * (y - (float)y0);
}

static void draw_glyph(float pen, int index, void *data) {
    const struct pass *pass = data;
    const struct text_options *options = pass->options;
    int width, height, x_offset, y_offset;
    // The distance field of a glyph costs time in proportion to its area, about a second for one glyph that
    // fills a window. So the field has at most SDF_FONT_SIZE, and `zoom` texture pixels show one of its pixels.
    float zoom = pass->outline ? fmaxf(pass->size / SDF_FONT_SIZE, 1) : 1;
    int margin = (int)ceilf(pass->line_width / 2 / zoom) + SDF_MARGIN;
    float distance_scale = (float)(SDF_ON_EDGE - 1) / (float)margin;
    unsigned char *bitmap = pass->outline
        ? stbtt_GetGlyphSDF(pass->font, pass->scale / zoom, index, margin, SDF_ON_EDGE, distance_scale, &width, &height,
                            &x_offset, &y_offset)
        : stbtt_GetGlyphBitmap(pass->font, pass->scale, pass->scale, index, &width, &height, &x_offset, &y_offset);
    if (!bitmap) return;
    int left = (int)lroundf(pass->left + pen * pass->scale + (float)x_offset * zoom);
    int top = (int)lroundf(pass->baseline + (float)y_offset * zoom);
    int drawn_width = (int)ceilf((float)width * zoom), drawn_height = (int)ceilf((float)height * zoom);
    for (int y = 0; y < drawn_height; y++) {
        if (top + y < 0 || top + y >= options->height) continue;
        for (int x = 0; x < drawn_width; x++) {
            if (left + x < 0 || left + x >= options->width) continue;
            float coverage;
            if (pass->outline) {
                float value = sample(bitmap, width, height, ((float)x + 0.5f) / zoom - 0.5f,
                                     ((float)y + 0.5f) / zoom - 0.5f);
                float distance = fabsf(value - SDF_ON_EDGE) / distance_scale * zoom;
                coverage = fminf(fmaxf(pass->line_width / 2 - distance + 0.5f, 0), 1);
            } else {
                coverage = (float)bitmap[y * width + x] / 255;
            }
            float alpha = coverage * pass->color[3];
            unsigned char *pixel = pass->pixels + ((size_t)(top + y) * (size_t)options->width + (size_t)(left + x)) * 4;
            for (int i = 0; i < 4; i++)
                pixel[i] = (unsigned char)lroundf((i < 3 ? pass->color[i] : 1) * alpha * 255 +
                                                  (float)pixel[i] * (1 - alpha));
        }
    }
    stbtt_FreeBitmap(bitmap, NULL);
}

bool text_draw(unsigned texture, const struct text_options *options) {
    const stbtt_fontinfo *font = find_font(options->family, options->bold, options->italic);
    bool found = font != NULL;
    if (options->width <= 0 || options->height <= 0 || options->width > MAX_TEXTURE_DIMENSION ||
        options->height > MAX_TEXTURE_DIMENSION)
        return false;
    unsigned char *pixels = calloc((size_t)options->width * (size_t)options->height, 4);
    if (!pixels) return false;
    // The comparisons are false for NaN, which then draws nothing.
    float size = options->size > 0 ? fminf(options->size, (float)MAX_TEXTURE_DIMENSION) : 0;
    struct pass pass = { .options = options, .font = size > 0 ? font : NULL, .pixels = pixels, .size = size,
                         .line_width = options->line_width > 0 ? fminf(options->line_width, MAX_LINE_WIDTH) : 0 };
    font = pass.font;
    int ascent = 0, descent = 0, line_count = 1;
    if (font) {
        pass.scale = stbtt_ScaleForMappingEmToPixels(font, size);
        stbtt_GetFontVMetrics(font, &ascent, &descent, NULL);
    }
    for (const char *c = options->text; *c; c++) line_count += *c == '\n';
    const char *line = options->text;
    for (int i = 0; font && i < line_count; i++) {
        size_t length = strcspn(line, "\n");
        float width = walk_line(font, line, length, NULL, NULL) * pass.scale;
        pass.left = options->x - width * (float)options->align / 2; // left 0, center 1, right 2
        // The lines spread over the height; each has its middle (the middle of the em square) at its position.
        pass.baseline = (float)options->height / (float)(line_count + 1) * (float)(i + 1) +
                        (float)(ascent + descent) * pass.scale / 2;
        for (int step = 0; step < TEXT_PASS_COUNT; step++) {
            if (!(options->passes & (1 << step))) continue;
            pass.outline = step == 1;
            uint32_t color = pass.outline ? options->stroke : options->fill;
            for (int k = 0; k < 4; k++) pass.color[k] = (float)((color >> (24 - 8 * k)) & 0xFF) / 255;
            walk_line(font, line, length, draw_glyph, &pass);
        }
        line += length + 1;
    }
    // To straight alpha; rounding can leave a color one step above its alpha, which must not wrap around.
    for (size_t i = 0; i < (size_t)options->width * (size_t)options->height * 4; i += 4)
        for (int k = 0; k < 3 && pixels[i + 3]; k++) {
            int value = pixels[i + k] * 255 / pixels[i + 3];
            pixels[i + k] = (unsigned char)(value > 255 ? 255 : value);
        }
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, options->width, options->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    return found;
}
