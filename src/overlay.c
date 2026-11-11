// Glyph rendering uses stb_truetype.h 1.26 (public domain or MIT, see THIRD_PARTY.md), one glyph per code point.
#include "overlay.h"

#include "unicode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

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

static int emoji_font = -1; // the vendored monochrome emoji font

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
static struct vertex *vertices, *sorted_vertices; // in draw order; overlay_end() sorts them by layer
static uint8_t *quad_layers; // the layer of each quad of `vertices`
static enum overlay_layer current_layer = OVERLAY_TOP;
static bool layer_difference[OVERLAY_LAYER_COUNT] = { [OVERLAY_POPUP] = true };
// Images (RGBA textures, such as extension thumbnails) draw after the text, one draw call each.
#define MAX_IMAGES 64
static struct {
    float x, y, width, height;
    unsigned texture;
} images[MAX_IMAGES];
static int image_count;
static bool atlas_full;
static size_t vertex_count, vertex_capacity;

// Layers that overlay_end() composites with blend mode difference (|destination - source|): the autocomplete
// list, and the code when the settings ask for it. KHR_blend_equation_advanced does it in the blending stage
// where the context has it; otherwise a shader reads a copy of the screen below the layer.
#define GL_DIFFERENCE_KHR_VALUE 0x929E
static struct {
    GLuint framebuffer, texture, below; // the layer, and the copy of the screen for the shader
    int texture_width, texture_height;
    GLuint program;
    GLint screen_location;
    bool checked, advanced, coherent;
    void (*barrier)(void);
} difference;

// The hydra output before the overlay, while a popup is open: each popup box first restores it below itself.
static struct {
    GLuint texture;
    int width, height;
} backdrop;

// With KHR_blend_equation_advanced the shader declares the blend equation that it draws with.
static const char *advanced_fragment_source =
    "#version 300 es\n"
    "#extension GL_KHR_blend_equation_advanced : require\n"
    "precision mediump float;\n"
    "layout(blend_support_difference) out;\n"
    "in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D layer;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = texture(layer, v_uv) * v_color; }\n";

static const char *difference_fragment_source =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D layer; uniform sampler2D below;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  vec4 source = texture(layer, v_uv), destination = texture(below, v_uv);\n"
    "  vec3 color = source.a > 0.0 ? source.rgb / source.a : vec3(0.0);\n"
    "  out_color = vec4(mix(destination.rgb, abs(destination.rgb - color), source.a), destination.a);\n"
    "}\n";

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

// A program of the overlay vertex shader and `fragment`, with the attributes of struct vertex.
static GLuint link_program(const char *fragment) {
    GLuint linked = glCreateProgram();
    glAttachShader(linked, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(linked, compile(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(linked, 0, "position");
    glBindAttribLocation(linked, 1, "uv");
    glBindAttribLocation(linked, 2, "color");
    glLinkProgram(linked);
    return linked;
}

static bool load_font(struct font *font, const char *path, int index) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file); // negative for a directory or a pipe
    fseek(file, 0, SEEK_SET);
    font->data = size > 0 ? malloc((size_t)size) : NULL;
    bool read = font->data && fread(font->data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    snprintf(font->path, sizeof font->path, "%s", path);
    font->index = index;
    int offset = read ? stbtt_GetFontOffsetForIndex(font->data, index) : -1;
    // stb_truetype reads no font without outlines, such as Noto Color Emoji with only PNG glyphs.
    if (offset >= 0 && stbtt_InitFont(&font->info, font->data, offset)) {
        font->scale = stbtt_ScaleForPixelHeight(&font->info, current_font_size);
        return true;
    }
    free(font->data);
    font->data = NULL;
    return false;
}

static int glyph_index(struct font *font, int codepoint) { return stbtt_FindGlyphIndex(&font->info, codepoint); }

// Code points that no font has, so that fontconfig is asked once and not in each frame; the oldest one goes first.
#define MAX_MISSING_CODEPOINTS 64
static int missing_codepoints[MAX_MISSING_CODEPOINTS];
static unsigned missing_codepoint_count;

static struct font *find_fallback_font(int codepoint);

// A font with the code point from fontconfig, or NULL.
static struct font *fallback_font(int codepoint) {
    for (unsigned i = 0; i < MAX_MISSING_CODEPOINTS; i++)
        if (missing_codepoints[i] == codepoint) return NULL;
    struct font *font = find_fallback_font(codepoint);
    if (!font) missing_codepoints[missing_codepoint_count++ % MAX_MISSING_CODEPOINTS] = codepoint;
    return font;
}

// Asks fontconfig for a font with the code point and adds it to the fallback fonts.
static struct font *find_fallback_font(int codepoint) {
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
            return glyph_index(&fonts[i], codepoint) ? &fonts[i] : NULL;
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

static bool is_emoji(int codepoint) {
    return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) || (codepoint >= 0x2600 && codepoint <= 0x27BF) ||
           (codepoint >= 0x2B00 && codepoint <= 0x2BFF) || codepoint == 0x203C || codepoint == 0x2049;
}

// The font for a code point: an emoji from the emoji font, the code font, else a fallback font.
static struct font *font_for(int codepoint) {
    if (is_emoji(codepoint) && emoji_font >= 0 && glyph_index(&fonts[emoji_font], codepoint))
        return &fonts[emoji_font];
    if (glyph_index(&fonts[0], codepoint)) return &fonts[0];
    for (int i = 1; i < font_count; i++)
        if (i != emoji_font && glyph_index(&fonts[i], codepoint)) return &fonts[i];
    return fallback_font(codepoint);
}

// Renders the glyph of a code point into the atlas, white with the coverage as alpha.
static struct glyph *render_glyph(struct glyph *slot, int codepoint) {
    struct font *font = font_for(codepoint);
    if (!font) return codepoint == FALLBACK_CODEPOINT ? NULL : overlay_glyph(FALLBACK_CODEPOINT);
    int index = glyph_index(font, codepoint), x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&font->info, index, font->scale, font->scale, &x0, &y0, &x1, &y1);
    int width = x1 > x0 ? x1 - x0 : 0, height = y1 > y0 ? y1 - y0 : 0;
    if (shelf_x + width + GLYPH_PADDING > ATLAS_SIZE) {
        shelf_x = 0;
        shelf_y += shelf_height + GLYPH_PADDING;
        shelf_height = 0;
    }
    if (shelf_y + height + GLYPH_PADDING > ATLAS_GLYPH_HEIGHT) return NULL; // full; overlay_end clears it
    if (width > 0 && height > 0) {
        unsigned char *alpha = calloc((size_t)width * (size_t)height, 1);
        unsigned char *bitmap = malloc((size_t)width * (size_t)height * 4);
        stbtt_MakeGlyphBitmap(&font->info, alpha, width, height, width, font->scale, font->scale, index);
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++) {
            bitmap[i * 4] = bitmap[i * 4 + 1] = bitmap[i * 4 + 2] = 0xFF;
            bitmap[i * 4 + 3] = alpha[i];
        }
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, shelf_x, shelf_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bitmap);
        free(alpha);
        free(bitmap);
    }
    // A glyph of a fallback font sits on the baseline of the code font.
    *slot = (struct glyph){ codepoint, unicode_columns(codepoint), (float)x0, (float)y0, (float)x1, (float)y1,
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

bool overlay_set_font(const char *font_path) {
    struct font font = { 0 };
    if (!load_font(&font, font_path, 0)) return false;
    free(fonts[0].data);
    fonts[0] = font;
    return overlay_set_font_size(current_font_size); // new metrics and an empty atlas
}

bool overlay_init(const char *font_path, const char *emoji_font_path, float font_size) {
    current_font_size = font_size;
    if (!load_font(&fonts[0], font_path, 0)) {
        fprintf(stderr, "overlay: cannot use the font %s\n", font_path);
        return false;
    }
    font_count = 1;
    if (emoji_font_path && load_font(&fonts[font_count], emoji_font_path, 0)) emoji_font = font_count++;
    program = link_program(fragment_source);
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
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vertex),
                          (void *)offsetof(struct vertex, color));
    glBindVertexArray(0);
    return overlay_set_font_size(font_size);
}

float overlay_font_size(void) { return current_font_size; }
const char *overlay_font_path(void) { return fonts[0].path; }
float overlay_char_width(void) { return char_width; }
float overlay_line_height(void) { return line_height; }

void overlay_begin(void) {
    vertex_count = 0;
    image_count = 0;
    current_layer = OVERLAY_TOP;
}

void overlay_set_layer(enum overlay_layer layer) { current_layer = layer; }

void overlay_set_difference(enum overlay_layer layer, bool difference) {
    if (layer != OVERLAY_POPUP) layer_difference[layer] = difference;
}

void overlay_image(float x, float y, float width, float height, unsigned texture) {
    if (image_count < MAX_IMAGES) images[image_count++] = (typeof(images[0])){ x, y, width, height, texture };
}

static void push_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color) {
    if (vertex_count + 6 > vertex_capacity) {
        vertex_capacity = vertex_capacity ? vertex_capacity * 2 : 6 * 4096;
        vertices = realloc(vertices, vertex_capacity * sizeof *vertices);
        sorted_vertices = realloc(sorted_vertices, vertex_capacity * sizeof *sorted_vertices);
        quad_layers = realloc(quad_layers, vertex_capacity / 6);
    }
    quad_layers[vertex_count / 6] = (uint8_t)current_layer;
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

const char *overlay_difference_method(void) {
    if (!difference.checked) return "none";
    return difference.advanced ? "KHR_blend_equation_advanced" : "shader";
}

static void check_difference_support(void) {
    difference.checked = true;
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    difference.advanced = extensions && strstr(extensions, "GL_KHR_blend_equation_advanced");
    difference.coherent = extensions && strstr(extensions, "GL_KHR_blend_equation_advanced_coherent");
    if (difference.advanced && !difference.coherent) {
        difference.barrier = (void (*)(void))SDL_GL_GetProcAddress("glBlendBarrierKHR");
        if (!difference.barrier) difference.advanced = false;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        difference.program = link_program(difference.advanced ? advanced_fragment_source : difference_fragment_source);
        GLint linked;
        glGetProgramiv(difference.program, GL_LINK_STATUS, &linked);
        if (linked || !difference.advanced) break;
        glDeleteProgram(difference.program); // the driver names the extension but does not compile its shaders
        difference.advanced = false;
    }
    difference.screen_location = glGetUniformLocation(difference.program, "screen");
    glUseProgram(difference.program);
    glUniform1i(glGetUniformLocation(difference.program, "layer"), 0);
    glUniform1i(glGetUniformLocation(difference.program, "below"), 1);
}

static GLuint window_texture(int width, int height) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

// Draws the vertices [first, end) into a window-sized texture (premultiplied), then composites their bounding
// rectangle onto the window with |destination - source|. The shader path copies the window below that rectangle.
static void draw_difference_layer(size_t first, size_t end, int width, int height) {
    if (!difference.checked) check_difference_support();
    if (difference.texture_width != width || difference.texture_height != height) {
        if (difference.texture) {
            glDeleteTextures(1, &difference.texture);
            glDeleteTextures(1, &difference.below);
        }
        difference.texture = window_texture(width, height);
        difference.below = window_texture(width, height);
        if (!difference.framebuffer) glGenFramebuffers(1, &difference.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, difference.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, difference.texture, 0);
        difference.texture_width = width;
        difference.texture_height = height;
    }
    float x0 = (float)width, y0 = (float)height, x1 = 0, y1 = 0;
    for (size_t i = first; i < end; i++) {
        x0 = fminf(x0, vertices[i].x);
        y0 = fminf(y0, vertices[i].y);
        x1 = fmaxf(x1, vertices[i].x);
        y1 = fmaxf(y1, vertices[i].y);
    }
    x0 = floorf(fmaxf(x0, 0));
    y0 = floorf(fmaxf(y0, 0));
    x1 = ceilf(fminf(x1, (float)width));
    y1 = ceilf(fminf(y1, (float)height));
    if (x1 <= x0 || y1 <= y0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, difference.framebuffer);
    glEnable(GL_SCISSOR_TEST); // only the rectangle of the layer is cleared and drawn
    glScissor((GLint)x0, height - (GLint)y1, (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glDrawArrays(GL_TRIANGLES, (GLint)first, (GLsizei)(end - first));
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    // The quad of the layer rectangle; v counts from the bottom, as the rows of the textures do.
    size_t quad = vertex_count;
    push_quad(x0, y0, x1, y1, x0 / (float)width, 1 - y0 / (float)height, x1 / (float)width, 1 - y1 / (float)height,
              0xFFFFFFFFu);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof *vertices), vertices, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, difference.texture);
    glUseProgram(difference.program);
    glUniform2f(difference.screen_location, (float)width, (float)height);
    if (difference.advanced) {
        glBlendEquation(GL_DIFFERENCE_KHR_VALUE);
        if (difference.barrier) difference.barrier();
        glDrawArrays(GL_TRIANGLES, (GLint)quad, 6);
        glBlendEquation(GL_FUNC_ADD);
    } else {
        int left = (int)x0 < 0 ? 0 : (int)x0, bottom = height - (int)y1 < 0 ? 0 : height - (int)y1;
        int copy_width = (int)x1 - left, copy_height = (height - (int)y0) - bottom;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, difference.below);
        if (copy_width > 0 && copy_height > 0)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, left, bottom, left, bottom, copy_width, copy_height);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, (GLint)quad, 6);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_BLEND);
    }
    vertex_count = quad;
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, atlas);
}

void overlay_rect(float x, float y, float width, float height, uint32_t color) {
    const float white = (ATLAS_SIZE - 1.0f) / ATLAS_SIZE;
    push_quad(x, y, x + width, y + height, 0.5f / ATLAS_SIZE, white, 0.5f / ATLAS_SIZE, white, color);
}

// ---------------------------------------------------------------- text

// Draws (or only measures, without `draw`) code points one after the other with the advances of their fonts,
// scaled by `scale`; returns the width. There is no shaping: a letter and its combining mark, or the parts of an
// emoji sequence, are separate glyphs.
static float text_run(float x, float y, const char *text, size_t length, uint32_t color, float scale, bool draw) {
    float pen_x = x;
    for (size_t offset = 0; offset < length;) {
        size_t consumed;
        int codepoint = unicode_decode((const unsigned char *)text + offset, length - offset, &consumed);
        offset += consumed;
        if (codepoint == 0xFFFD) codepoint = FALLBACK_CODEPOINT;
        struct font *font = font_for(codepoint);
        // A joiner or a variation selector that no font has adds nothing.
        if (!font && (codepoint == 0x200D || unicode_break_class(codepoint) == BREAK_EXTEND)) continue;
        if (!font) {
            font = &fonts[0];
            codepoint = FALLBACK_CODEPOINT;
        }
        int advance, bearing;
        stbtt_GetGlyphHMetrics(&font->info, glyph_index(font, codepoint), &advance, &bearing);
        struct glyph *glyph = draw && codepoint > ' ' ? overlay_glyph(codepoint) : NULL;
        if (glyph)
            push_quad(pen_x + glyph->x0 * scale, y + ascent + glyph->y0 * scale, pen_x + glyph->x1 * scale,
                      y + ascent + glyph->y1 * scale, glyph->s0, glyph->t0, glyph->s1, glyph->t1, color);
        else if (draw && codepoint > ' ') atlas_full = true;
        pen_x += (float)advance * font->scale * scale;
    }
    return pen_x - x;
}

float overlay_text_width(const char *text, size_t length) { return text_run(0, 0, text, length, 0, 1, false); }

float overlay_text_run(float x, float y, const char *text, size_t length, uint32_t color) {
    return text_run(x, y, text, length, color, 1, true);
}

void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {
    size_t column = 0;
    for (size_t offset = 0; offset < length && column < first_column + max_columns;) {
        size_t consumed, end = unicode_cluster_end((const unsigned char *)text, length, offset);
        int codepoint = unicode_decode((const unsigned char *)text + offset, length - offset, &consumed);
        if (codepoint == 0xFFFD) codepoint = FALLBACK_CODEPOINT;
        int columns = codepoint == '\t' ? OVERLAY_TAB_WIDTH - (int)(column % OVERLAY_TAB_WIDTH)
                                         : unicode_cluster_columns((const unsigned char *)text, offset, end);
        float pen_x = x + (float)(column - first_column) * char_width;
        uint32_t glyph_color = colors ? colors[offset] : color;
        if (column >= first_column && end > offset + consumed) {
            // A cluster of several code points draws them in a row, shrunk to its cells when it is wider.
            float width = overlay_text_width(text + offset, end - offset), cell = (float)columns * char_width;
            text_run(pen_x, y, text + offset, end - offset, glyph_color, width > cell ? cell / width : 1, true);
        } else if (column >= first_column && codepoint > ' ') {
            struct glyph *glyph = overlay_glyph(codepoint);
            if (glyph)
                push_quad(pen_x + glyph->x0, y + ascent + glyph->y0, pen_x + glyph->x1, y + ascent + glyph->y1,
                          glyph->s0, glyph->t0, glyph->s1, glyph->t1, glyph_color);
            else atlas_full = true;
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

static void draw_images(void);

static void copy_backdrop(int width, int height) {
    if (backdrop.width != width || backdrop.height != height) {
        if (backdrop.texture) glDeleteTextures(1, &backdrop.texture);
        backdrop.texture = window_texture(width, height);
        backdrop.width = width;
        backdrop.height = height;
    }
    glBindTexture(GL_TEXTURE_2D, backdrop.texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    glBindTexture(GL_TEXTURE_2D, atlas);
}

// Draws the backdrop into the rectangle of each quad of [first, end) with the alpha of the quad, over the code below
// the popup boxes.
static void restore_backdrop(size_t first, size_t end, int width, int height) {
    size_t quad = vertex_count;
    for (size_t i = first; i < end; i += 6) {
        float x0 = vertices[i].x, y0 = vertices[i].y, x1 = vertices[i + 2].x, y1 = vertices[i + 2].y;
        push_quad(x0, y0, x1, y1, x0 / (float)width, 1 - y0 / (float)height, x1 / (float)width, 1 - y1 / (float)height,
                  0xFFFFFF00u | vertices[i].color[3]);
    }
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof *vertices), vertices, GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_2D, backdrop.texture);
    glDrawArrays(GL_TRIANGLES, (GLint)quad, (GLsizei)(vertex_count - quad));
    glBindTexture(GL_TEXTURE_2D, atlas);
    vertex_count = quad;
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
    // A stable sort of the quads by layer, so that each layer is one range in draw order.
    size_t starts[OVERLAY_LAYER_COUNT + 1] = { 0 }, quads = vertex_count / 6;
    for (size_t q = 0; q < quads; q++) starts[quad_layers[q] + 1] += 6;
    for (int layer = 0; layer < OVERLAY_LAYER_COUNT; layer++) starts[layer + 1] += starts[layer];
    size_t next[OVERLAY_LAYER_COUNT];
    memcpy(next, starts, sizeof next);
    for (size_t q = 0; q < quads; q++) {
        memcpy(sorted_vertices + next[quad_layers[q]], vertices + 6 * q, 6 * sizeof *vertices);
        next[quad_layers[q]] += 6;
    }
    struct vertex *swap = vertices;
    vertices = sorted_vertices;
    sorted_vertices = swap;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof *vertices), vertices, GL_STREAM_DRAW);
    bool popup = starts[OVERLAY_POPUP + 1] > starts[OVERLAY_POPUP];
    if (popup) copy_backdrop(width, height);
    for (int layer = 0; layer < OVERLAY_LAYER_COUNT; layer++) {
        if (layer == OVERLAY_POPUP) draw_images();
        size_t first = starts[layer], end = starts[layer + 1];
        if (end == first) continue;
        if (layer == OVERLAY_POPUP) restore_backdrop(first, end, width, height);
        if (layer_difference[layer]) draw_difference_layer(first, end, width, height);
        else glDrawArrays(GL_TRIANGLES, (GLint)first, (GLsizei)(end - first));
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

// Images draw after the top layer, one draw call each, from quads after the vertices of the layers.
static void draw_images(void) {
    if (image_count) {
        size_t first = vertex_count;
        for (int i = 0; i < image_count; i++)
            push_quad(images[i].x, images[i].y, images[i].x + images[i].width, images[i].y + images[i].height, 0, 0, 1,
                      1,
                      0xFFFFFFFFu);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof *vertices), vertices, GL_STREAM_DRAW);
        for (int i = 0; i < image_count; i++) {
            glBindTexture(GL_TEXTURE_2D, images[i].texture);
            glDrawArrays(GL_TRIANGLES, (GLint)(first + 6 * (size_t)i), 6);
        }
        vertex_count = first;
        glBindTexture(GL_TEXTURE_2D, atlas);
    }
}
