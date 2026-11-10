// Glyph rendering uses stb_truetype.h 1.26 (public domain or MIT, see THIRD_PARTY.md).
#include "overlay.h"
#include "unicode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "stb_image.h" // implemented in src/thumbnails.c; decodes the PNG glyphs of color emoji

#include "hb.h" // HarfBuzz 14.6.0 (MIT, see THIRD_PARTY.md) shapes the text of panels and prompts
#include "hb-ot.h"
#include <SheenBidi/SheenBidi.h> // SheenBidi 2.9.0 (Apache-2.0, see THIRD_PARTY.md) orders mixed directions

#define ATLAS_SIZE 2048
// The last two rows of the atlas stay white; rectangles sample them, so text and rectangles share one shader.
#define ATLAS_GLYPH_HEIGHT (ATLAS_SIZE - 2)
#define GLYPH_PADDING 1
#define FALLBACK_CODEPOINT '?'
#define LINE_SPACING 1.25f
#define GLYPH_TABLE_SIZE 8192 // a power of two
#define MAX_FONTS 16
#define MAX_FONT_PATH 1024
#define COLOR_EMOJI_PATTERN "emoji:color=true"

// A font file: the code font first, then fallback fonts that fontconfig finds for other scripts.
struct font {
    char path[MAX_FONT_PATH];
    int index;
    unsigned char *data;
    size_t size;
    stbtt_fontinfo info; // not for a color font
    bool color; // PNG glyphs (CBDT or sbix), such as Noto Color Emoji; HarfBuzz reads it
    float scale;
    hb_font_t *shaper; // created on the first shaped text
};

static int color_emoji_font = -1; // the color emoji font of the system, as browsers use it
static int emoji_font = -1; // the vendored monochrome emoji font, when the system has no color emoji

// A glyph in the atlas, rendered on the first use.
struct glyph {
    int codepoint; // 0: free slot
    int columns; // 1, or 2 for wide East Asian characters
    bool color; // RGBA in the atlas; the other glyphs are white with alpha
    float x0, y0, x1, y1; // the bitmap relative to the pen position on the baseline
    float s0, t0, s1, t1;
};

static struct glyph *overlay_glyph(int codepoint);

struct vertex {
    float x, y, u, v;
    uint8_t color[4];
};

static struct font fonts[MAX_FONTS];
static int font_count;
static struct glyph glyphs[GLYPH_TABLE_SIZE];
static int shelf_x, shelf_y, shelf_height; // the shelf packer of the atlas
static float current_font_size, char_width, ascent, line_height;
static GLuint atlas, program, vertex_array, vertex_buffer;
static GLint screen_location;
static struct vertex *vertices;
// Images (RGBA textures, such as extension thumbnails) draw after the text, one draw call each.
#define MAX_IMAGES 64
static struct {
    float x, y, width, height;
    unsigned texture;
} images[MAX_IMAGES];
static int image_count;
static bool atlas_full;
static size_t vertex_count, vertex_capacity;

static const char *vertex_source =
    "#version 300 es\n"
    "in vec2 position; in vec2 uv; in vec4 color;\n"
    "uniform vec2 screen;\n"
    "out vec2 v_uv; out vec4 v_color;\n"
    "void main() {\n"
    "  v_uv = uv; v_color = color;\n"
    "  vec2 p = position / screen * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
    "}\n";

// The atlas and the images are RGBA; glyphs of text are white with alpha, so the vertex color tints them.
static const char *fragment_source =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D atlas;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = texture(atlas, v_uv) * v_color; }\n";

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "overlay shader: %s\n", log);
    }
    return shader;
}

static hb_font_t *shaper(struct font *font) {
    if (!font->shaper) {
        hb_blob_t *blob = hb_blob_create((const char *)font->data, (unsigned)font->size, HB_MEMORY_MODE_READONLY, NULL, NULL);
        hb_face_t *face = hb_face_create(blob, (unsigned)font->index);
        font->shaper = hb_font_create(face);
        hb_face_destroy(face);
        hb_blob_destroy(blob);
    }
    return font->shaper;
}

// Pixels per font unit, so that the ascent and descent fill `size` pixels, as stbtt_ScaleForPixelHeight does.
static float font_scale(struct font *font, float size) {
    if (!font->color) return stbtt_ScaleForPixelHeight(&font->info, size);
    hb_font_extents_t extents;
    hb_font_get_h_extents(shaper(font), &extents);
    return size / (float)(extents.ascender - extents.descender);
}

static bool load_font(struct font *font, const char *path, int index) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    font->data = malloc((size_t)size);
    font->size = (size_t)size;
    bool read = fread(font->data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    snprintf(font->path, sizeof font->path, "%s", path);
    font->index = index;
    int offset = read ? stbtt_GetFontOffsetForIndex(font->data, index) : -1;
    font->color = false;
    if (offset >= 0 && stbtt_InitFont(&font->info, font->data, offset)) {
        font->scale = stbtt_ScaleForPixelHeight(&font->info, current_font_size);
        return true;
    }
    // stb_truetype reads no font without outlines, such as Noto Color Emoji with only PNG glyphs.
    if (read && hb_ot_color_has_png(hb_font_get_face(shaper(font)))) {
        font->color = true;
        font->scale = font_scale(font, current_font_size);
        return true;
    }
    if (font->shaper) hb_font_destroy(font->shaper);
    font->shaper = NULL;
    free(font->data);
    font->data = NULL;
    return false;
}

static unsigned glyph_index(struct font *font, int codepoint) {
    if (!font->color) return (unsigned)stbtt_FindGlyphIndex(&font->info, codepoint);
    hb_codepoint_t glyph;
    return hb_font_get_nominal_glyph(shaper(font), (hb_codepoint_t)codepoint, &glyph) ? glyph : 0;
}

// Asks fontconfig for a font with the code point and adds it to the fallback fonts.
static struct font *fallback_font(int codepoint) {
    if (font_count == MAX_FONTS) return NULL;
    char command[128], answer[MAX_FONT_PATH + 16] = "";
    snprintf(command, sizeof command, "fc-match -f '%%{file}|%%{index}' ':charset=%x'", codepoint);
    FILE *pipe = popen(command, "r");
    if (!pipe) return NULL;
    if (!fgets(answer, sizeof answer, pipe)) answer[0] = '\0';
    pclose(pipe);
    char *separator = strrchr(answer, '|');
    if (!separator) return NULL;
    *separator = '\0';
    int index = atoi(separator + 1);
    for (int i = 0; i < font_count; i++)
        if (!strcmp(fonts[i].path, answer) && fonts[i].index == index) return glyph_index(&fonts[i], codepoint) ? &fonts[i] : NULL;
    if (!load_font(&fonts[font_count], answer, index)) return NULL;
    struct font *font = &fonts[font_count++];
    return glyph_index(font, codepoint) ? font : NULL;
}

static void clear_glyphs(void) {
    memset(glyphs, 0, sizeof glyphs);
    shelf_x = shelf_y = shelf_height = 0;
    unsigned char *pixels = calloc((size_t)ATLAS_SIZE * ATLAS_SIZE, 4);
    memset(pixels + (size_t)ATLAS_GLYPH_HEIGHT * ATLAS_SIZE * 4, 0xFF, 2 * ATLAS_SIZE * 4);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
}

int overlay_columns(int codepoint) { return unicode_columns(codepoint); }

static bool is_emoji(int codepoint) {
    return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) || (codepoint >= 0x2600 && codepoint <= 0x27BF) ||
           (codepoint >= 0x2B00 && codepoint <= 0x2BFF) || codepoint == 0x203C || codepoint == 0x2049;
}

// The font for a code point: the code font, an emoji from an emoji font, else a fallback font.
static struct font *font_for(int codepoint) {
    for (int i = 0; i < 2; i++) {
        int emoji = i == 0 ? color_emoji_font : emoji_font;
        if (is_emoji(codepoint) && emoji >= 0 && glyph_index(&fonts[emoji], codepoint)) return &fonts[emoji];
    }
    if (glyph_index(&fonts[0], codepoint)) return &fonts[0];
    for (int i = 1; i < font_count; i++)
        if (i != emoji_font && i != color_emoji_font && glyph_index(&fonts[i], codepoint)) return &fonts[i];
    return fallback_font(codepoint);
}

static struct glyph *render_font_glyph(struct glyph *slot, int key, struct font *font, int glyph_index, int columns);

static struct glyph *render_glyph(struct glyph *slot, int codepoint) {
    struct font *font = font_for(codepoint);
    if (!font) return codepoint == FALLBACK_CODEPOINT ? NULL : overlay_glyph(FALLBACK_CODEPOINT);
    return render_font_glyph(slot, codepoint, font, (int)glyph_index(font, codepoint), overlay_columns(codepoint));
}

// The PNG of a color glyph, scaled to `width` by `height` pixels with an area average of premultiplied colors.
static unsigned char *color_glyph_bitmap(struct font *font, int glyph, int width, int height) {
    hb_blob_t *blob = hb_ot_color_glyph_reference_png(shaper(font), (hb_codepoint_t)glyph);
    unsigned length;
    const char *data = hb_blob_get_data(blob, &length);
    int source_width, source_height, channels;
    unsigned char *source = length ? stbi_load_from_memory((const unsigned char *)data, (int)length, &source_width,
                                                           &source_height, &channels, 4) : NULL;
    hb_blob_destroy(blob);
    if (!source) return NULL;
    unsigned char *bitmap = calloc((size_t)width * (size_t)height, 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int sx0 = x * source_width / width, sx1 = (x + 1) * source_width / width;
            int sy0 = y * source_height / height, sy1 = (y + 1) * source_height / height;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            float sum[4] = { 0 };
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *pixel = source + ((size_t)sy * source_width + sx) * 4;
                    for (int c = 0; c < 3; c++) sum[c] += pixel[c] * pixel[3] / 255.0f;
                    sum[3] += pixel[3];
                }
            }
            float count = (float)((sx1 - sx0) * (sy1 - sy0)), alpha = sum[3] / count;
            unsigned char *out = bitmap + ((size_t)y * width + x) * 4;
            for (int c = 0; c < 3; c++) out[c] = alpha > 0 ? (unsigned char)(sum[c] / count * 255.0f / alpha + 0.5f) : 0;
            out[3] = (unsigned char)(alpha + 0.5f);
        }
    }
    stbi_image_free(source);
    return bitmap;
}

// The RGBA bitmap of a glyph and its box relative to the pen position on the baseline.
static unsigned char *glyph_bitmap(struct font *font, int glyph, int *x0, int *y0, int *x1, int *y1) {
    if (font->color) {
        hb_glyph_extents_t extents;
        if (!hb_font_get_glyph_extents(shaper(font), (hb_codepoint_t)glyph, &extents)) return NULL;
        *x0 = (int)(extents.x_bearing * font->scale + 0.5f);
        *y0 = (int)(-extents.y_bearing * font->scale + 0.5f);
        *x1 = *x0 + (int)(extents.width * font->scale + 0.5f);
        *y1 = *y0 + (int)(-extents.height * font->scale + 0.5f);
        return *x1 > *x0 && *y1 > *y0 ? color_glyph_bitmap(font, glyph, *x1 - *x0, *y1 - *y0) : NULL;
    }
    stbtt_GetGlyphBitmapBox(&font->info, glyph, font->scale, font->scale, x0, y0, x1, y1);
    int width = *x1 - *x0, height = *y1 - *y0;
    if (width <= 0 || height <= 0) return NULL;
    unsigned char *alpha = calloc((size_t)width * (size_t)height, 1), *bitmap = malloc((size_t)width * (size_t)height * 4);
    stbtt_MakeGlyphBitmap(&font->info, alpha, width, height, width, font->scale, font->scale, glyph);
    for (size_t i = 0; i < (size_t)width * (size_t)height; i++) {
        bitmap[i * 4] = bitmap[i * 4 + 1] = bitmap[i * 4 + 2] = 0xFF;
        bitmap[i * 4 + 3] = alpha[i];
    }
    free(alpha);
    return bitmap;
}

// Renders a glyph of `font` into the atlas; `key` is a code point, or a negative key for a shaped glyph.
static struct glyph *render_font_glyph(struct glyph *slot, int key, struct font *font, int glyph_index, int columns) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned char *bitmap = glyph_bitmap(font, glyph_index, &x0, &y0, &x1, &y1);
    int width = bitmap ? x1 - x0 : 0, height = bitmap ? y1 - y0 : 0;
    if (shelf_x + width + GLYPH_PADDING > ATLAS_SIZE) {
        shelf_x = 0;
        shelf_y += shelf_height + GLYPH_PADDING;
        shelf_height = 0;
    }
    if (shelf_y + height + GLYPH_PADDING > ATLAS_GLYPH_HEIGHT) { // full; overlay_end clears it
        free(bitmap);
        return NULL;
    }
    if (bitmap) {
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, shelf_x, shelf_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bitmap);
        free(bitmap);
    }
    // A glyph of a fallback font sits on the baseline of the code font.
    *slot = (struct glyph){ key, columns, font->color, (float)x0, (float)y0, (float)x1, (float)y1,
                            (float)shelf_x / ATLAS_SIZE, (float)shelf_y / ATLAS_SIZE,
                            (float)(shelf_x + width) / ATLAS_SIZE, (float)(shelf_y + height) / ATLAS_SIZE };
    shelf_x += width + GLYPH_PADDING;
    if (height > shelf_height) shelf_height = height;
    return slot;
}

static struct glyph *overlay_glyph(int codepoint) {
    unsigned hash = (unsigned)codepoint * 2654435761u;
    for (unsigned probe = 0; probe < GLYPH_TABLE_SIZE; probe++) {
        struct glyph *slot = &glyphs[(hash + probe) & (GLYPH_TABLE_SIZE - 1)];
        if (slot->codepoint == codepoint) return slot;
        if (slot->codepoint == 0) return render_glyph(slot, codepoint);
    }
    return NULL;
}

bool overlay_set_font_size(float font_size) {
    if (font_size < OVERLAY_MIN_FONT_SIZE) font_size = OVERLAY_MIN_FONT_SIZE;
    if (font_size > OVERLAY_MAX_FONT_SIZE) font_size = OVERLAY_MAX_FONT_SIZE;
    current_font_size = font_size;
    for (int i = 0; i < font_count; i++) fonts[i].scale = font_scale(&fonts[i], font_size);
    int font_ascent, font_descent, line_gap, advance, bearing;
    stbtt_GetFontVMetrics(&fonts[0].info, &font_ascent, &font_descent, &line_gap);
    stbtt_GetCodepointHMetrics(&fonts[0].info, 'M', &advance, &bearing);
    char_width = (float)(int)(advance * fonts[0].scale + 0.5f);
    ascent = (float)(int)(font_ascent * fonts[0].scale + 0.5f);
    line_height = (float)(int)(font_size * LINE_SPACING + 0.5f);
    if (!atlas) {
        glGenTextures(1, &atlas);
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    clear_glyphs();
    return true;
}

bool overlay_init(const char *font_path, const char *emoji_font_path, float font_size) {
    current_font_size = font_size;
    if (!load_font(&fonts[0], font_path, 0)) {
        fprintf(stderr, "overlay: cannot use the font %s\n", font_path);
        return false;
    }
    font_count = 1;
    char color_path[MAX_FONT_PATH + 16] = "";
    FILE *pipe = popen("fc-match -f '%{file}' '" COLOR_EMOJI_PATTERN "'", "r");
    if (pipe) {
        if (!fgets(color_path, sizeof color_path, pipe)) color_path[0] = '\0';
        pclose(pipe);
    }
    if (color_path[0] && load_font(&fonts[font_count], color_path, 0) && fonts[font_count].color) color_emoji_font = font_count++;
    if (emoji_font_path && load_font(&fonts[font_count], emoji_font_path, 0)) emoji_font = font_count++;
    program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "uv");
    glBindAttribLocation(program, 2, "color");
    glLinkProgram(program);
    screen_location = glGetUniformLocation(program, "screen");

    // A vertex array object of its own leaves the attribute state of the hydra passes untouched.
    glGenVertexArrays(1, &vertex_array);
    glBindVertexArray(vertex_array);
    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)offsetof(struct vertex, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)offsetof(struct vertex, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vertex), (void *)offsetof(struct vertex, color));
    glBindVertexArray(0);
    return overlay_set_font_size(font_size);
}

float overlay_font_size(void) { return current_font_size; }
float overlay_char_width(void) { return char_width; }
float overlay_line_height(void) { return line_height; }

void overlay_begin(void) {
    vertex_count = 0;
    image_count = 0;
}

void overlay_image(float x, float y, float width, float height, unsigned texture) {
    if (image_count < MAX_IMAGES) images[image_count++] = (typeof(images[0])){ x, y, width, height, texture };
}

static void push_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color) {
    if (vertex_count + 6 > vertex_capacity) {
        vertex_capacity = vertex_capacity ? vertex_capacity * 2 : 6 * 4096;
        vertices = realloc(vertices, vertex_capacity * sizeof *vertices);
    }
    uint8_t rgba[4] = { color >> 24, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF };
    struct vertex corners[4] = {
        { x0, y0, u0, v0, { 0 } }, { x1, y0, u1, v0, { 0 } }, { x1, y1, u1, v1, { 0 } }, { x0, y1, u0, v1, { 0 } },
    };
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        vertices[vertex_count] = corners[order[i]];
        memcpy(vertices[vertex_count].color, rgba, 4);
        vertex_count++;
    }
}

// Color glyphs keep their colors and take only the alpha of the text color.
static uint32_t glyph_color(const struct glyph *glyph, uint32_t color) { return glyph->color ? 0xFFFFFF00u | (color & 0xFF) : color; }

void overlay_rect(float x, float y, float width, float height, uint32_t color) {
    const float white = (ATLAS_SIZE - 1.0f) / ATLAS_SIZE;
    push_quad(x, y, x + width, y + height, 0.5f / ATLAS_SIZE, white, 0.5f / ATLAS_SIZE, white, color);
}

static int decode_utf8(const unsigned char *text, size_t length, size_t *consumed) {
    int codepoint = unicode_decode(text, length, consumed);
    return codepoint == 0xFFFD ? FALLBACK_CODEPOINT : codepoint;
}

// ---------------------------------------------------------------- shaped text with bidi

#define MAX_SHAPED 2048

// Embedding levels of the full Unicode bidi algorithm (UAX #9, with explicit embeddings and isolates) from
// SheenBidi; the levels of the line include rule L1 for the white space at the end.
static void bidi_levels(const int *codepoints, int count, int paragraph, unsigned char *levels) {
    for (int i = 0; i < count; i++) levels[i] = (unsigned char)paragraph; // text after a paragraph separator
    // SheenBidi returns no algorithm for empty text.
    if (count <= 0) return;
    SBCodepointSequence sequence = { SBStringEncodingUTF32, codepoints, (SBUInteger)count };
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    if (!algorithm) return;
    SBParagraphRef bidi_paragraph = SBAlgorithmCreateParagraph(algorithm, 0, (SBUInteger)count, (SBLevel)paragraph);
    SBUInteger length = SBParagraphGetLength(bidi_paragraph);
    SBLineRef line = SBParagraphCreateLine(bidi_paragraph, 0, length);
    const SBRun *runs = SBLineGetRunsPtr(line);
    for (SBUInteger r = 0; r < SBLineGetRunCount(line); r++)
        for (SBUInteger k = runs[r].offset; k < runs[r].offset + runs[r].length; k++) levels[k] = runs[r].level;
    SBLineRelease(line);
    SBParagraphRelease(bidi_paragraph);
    SBAlgorithmRelease(algorithm);
}

struct shaped_glyph {
    struct font *font;
    unsigned index;
    float advance, x_offset, y_offset;
};

// Shapes `text` into glyphs in visual order and returns their count; `paragraph` is 1 for right-to-left text.
// `forced` (or NULL) is the font for all of the text, as for an emoji sequence.
static int shape_with(const char *text, size_t length, int paragraph, struct shaped_glyph *glyphs, int capacity, struct font *forced) {
    int codepoints[MAX_SHAPED], count = 0;
    for (size_t offset = 0; offset < length && count < MAX_SHAPED;) {
        size_t consumed;
        codepoints[count++] = unicode_decode((const unsigned char *)text + offset, length - offset, &consumed);
        offset += consumed;
    }
    unsigned char levels[MAX_SHAPED];
    bidi_levels(codepoints, count, paragraph, levels);
    // Runs of one level and one font, shaped one by one; each run gives its glyphs in visual order.
    struct { int start, end; unsigned char level; int first_glyph, glyph_count; } runs[MAX_SHAPED];
    int run_count = 0, glyph_count = 0;
    struct shaped_glyph logical[MAX_SHAPED];
    for (int i = 0; i < count;) {
        struct font *font = forced ? forced : font_for(codepoints[i]);
        if (!font) font = &fonts[0];
        int end = i + 1;
        // A run keeps its font for spaces and for the joiners, marks and modifiers of an emoji sequence.
        while (end < count && levels[end] == levels[i] &&
               (forced || font_for(codepoints[end]) == font || codepoints[end] == ' ' || unicode_break_class(codepoints[end]) == BREAK_EXTEND ||
                codepoints[end] == 0x200D || (codepoints[end - 1] == 0x200D && color_emoji_font >= 0 && font == &fonts[color_emoji_font])))
            end++;
        hb_buffer_t *buffer = hb_buffer_create();
        hb_buffer_add_codepoints(buffer, (const hb_codepoint_t *)codepoints, count, (unsigned)i, end - i);
        hb_buffer_set_direction(buffer, levels[i] % 2 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(shaper(font), buffer, NULL, 0);
        unsigned shaped_count;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &shaped_count);
        hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &shaped_count);
        runs[run_count].start = i;
        runs[run_count].end = end;
        runs[run_count].level = levels[i];
        runs[run_count].first_glyph = glyph_count;
        for (unsigned g = 0; g < shaped_count && glyph_count < MAX_SHAPED; g++)
            logical[glyph_count++] = (struct shaped_glyph){ font, info[g].codepoint, positions[g].x_advance * font->scale,
                                                            positions[g].x_offset * font->scale, positions[g].y_offset * font->scale };
        runs[run_count].glyph_count = glyph_count - runs[run_count].first_glyph;
        run_count++;
        hb_buffer_destroy(buffer);
        i = end;
    }
    // Rule L2: reverse the sequences of runs at each level from the highest to the lowest odd level.
    int order[MAX_SHAPED];
    unsigned char highest = 0, lowest_odd = 255;
    for (int r = 0; r < run_count; r++) {
        order[r] = r;
        if (runs[r].level > highest) highest = runs[r].level;
        if (runs[r].level % 2 && runs[r].level < lowest_odd) lowest_odd = runs[r].level;
    }
    for (int level = highest; lowest_odd != 255 && level >= lowest_odd; level--) {
        for (int r = 0; r < run_count;) {
            if (runs[order[r]].level < level) { r++; continue; }
            int end = r;
            while (end < run_count && runs[order[end]].level >= level) end++;
            for (int a = r, b = end - 1; a < b; a++, b--) {
                int swap = order[a];
                order[a] = order[b];
                order[b] = swap;
            }
            r = end;
        }
    }
    int out = 0;
    for (int r = 0; r < run_count; r++)
        for (int g = 0; g < runs[order[r]].glyph_count && out < capacity; g++)
            glyphs[out++] = logical[runs[order[r]].first_glyph + g];
    return out;
}

static int shape(const char *text, size_t length, int paragraph, struct shaped_glyph *glyphs, int capacity) {
    return shape_with(text, length, paragraph, glyphs, capacity, NULL);
}

static struct shaped_glyph shaped_glyphs[MAX_SHAPED];

float overlay_text_shaped_width(const char *text, size_t length, bool right_to_left) {
    int count = shape(text, length, right_to_left, shaped_glyphs, MAX_SHAPED);
    float width = 0;
    for (int i = 0; i < count; i++) width += shaped_glyphs[i].advance;
    return width;
}

// Draws shaped glyphs from the pen position; `scale` shrinks them (a cluster that is wider than its cells).
static float draw_glyphs(float x, float y, const struct shaped_glyph *list, int count, uint32_t color, float scale) {
    float pen_x = x;
    for (int i = 0; i < count; i++) {
        const struct shaped_glyph *shaped = &list[i];
        int font_index = (int)(shaped->font - fonts);
        int key = -(font_index << 24 | (int)shaped->index) - 1; // shaped glyphs use negative keys
        unsigned hash = (unsigned)key * 2654435761u;
        struct glyph *glyph = NULL;
        for (unsigned probe = 0; probe < GLYPH_TABLE_SIZE && !glyph; probe++) {
            struct glyph *slot = &glyphs[(hash + probe) & (GLYPH_TABLE_SIZE - 1)];
            if (slot->codepoint == key) glyph = slot;
            else if (slot->codepoint == 0) glyph = render_font_glyph(slot, key, shaped->font, (int)shaped->index, 1);
        }
        if (glyph) {
            float gx = pen_x + shaped->x_offset * scale, gy = y + ascent - shaped->y_offset * scale;
            push_quad(gx + glyph->x0 * scale, gy + glyph->y0 * scale, gx + glyph->x1 * scale, gy + glyph->y1 * scale, glyph->s0,
                      glyph->t0, glyph->s1, glyph->t1, glyph_color(glyph, color));
        } else {
            atlas_full = true;
        }
        pen_x += shaped->advance * scale;
    }
    return pen_x - x;
}

float overlay_text_shaped(float x, float y, const char *text, size_t length, bool right_to_left, uint32_t color) {
    int count = shape(text, length, right_to_left, shaped_glyphs, MAX_SHAPED);
    return draw_glyphs(x, y, shaped_glyphs, count, color, 1);
}

void overlay_text_fitted(float x, float y, const char *text, size_t length, float width, uint32_t color) {
    int count = shape(text, length, 1, shaped_glyphs, MAX_SHAPED);
    float natural = 0;
    for (int i = 0; i < count; i++) natural += shaped_glyphs[i].advance;
    float scale = natural > width && natural > 0 ? width / natural : 1;
    draw_glyphs(x + width - natural * scale, y, shaped_glyphs, count, color, scale);
}

#define MAX_CLUSTER_GLYPHS 32

// A grapheme cluster of several code points (ZWJ sequence, flag, keycap, letter with marks) shaped as one,
// shrunk to its cells when it is wider.
static void draw_cluster(float x, float y, const char *text, size_t length, int columns, uint32_t color) {
    struct shaped_glyph cluster[MAX_CLUSTER_GLYPHS];
    // A cluster in emoji presentation (two columns) takes the color emoji font, also for keycaps such as 1 U+20E3.
    size_t size;
    int first = unicode_decode((const unsigned char *)text, length, &size);
    bool emoji = columns == 2 && (!unicode_is_wide(first) || UNICODE_LOOKUP(extended_pictographic_ranges, first));
    struct font *forced = emoji && color_emoji_font >= 0 ? &fonts[color_emoji_font] : NULL;
    int count = shape_with(text, length, 0, cluster, MAX_CLUSTER_GLYPHS, forced);
    float width = 0, cell = (float)columns * char_width;
    for (int i = 0; i < count; i++) width += cluster[i].advance;
    draw_glyphs(x, y, cluster, count, color, width > cell && width > 0 ? cell / width : 1);
}

void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {
    size_t column = 0;
    for (size_t offset = 0; offset < length && column < first_column + max_columns;) {
        size_t consumed, end = unicode_cluster_end((const unsigned char *)text, length, offset);
        int codepoint = decode_utf8((const unsigned char *)text + offset, length - offset, &consumed);
        int columns = codepoint == '\t' ? OVERLAY_TAB_WIDTH - (int)(column % OVERLAY_TAB_WIDTH)
                                         : unicode_cluster_columns((const unsigned char *)text, offset, end);
        if (column >= first_column && end > offset + consumed) {
            draw_cluster(x + (float)(column - first_column) * char_width, y, text + offset, end - offset, columns,
                         colors ? colors[offset] : color);
        } else if (column >= first_column && codepoint > ' ') {
            struct glyph *glyph = overlay_glyph(codepoint);
            if (glyph) {
                float pen_x = x + (float)(column - first_column) * char_width, pen_y = y + ascent;
                // A color emoji that is wider than its columns shrinks to fit the grid.
                float cell = (float)columns * char_width, glyph_width = glyph->x1 - glyph->x0;
                float fit = glyph->color && glyph_width > cell ? cell / glyph_width : 1.0f;
                push_quad(pen_x + glyph->x0 * fit, pen_y + glyph->y0 * fit, pen_x + glyph->x1 * fit, pen_y + glyph->y1 * fit,
                          glyph->s0, glyph->t0, glyph->s1, glyph->t1, glyph_color(glyph, colors ? colors[offset] : color));
            } else {
                atlas_full = true;
            }
        }
        offset = end;
        column += (size_t)columns;
    }
}

size_t overlay_text_columns(const char *text, size_t length) {
    size_t columns = 0;
    for (size_t offset = 0; offset < length;) {
        size_t end = unicode_cluster_end((const unsigned char *)text, length, offset);
        columns += text[offset] == '\t' ? OVERLAY_TAB_WIDTH - columns % OVERLAY_TAB_WIDTH
                                         : (size_t)unicode_cluster_columns((const unsigned char *)text, offset, end);
        offset = end;
    }
    return columns;
}

void overlay_end(int width, int height) {
    // A full atlas starts again empty; the glyphs of this frame come back in the next frame.
    if (atlas_full) {
        atlas_full = false;
        clear_glyphs();
    }
    if (vertex_count == 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glEnable(GL_BLEND);
    // The destination alpha stays as hydra wrote it.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glUseProgram(program);
    glUniform2f(screen_location, (float)width, (float)height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glBindVertexArray(vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof *vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertex_count);
    if (image_count) {
        size_t first = vertex_count;
        for (int i = 0; i < image_count; i++)
            push_quad(images[i].x, images[i].y, images[i].x + images[i].width, images[i].y + images[i].height, 0, 0, 1, 1,
                      0xFFFFFFFFu);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof *vertices), vertices, GL_STREAM_DRAW);
        for (int i = 0; i < image_count; i++) {
            glBindTexture(GL_TEXTURE_2D, images[i].texture);
            glDrawArrays(GL_TRIANGLES, (GLint)(first + 6 * (size_t)i), 6);
        }
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}
