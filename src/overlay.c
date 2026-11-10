// Glyph rendering uses stb_truetype.h 1.26 (public domain or MIT, see THIRD_PARTY.md).
#include "overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define ATLAS_SIZE 1024
// The last two rows of the atlas stay white; rectangles sample them, so text and rectangles share one shader.
#define ATLAS_PACK_HEIGHT (ATLAS_SIZE - 2)
#define FALLBACK_CODEPOINT '?'
#define LINE_SPACING 1.25f

struct glyph_range {
    int first;
    int count;
};

static const struct glyph_range glyph_ranges[] = {
    { 32, 95 },      // ASCII
    { 160, 224 },    // Latin-1 and Latin Extended-A
    { 0x2010, 24 },  // dashes, quotes, bullet, ellipsis
    { 0x20AC, 1 },   // euro sign
    { 0x2190, 4 },   // arrows
};
#define GLYPH_RANGE_COUNT (sizeof glyph_ranges / sizeof glyph_ranges[0])

struct vertex {
    float x, y, u, v;
    uint8_t color[4];
};

static unsigned char *font_data;
static stbtt_packedchar *packed[GLYPH_RANGE_COUNT];
static float current_font_size, char_width, ascent, line_height;
static GLuint atlas, program, vertex_array, vertex_buffer;
static GLint screen_location;
static struct vertex *vertices;
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

bool overlay_set_font_size(float font_size) {
    if (font_size < OVERLAY_MIN_FONT_SIZE) font_size = OVERLAY_MIN_FONT_SIZE;
    if (font_size > OVERLAY_MAX_FONT_SIZE) font_size = OVERLAY_MAX_FONT_SIZE;
    unsigned char *pixels = calloc(ATLAS_SIZE, ATLAS_SIZE);
    memset(pixels + ATLAS_PACK_HEIGHT * ATLAS_SIZE, 0xFF, 2 * ATLAS_SIZE);
    stbtt_pack_context context;
    bool ok = stbtt_PackBegin(&context, pixels, ATLAS_SIZE, ATLAS_PACK_HEIGHT, ATLAS_SIZE, 1, NULL);
    for (size_t i = 0; ok && i < GLYPH_RANGE_COUNT; i++) {
        free(packed[i]);
        packed[i] = calloc((size_t)glyph_ranges[i].count, sizeof(stbtt_packedchar));
        ok = stbtt_PackFontRange(&context, font_data, 0, font_size, glyph_ranges[i].first, glyph_ranges[i].count,
                                 packed[i]);
    }
    if (ok) stbtt_PackEnd(&context);
    if (!ok) {
        fprintf(stderr, "overlay: font size %.0f does not fit the glyph atlas\n", font_size);
        free(pixels);
        return false;
    }

    stbtt_fontinfo info;
    stbtt_InitFont(&info, font_data, stbtt_GetFontOffsetForIndex(font_data, 0));
    float scale = stbtt_ScaleForPixelHeight(&info, font_size);
    int font_ascent, font_descent, line_gap, advance, bearing;
    stbtt_GetFontVMetrics(&info, &font_ascent, &font_descent, &line_gap);
    stbtt_GetCodepointHMetrics(&info, 'M', &advance, &bearing);
    current_font_size = font_size;
    char_width = (float)(int)(advance * scale + 0.5f);
    ascent = (float)(int)(font_ascent * scale + 0.5f);
    line_height = (float)(int)(font_size * LINE_SPACING + 0.5f);

    if (!atlas) glGenTextures(1, &atlas);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    return true;
}

bool overlay_init(const char *font_path, float font_size) {
    FILE *file = fopen(font_path, "rb");
    if (!file) {
        fprintf(stderr, "overlay: cannot open font %s\n", font_path);
        return false;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    font_data = malloc((size_t)size);
    bool read = fread(font_data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read || stbtt_GetFontOffsetForIndex(font_data, 0) < 0) {
        fprintf(stderr, "overlay: %s is no TrueType or OpenType font\n", font_path);
        return false;
    }

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

void overlay_begin(void) { vertex_count = 0; }

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

static const stbtt_packedchar *find_glyph(int codepoint, int *range_index, int *index) {
    for (int range = 0; range < (int)GLYPH_RANGE_COUNT; range++) {
        int offset = codepoint - glyph_ranges[range].first;
        if (offset >= 0 && offset < glyph_ranges[range].count) {
            *range_index = range;
            *index = offset;
            return &packed[range][offset];
        }
    }
    return codepoint == FALLBACK_CODEPOINT ? NULL : find_glyph(FALLBACK_CODEPOINT, range_index, index);
}

static int decode_utf8(const unsigned char *text, size_t length, size_t *consumed) {
    unsigned char first = text[0];
    int count = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
    if ((size_t)count > length || (first >= 0x80 && first < 0xC0)) {
        *consumed = 1;
        return FALLBACK_CODEPOINT;
    }
    int codepoint = count == 1 ? first : first & (0x3F >> (count - 1));
    for (int i = 1; i < count; i++) codepoint = (codepoint << 6) | (text[i] & 0x3F);
    *consumed = (size_t)count;
    return codepoint;
}

void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {
    size_t column = 0;
    for (size_t offset = 0; offset < length && column < first_column + max_columns;) {
        size_t consumed;
        int codepoint = decode_utf8((const unsigned char *)text + offset, length - offset, &consumed);
        if (column >= first_column && codepoint > ' ') {
            int range, index;
            if (find_glyph(codepoint, &range, &index)) {
                float pen_x = x + (float)(column - first_column) * char_width, pen_y = y + ascent;
                stbtt_aligned_quad quad;
                stbtt_GetPackedQuad(packed[range], ATLAS_SIZE, ATLAS_SIZE, index, &pen_x, &pen_y, &quad, 1);
                push_quad(quad.x0, quad.y0, quad.x1, quad.y1, quad.s0, quad.t0, quad.s1, quad.t1,
                          colors ? colors[offset] : color);
            }
        }
        offset += consumed;
        column += codepoint == '\t' ? OVERLAY_TAB_WIDTH - column % OVERLAY_TAB_WIDTH : 1;
    }
}

void overlay_end(int width, int height) {
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
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}
