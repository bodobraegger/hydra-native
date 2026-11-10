// Glyph rendering uses stb_truetype.h 1.26 (public domain or MIT, see THIRD_PARTY.md).
#include "overlay.h"
#include "unicode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define ATLAS_SIZE 2048
// The last two rows of the atlas stay white; rectangles sample them, so text and rectangles share one shader.
#define ATLAS_GLYPH_HEIGHT (ATLAS_SIZE - 2)
#define GLYPH_PADDING 1
#define FALLBACK_CODEPOINT '?'
#define LINE_SPACING 1.25f
#define GLYPH_TABLE_SIZE 8192 // a power of two
#define MAX_FONTS 16
#define MAX_FONT_PATH 1024

// A font file: the code font first, then fallback fonts that fontconfig finds for other scripts.
struct font {
    char path[MAX_FONT_PATH];
    int index;
    unsigned char *data;
    stbtt_fontinfo info;
    float scale;
};

// A glyph in the atlas, rendered on the first use.
struct glyph {
    int codepoint; // 0: free slot
    int columns; // 1, or 2 for wide East Asian characters
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
static GLuint image_program;
static GLint image_screen_location;
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

static const char *image_fragment_source =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D atlas;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = texture(atlas, v_uv) * v_color; }\n";

static const char *fragment_source =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D atlas;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = vec4(v_color.rgb, v_color.a * texture(atlas, v_uv).r); }\n";

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

static bool load_font(struct font *font, const char *path, int index) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    font->data = malloc((size_t)size);
    bool read = fread(font->data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    int offset = read ? stbtt_GetFontOffsetForIndex(font->data, index) : -1;
    if (offset < 0 || !stbtt_InitFont(&font->info, font->data, offset)) {
        free(font->data);
        font->data = NULL;
        return false;
    }
    snprintf(font->path, sizeof font->path, "%s", path);
    font->index = index;
    font->scale = stbtt_ScaleForPixelHeight(&font->info, current_font_size);
    return true;
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
        if (!strcmp(fonts[i].path, answer) && fonts[i].index == index)
            return stbtt_FindGlyphIndex(&fonts[i].info, codepoint) ? &fonts[i] : NULL;
    if (!load_font(&fonts[font_count], answer, index)) return NULL;
    struct font *font = &fonts[font_count++];
    return stbtt_FindGlyphIndex(&font->info, codepoint) ? font : NULL;
}

static void clear_glyphs(void) {
    memset(glyphs, 0, sizeof glyphs);
    shelf_x = shelf_y = shelf_height = 0;
    unsigned char *pixels = calloc(ATLAS_SIZE, ATLAS_SIZE);
    memset(pixels + (size_t)ATLAS_GLYPH_HEIGHT * ATLAS_SIZE, 0xFF, 2 * ATLAS_SIZE);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
}

int overlay_columns(int codepoint) { return unicode_is_wide(codepoint) ? 2 : 1; }

static struct glyph *render_glyph(struct glyph *slot, int codepoint) {
    struct font *font = &fonts[0];
    if (!stbtt_FindGlyphIndex(&font->info, codepoint)) {
        font = NULL;
        for (int i = 1; i < font_count && !font; i++)
            if (stbtt_FindGlyphIndex(&fonts[i].info, codepoint)) font = &fonts[i];
        if (!font) font = fallback_font(codepoint);
        if (!font) return codepoint == FALLBACK_CODEPOINT ? NULL : overlay_glyph(FALLBACK_CODEPOINT);
    }
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, codepoint, font->scale, font->scale, &x0, &y0, &x1, &y1);
    int width = x1 - x0, height = y1 - y0;
    if (shelf_x + width + GLYPH_PADDING > ATLAS_SIZE) {
        shelf_x = 0;
        shelf_y += shelf_height + GLYPH_PADDING;
        shelf_height = 0;
    }
    if (shelf_y + height + GLYPH_PADDING > ATLAS_GLYPH_HEIGHT) return NULL; // full; overlay_end clears it
    unsigned char *bitmap = calloc((size_t)(width > 0 ? width : 1) * (size_t)(height > 0 ? height : 1), 1);
    if (width > 0 && height > 0) {
        stbtt_MakeCodepointBitmap(&font->info, bitmap, width, height, width, font->scale, font->scale, codepoint);
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, shelf_x, shelf_y, width, height, GL_RED, GL_UNSIGNED_BYTE, bitmap);
    }
    free(bitmap);
    // A glyph of a fallback font sits on the baseline of the code font.
    *slot = (struct glyph){ codepoint, overlay_columns(codepoint), (float)x0, (float)y0, (float)x1, (float)y1,
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
    for (int i = 0; i < font_count; i++) fonts[i].scale = stbtt_ScaleForPixelHeight(&fonts[i].info, font_size);
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

bool overlay_init(const char *font_path, float font_size) {
    current_font_size = font_size;
    if (!load_font(&fonts[0], font_path, 0)) {
        fprintf(stderr, "overlay: cannot use the font %s\n", font_path);
        return false;
    }
    font_count = 1;
    program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "uv");
    glBindAttribLocation(program, 2, "color");
    glLinkProgram(program);
    screen_location = glGetUniformLocation(program, "screen");
    image_program = glCreateProgram();
    glAttachShader(image_program, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(image_program, compile(GL_FRAGMENT_SHADER, image_fragment_source));
    glBindAttribLocation(image_program, 0, "position");
    glBindAttribLocation(image_program, 1, "uv");
    glBindAttribLocation(image_program, 2, "color");
    glLinkProgram(image_program);
    image_screen_location = glGetUniformLocation(image_program, "screen");

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

void overlay_rect(float x, float y, float width, float height, uint32_t color) {
    const float white = (ATLAS_SIZE - 1.0f) / ATLAS_SIZE;
    push_quad(x, y, x + width, y + height, 0.5f / ATLAS_SIZE, white, 0.5f / ATLAS_SIZE, white, color);
}

static int decode_utf8(const unsigned char *text, size_t length, size_t *consumed) {
    int codepoint = unicode_decode(text, length, consumed);
    return codepoint == 0xFFFD ? FALLBACK_CODEPOINT : codepoint;
}

void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {
    size_t column = 0;
    for (size_t offset = 0; offset < length && column < first_column + max_columns;) {
        size_t consumed;
        int codepoint = decode_utf8((const unsigned char *)text + offset, length - offset, &consumed);
        int columns = codepoint == '\t' ? OVERLAY_TAB_WIDTH - (int)(column % OVERLAY_TAB_WIDTH) : overlay_columns(codepoint);
        if (column >= first_column && codepoint > ' ') {
            struct glyph *glyph = overlay_glyph(codepoint);
            if (glyph) {
                float pen_x = x + (float)(column - first_column) * char_width, pen_y = y + ascent;
                push_quad(pen_x + glyph->x0, pen_y + glyph->y0, pen_x + glyph->x1, pen_y + glyph->y1, glyph->s0,
                          glyph->t0, glyph->s1, glyph->t1, colors ? colors[offset] : color);
            } else {
                atlas_full = true;
            }
        }
        offset += consumed;
        column += (size_t)columns;
    }
}

size_t overlay_text_columns(const char *text, size_t length) {
    size_t columns = 0;
    for (size_t offset = 0; offset < length;) {
        size_t consumed;
        int codepoint = decode_utf8((const unsigned char *)text + offset, length - offset, &consumed);
        columns += codepoint == '\t' ? OVERLAY_TAB_WIDTH - columns % OVERLAY_TAB_WIDTH : (size_t)overlay_columns(codepoint);
        offset += consumed;
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
        glUseProgram(image_program);
        glUniform2f(image_screen_location, (float)width, (float)height);
        for (int i = 0; i < image_count; i++) {
            glBindTexture(GL_TEXTURE_2D, images[i].texture);
            glDrawArrays(GL_TRIANGLES, (GLint)(first + 6 * (size_t)i), 6);
        }
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}
