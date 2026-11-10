// The 2D canvas of js/canvas.js (CanvasRenderingContext2D for hydra-text, hydra-canvas and initCanvas),
// drawn with plutovg (MIT, see THIRD_PARTY.md). Fonts come from fontconfig through fc-match.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "canvas2d.h"
#include "native.h"
#include "plutovg.h"
#include "stb_image.h"

#define MAX_CANVASES 256
#define MAX_FONTS 32
#define MAX_FONT_NAME 256

struct canvas {
    plutovg_surface_t *surface;
    plutovg_canvas_t *canvas;
    int width, height;
    // While a layer is open (shadows and blend modes that plutovg lacks), drawing goes to the layer.
    plutovg_surface_t *main_surface;
    plutovg_canvas_t *main_canvas;
    int fetch; // an image that is downloading, else -1
};

struct font {
    char name[MAX_FONT_NAME]; // family, bold and italic as one key
    plutovg_font_face_t *face;
};

static struct canvas canvases[MAX_CANVASES];
static struct font fonts[MAX_FONTS];
static int font_count;
static JSContext *script;
static unsigned char *upload_buffer;
static size_t upload_capacity;

static struct canvas *find(JSContext *ctx, JSValueConst value) {
    int32_t id = -1;
    JS_ToInt32(ctx, &id, value);
    return id >= 0 && id < MAX_CANVASES && canvases[id].canvas ? &canvases[id] : NULL;
}

static float number(JSContext *ctx, JSValueConst value) {
    double result = 0;
    JS_ToFloat64(ctx, &result, value);
    return (float)result;
}

#define CANVAS_FUNCTION(name) static JSValue name(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
#define CANVAS_OR_RETURN()                         \
    struct canvas *canvas = find(ctx, argv[0]);    \
    if (!canvas) return JS_UNDEFINED;              \
    plutovg_canvas_t *vg = canvas->canvas;         \
    (void)vg

static void create_surface(struct canvas *canvas, int width, int height) {
    if (canvas->canvas) plutovg_canvas_destroy(canvas->canvas);
    if (canvas->surface) plutovg_surface_destroy(canvas->surface);
    canvas->width = width > 0 ? width : 1;
    canvas->height = height > 0 ? height : 1;
    canvas->surface = plutovg_surface_create(canvas->width, canvas->height);
    canvas->canvas = plutovg_canvas_create(canvas->surface);
}

static int new_canvas(int width, int height) {
    for (int id = 0; id < MAX_CANVASES; id++) {
        if (canvases[id].canvas) continue;
        create_surface(&canvases[id], width, height);
        canvases[id].fetch = -1;
        return id;
    }
    return -1;
}

CANVAS_FUNCTION(js_canvas_create) {
    int id = new_canvas((int)number(ctx, argv[0]), (int)number(ctx, argv[1]));
    if (id < 0) return JS_ThrowRangeError(ctx, "too many canvases (%d)", MAX_CANVASES);
    return JS_NewInt32(ctx, id);
}

CANVAS_FUNCTION(js_canvas_resize) {
    CANVAS_OR_RETURN();
    create_surface(canvas, (int)number(ctx, argv[1]), (int)number(ctx, argv[2]));
    return JS_UNDEFINED;
}

CANVAS_FUNCTION(js_canvas_destroy) {
    CANVAS_OR_RETURN();
    if (canvas->main_canvas) {
        plutovg_canvas_destroy(canvas->main_canvas);
        plutovg_surface_destroy(canvas->main_surface);
    }
    plutovg_canvas_destroy(canvas->canvas);
    plutovg_surface_destroy(canvas->surface);
    memset(canvas, 0, sizeof *canvas);
    return JS_UNDEFINED;
}

// paint(id, r, g, b, a, lineWidth, lineCap, lineJoin, miterLimit, globalAlpha, operator): the state for the next
// fill or stroke. Caps, joins and operators are the indexes of the plutovg enums (see js/canvas.js).
CANVAS_FUNCTION(js_canvas_paint) {
    CANVAS_OR_RETURN();
    plutovg_canvas_set_rgba(vg, number(ctx, argv[1]), number(ctx, argv[2]), number(ctx, argv[3]), number(ctx, argv[4]));
    plutovg_canvas_set_line_width(vg, number(ctx, argv[5]));
    plutovg_canvas_set_line_cap(vg, (plutovg_line_cap_t)number(ctx, argv[6]));
    plutovg_canvas_set_line_join(vg, (plutovg_line_join_t)number(ctx, argv[7]));
    plutovg_canvas_set_miter_limit(vg, number(ctx, argv[8]));
    plutovg_canvas_set_opacity(vg, number(ctx, argv[9]));
    plutovg_canvas_set_operator(vg, (plutovg_operator_t)number(ctx, argv[10]));
    return JS_UNDEFINED;
}

CANVAS_FUNCTION(js_canvas_save) {
    CANVAS_OR_RETURN();
    plutovg_canvas_save(vg);
    return JS_UNDEFINED;
}

CANVAS_FUNCTION(js_canvas_restore) {
    CANVAS_OR_RETURN();
    plutovg_canvas_restore(vg);
    return JS_UNDEFINED;
}

// setTransform(id, a, b, c, d, e, f) with the matrix of the 2D canvas API.
CANVAS_FUNCTION(js_canvas_set_transform) {
    CANVAS_OR_RETURN();
    plutovg_matrix_t matrix = { number(ctx, argv[1]), number(ctx, argv[2]), number(ctx, argv[3]),
                                number(ctx, argv[4]), number(ctx, argv[5]), number(ctx, argv[6]) };
    plutovg_canvas_set_matrix(vg, &matrix);
    return JS_UNDEFINED;
}

// path(id, [op, ...arguments, op, ...]) with the ops of PATH_* in js/canvas.js. One call per path keeps
// the number of JS to C calls small.
enum path_op { PATH_MOVE, PATH_LINE, PATH_QUAD, PATH_CUBIC, PATH_ARC, PATH_ELLIPSE, PATH_RECT, PATH_CLOSE };

static void ellipse(plutovg_canvas_t *vg, const float *a) {
    // a: cx, cy, rx, ry, rotation, start, end, anticlockwise
    plutovg_matrix_t saved;
    plutovg_canvas_get_matrix(vg, &saved);
    plutovg_canvas_translate(vg, a[0], a[1]);
    plutovg_canvas_rotate(vg, a[4]);
    plutovg_canvas_scale(vg, a[2], a[3]);
    plutovg_canvas_arc(vg, 0, 0, 1, a[5], a[6], a[7] != 0);
    plutovg_canvas_set_matrix(vg, &saved);
}

CANVAS_FUNCTION(js_canvas_path) {
    CANVAS_OR_RETURN();
    static const int argument_counts[] = { 2, 2, 4, 6, 6, 8, 4, 0 };
    JSValue length_value = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    plutovg_canvas_new_path(vg);
    for (int32_t i = 0; i < length;) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i++);
        int op = (int)number(ctx, item);
        JS_FreeValue(ctx, item);
        if (op < 0 || op > PATH_CLOSE) break;
        float a[8] = { 0 };
        for (int k = 0; k < argument_counts[op] && i < length; k++) {
            item = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i++);
            a[k] = number(ctx, item);
            JS_FreeValue(ctx, item);
        }
        switch (op) {
        case PATH_MOVE: plutovg_canvas_move_to(vg, a[0], a[1]); break;
        case PATH_LINE: plutovg_canvas_line_to(vg, a[0], a[1]); break;
        case PATH_QUAD: plutovg_canvas_quad_to(vg, a[0], a[1], a[2], a[3]); break;
        case PATH_CUBIC: plutovg_canvas_cubic_to(vg, a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case PATH_ARC: plutovg_canvas_arc(vg, a[0], a[1], a[2], a[3], a[4], a[5] != 0); break;
        case PATH_ELLIPSE: ellipse(vg, a); break;
        case PATH_RECT: plutovg_canvas_rect(vg, a[0], a[1], a[2], a[3]); break;
        case PATH_CLOSE: plutovg_canvas_close_path(vg); break;
        }
    }
    return JS_UNDEFINED;
}

// fill(id, evenOdd), stroke(id), clip(id, evenOdd): use the path of the last path() call.
CANVAS_FUNCTION(js_canvas_fill) {
    CANVAS_OR_RETURN();
    plutovg_canvas_set_fill_rule(vg, JS_ToBool(ctx, argv[1]) ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_fill_preserve(vg);
    return JS_UNDEFINED;
}

CANVAS_FUNCTION(js_canvas_stroke) {
    CANVAS_OR_RETURN();
    plutovg_canvas_stroke_preserve(vg);
    return JS_UNDEFINED;
}

CANVAS_FUNCTION(js_canvas_clip) {
    CANVAS_OR_RETURN();
    plutovg_canvas_set_fill_rule(vg, JS_ToBool(ctx, argv[1]) ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_clip_preserve(vg);
    return JS_UNDEFINED;
}

// rect(id, kind, x, y, width, height) with kind 0 fill, 1 stroke, 2 clear.
CANVAS_FUNCTION(js_canvas_rect) {
    CANVAS_OR_RETURN();
    float x = number(ctx, argv[2]), y = number(ctx, argv[3]), width = number(ctx, argv[4]), height = number(ctx, argv[5]);
    switch ((int)number(ctx, argv[1])) {
    case 0: plutovg_canvas_fill_rect(vg, x, y, width, height); break;
    case 1: plutovg_canvas_stroke_rect(vg, x, y, width, height); break;
    default:
        plutovg_canvas_save(vg);
        plutovg_canvas_set_operator(vg, PLUTOVG_OPERATOR_CLEAR);
        plutovg_canvas_fill_rect(vg, x, y, width, height);
        plutovg_canvas_restore(vg);
    }
    return JS_UNDEFINED;
}

// Finds the font file of a CSS family with fc-match (generic names like serif work too).
static plutovg_font_face_t *load_font(const char *family, bool bold, bool italic) {
    char key[MAX_FONT_NAME];
    snprintf(key, sizeof key, "%s|%d|%d", family, bold, italic);
    for (int i = 0; i < font_count; i++)
        if (!strcmp(fonts[i].name, key)) return fonts[i].face;
    char *command;
    char quoted[MAX_FONT_NAME * 2];
    size_t used = 0;
    for (const char *c = family; *c && used + 5 < sizeof quoted; c++) {
        if (*c == '\'') {
            memcpy(quoted + used, "'\\''", 4);
            used += 4;
        } else {
            quoted[used++] = *c;
        }
    }
    quoted[used] = '\0';
    if (asprintf(&command, "fc-match -f '%%{file}' '%s:weight=%s:slant=%s'", quoted, bold ? "bold" : "regular",
                 italic ? "italic" : "roman") < 0)
        return NULL;
    FILE *pipe = popen(command, "r");
    free(command);
    char path[1024] = { 0 };
    if (pipe) {
        if (!fgets(path, sizeof path, pipe)) path[0] = '\0';
        pclose(pipe);
    }
    plutovg_font_face_t *face = path[0] ? plutovg_font_face_load_from_file(path, 0) : NULL;
    if (!face) fprintf(stderr, "canvas: no font for %s\n", family);
    if (font_count < MAX_FONTS) {
        snprintf(fonts[font_count].name, sizeof fonts[font_count].name, "%s", key);
        fonts[font_count++].face = face;
    }
    return face;
}

// font(id, family, bold, italic, size)
CANVAS_FUNCTION(js_canvas_font) {
    CANVAS_OR_RETURN();
    const char *family = JS_ToCString(ctx, argv[1]);
    if (!family) return JS_EXCEPTION;
    plutovg_font_face_t *face = load_font(family, JS_ToBool(ctx, argv[2]), JS_ToBool(ctx, argv[3]));
    JS_FreeCString(ctx, family);
    if (face) plutovg_canvas_set_font(vg, face, number(ctx, argv[4]));
    return JS_UNDEFINED;
}

// text(id, string, x, y, stroke) draws at the baseline; returns the advance.
CANVAS_FUNCTION(js_canvas_text) {
    CANVAS_OR_RETURN();
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!text) return JS_EXCEPTION;
    float x = number(ctx, argv[2]), y = number(ctx, argv[3]), advance;
    if (JS_ToBool(ctx, argv[4])) advance = plutovg_canvas_stroke_text(vg, text, (int)length, PLUTOVG_TEXT_ENCODING_UTF8, x, y);
    else advance = plutovg_canvas_fill_text(vg, text, (int)length, PLUTOVG_TEXT_ENCODING_UTF8, x, y);
    JS_FreeCString(ctx, text);
    return JS_NewFloat64(ctx, advance);
}

// measure(id, string) returns [advance, ascent, descent, left, top, right, bottom] of the current font.
CANVAS_FUNCTION(js_canvas_measure) {
    CANVAS_OR_RETURN();
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!text) return JS_EXCEPTION;
    plutovg_rect_t extents = { 0 };
    float advance = plutovg_canvas_text_extents(vg, text, (int)length, PLUTOVG_TEXT_ENCODING_UTF8, &extents);
    JS_FreeCString(ctx, text);
    float ascent = 0, descent = 0, line_gap = 0;
    plutovg_canvas_font_metrics(vg, &ascent, &descent, &line_gap, NULL);
    float values[] = { advance, ascent, descent, extents.x, extents.y, extents.x + extents.w, extents.y + extents.h };
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < sizeof values / sizeof values[0]; i++)
        JS_SetPropertyUint32(ctx, array, i, JS_NewFloat64(ctx, values[i]));
    return array;
}

// drawCanvas(id, sourceId, sx, sy, sw, sh, dx, dy, dw, dh) draws a part of another canvas into a rectangle.
CANVAS_FUNCTION(js_canvas_draw_canvas) {
    CANVAS_OR_RETURN();
    struct canvas *source = find(ctx, argv[1]);
    if (!source) return JS_UNDEFINED;
    float sx = number(ctx, argv[2]), sy = number(ctx, argv[3]), sw = number(ctx, argv[4]), sh = number(ctx, argv[5]);
    float dx = number(ctx, argv[6]), dy = number(ctx, argv[7]), dw = number(ctx, argv[8]), dh = number(ctx, argv[9]);
    if (sw == 0 || sh == 0) return JS_UNDEFINED;
    plutovg_matrix_t matrix;
    plutovg_matrix_init_translate(&matrix, dx, dy);
    plutovg_matrix_scale(&matrix, dw / sw, dh / sh);
    plutovg_matrix_translate(&matrix, -sx, -sy);
    plutovg_canvas_save(vg);
    plutovg_canvas_set_texture(vg, source->surface, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.0f, &matrix);
    plutovg_canvas_fill_rect(vg, dx, dy, dw, dh);
    plutovg_canvas_restore(vg);
    return JS_UNDEFINED;
}

// upload(id, texture) copies the canvas into a GL texture as straight (not premultiplied) RGBA, as WebGL does.
CANVAS_FUNCTION(js_canvas_upload) {
    CANVAS_OR_RETURN();
    int32_t texture = 0;
    JS_ToInt32(ctx, &texture, argv[1]);
    size_t size = (size_t)canvas->width * (size_t)canvas->height * 4;
    if (size > upload_capacity) upload_buffer = realloc(upload_buffer, upload_capacity = size);
    plutovg_convert_argb_to_rgba(upload_buffer, plutovg_surface_get_data(canvas->surface), canvas->width,
                                 canvas->height, plutovg_surface_get_stride(canvas->surface));
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, canvas->width, canvas->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload_buffer);
    return JS_UNDEFINED;
}


// ---------------------------------------------------------------- gradients and patterns

static int read_stops(JSContext *ctx, JSValueConst array, plutovg_gradient_stop_t *stops, int capacity) {
    JSValue length_value = JS_GetPropertyStr(ctx, array, "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    int count = 0;
    for (int32_t i = 0; i + 5 <= length && count < capacity; i += 5, count++) {
        float values[5];
        for (int k = 0; k < 5; k++) {
            JSValue item = JS_GetPropertyUint32(ctx, array, (uint32_t)(i + k));
            values[k] = number(ctx, item);
            JS_FreeValue(ctx, item);
        }
        stops[count] = (plutovg_gradient_stop_t){ values[0], { values[1], values[2], values[3], values[4] } };
    }
    return count;
}

#define MAX_STOPS 64

// gradient(id, kind, x0, y0, r0, x1, y1, r1, [offset, r, g, b, a, ...]) with kind 0 linear, 1 radial:
// the paint of the next fill or stroke, after paint().
CANVAS_FUNCTION(js_canvas_gradient) {
    CANVAS_OR_RETURN();
    plutovg_gradient_stop_t stops[MAX_STOPS];
    int count = read_stops(ctx, argv[8], stops, MAX_STOPS);
    float x0 = number(ctx, argv[2]), y0 = number(ctx, argv[3]), r0 = number(ctx, argv[4]);
    float x1 = number(ctx, argv[5]), y1 = number(ctx, argv[6]), r1 = number(ctx, argv[7]);
    if ((int)number(ctx, argv[1]) == 0)
        plutovg_canvas_set_linear_gradient(vg, x0, y0, x1, y1, PLUTOVG_SPREAD_METHOD_PAD, stops, count, NULL);
    else // the canvas API names the start circle first; plutovg names the end circle first
        plutovg_canvas_set_radial_gradient(vg, x1, y1, r1, x0, y0, r0, PLUTOVG_SPREAD_METHOD_PAD, stops, count, NULL);
    return JS_UNDEFINED;
}

static plutovg_color_t stop_color(const plutovg_gradient_stop_t *stops, int count, float position) {
    if (count == 0) return (plutovg_color_t){ 0, 0, 0, 0 };
    if (position <= stops[0].offset) return stops[0].color;
    for (int i = 1; i < count; i++) {
        if (position > stops[i].offset) continue;
        float span = stops[i].offset - stops[i - 1].offset, t = span > 0 ? (position - stops[i - 1].offset) / span : 0;
        plutovg_color_t a = stops[i - 1].color, b = stops[i].color;
        return (plutovg_color_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    }
    return stops[count - 1].color;
}

static uint32_t premultiplied_argb(plutovg_color_t color, float alpha) {
    float a = color.a * alpha;
    return (uint32_t)(a * 255 + 0.5f) << 24 | (uint32_t)(color.r * a * 255 + 0.5f) << 16 |
           (uint32_t)(color.g * a * 255 + 0.5f) << 8 | (uint32_t)(color.b * a * 255 + 0.5f);
}

static plutovg_surface_t *conic_surfaces[MAX_CANVASES];

// conic(id, startAngle, x, y, [stops]): plutovg has no conic gradient, so a canvas-sized texture gets the colors.
CANVAS_FUNCTION(js_canvas_conic) {
    CANVAS_OR_RETURN();
    plutovg_gradient_stop_t stops[MAX_STOPS];
    int count = read_stops(ctx, argv[4], stops, MAX_STOPS);
    float start = number(ctx, argv[1]), cx = number(ctx, argv[2]), cy = number(ctx, argv[3]);
    int index = (int)(canvas - canvases);
    if (conic_surfaces[index]) plutovg_surface_destroy(conic_surfaces[index]);
    plutovg_surface_t *surface = conic_surfaces[index] = plutovg_surface_create(canvas->width, canvas->height);
    plutovg_matrix_t matrix, inverse;
    plutovg_canvas_get_matrix(vg, &matrix);
    if (!plutovg_matrix_invert(&matrix, &inverse)) return JS_UNDEFINED;
    unsigned char *data = plutovg_surface_get_data(surface);
    int stride = plutovg_surface_get_stride(surface);
    for (int y = 0; y < canvas->height; y++) {
        uint32_t *row = (uint32_t *)(data + (size_t)y * (size_t)stride);
        for (int x = 0; x < canvas->width; x++) {
            float ux, uy;
            plutovg_matrix_map(&inverse, x + 0.5f, y + 0.5f, &ux, &uy);
            float angle = atan2f(uy - cy, ux - cx) - start;
            float turn = angle / (2 * (float)M_PI);
            turn -= floorf(turn);
            row[x] = premultiplied_argb(stop_color(stops, count, turn), 1);
        }
    }
    plutovg_canvas_set_texture(vg, surface, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &inverse);
    return JS_UNDEFINED;
}

// dash(id, offset, [lengths]): the dash pattern of strokes; an empty list draws solid lines.
CANVAS_FUNCTION(js_canvas_dash) {
    CANVAS_OR_RETURN();
    JSValue length_value = JS_GetPropertyStr(ctx, argv[2], "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    float dashes[MAX_STOPS];
    if (length > MAX_STOPS) length = MAX_STOPS;
    for (int32_t i = 0; i < length; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
        dashes[i] = number(ctx, item);
        JS_FreeValue(ctx, item);
    }
    plutovg_canvas_set_dash(vg, number(ctx, argv[1]), dashes, length);
    return JS_UNDEFINED;
}

// pattern(id, sourceId, repeat): the canvas or image `sourceId` tiles the paint.
CANVAS_FUNCTION(js_canvas_pattern) {
    CANVAS_OR_RETURN();
    struct canvas *source = find(ctx, argv[1]);
    if (!source) return JS_UNDEFINED;
    plutovg_canvas_set_texture(vg, source->surface, JS_ToBool(ctx, argv[2]) ? PLUTOVG_TEXTURE_TYPE_TILED : PLUTOVG_TEXTURE_TYPE_PLAIN,
                               1, NULL);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- layers: shadows and blend modes

// layerBegin(id): the next drawing goes to an empty layer with the same transform.
CANVAS_FUNCTION(js_canvas_layer_begin) {
    CANVAS_OR_RETURN();
    if (canvas->main_canvas) return JS_UNDEFINED;
    plutovg_matrix_t matrix;
    plutovg_canvas_get_matrix(vg, &matrix);
    canvas->main_surface = canvas->surface;
    canvas->main_canvas = canvas->canvas;
    canvas->surface = plutovg_surface_create(canvas->width, canvas->height);
    canvas->canvas = plutovg_canvas_create(canvas->surface);
    plutovg_canvas_set_matrix(canvas->canvas, &matrix);
    return JS_UNDEFINED;
}

enum blend_mode {
    BLEND_SOURCE_OVER, BLEND_LIGHTER, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_OVERLAY, BLEND_DARKEN, BLEND_LIGHTEN,
    BLEND_COLOR_DODGE, BLEND_COLOR_BURN, BLEND_HARD_LIGHT, BLEND_SOFT_LIGHT, BLEND_DIFFERENCE, BLEND_EXCLUSION,
    BLEND_HUE, BLEND_SATURATION, BLEND_COLOR, BLEND_LUMINOSITY,
};

static float blend_channel(enum blend_mode mode, float b, float s) {
    switch (mode) {
    case BLEND_MULTIPLY: return b * s;
    case BLEND_SCREEN: return b + s - b * s;
    case BLEND_OVERLAY: return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case BLEND_DARKEN: return fminf(b, s);
    case BLEND_LIGHTEN: return fmaxf(b, s);
    case BLEND_COLOR_DODGE: return b == 0 ? 0 : s >= 1 ? 1 : fminf(1, b / (1 - s));
    case BLEND_COLOR_BURN: return b >= 1 ? 1 : s <= 0 ? 0 : 1 - fminf(1, (1 - b) / s);
    case BLEND_HARD_LIGHT: return s <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case BLEND_SOFT_LIGHT: {
        float d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : sqrtf(b);
        return s <= 0.5f ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * (d - b);
    }
    case BLEND_DIFFERENCE: return fabsf(b - s);
    case BLEND_EXCLUSION: return b + s - 2 * b * s;
    default: return s;
    }
}

static float luminosity(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

static void clip_color(float *c) {
    float l = luminosity(c), n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        if (n < 0) c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}

static void set_luminosity(float *c, float l) {
    float d = l - luminosity(c);
    for (int i = 0; i < 3; i++) c[i] += d;
    clip_color(c);
}

static float saturation(const float *c) { return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2])); }

static void set_saturation(float *c, float s) {
    int max = 0, min = 0;
    for (int i = 1; i < 3; i++) {
        if (c[i] > c[max]) max = i;
        if (c[i] < c[min]) min = i;
    }
    int mid = 3 - max - min;
    if (max == min) mid = (max + 1) % 3;
    if (c[max] > c[min]) {
        c[mid] = (c[mid] - c[min]) * s / (c[max] - c[min]);
        c[max] = s;
    } else {
        c[mid] = c[max] = 0;
    }
    c[min] = 0;
}

// Composites one premultiplied ARGB pixel over another with a W3C blend mode.
static uint32_t composite_pixel(uint32_t destination, uint32_t source, enum blend_mode mode) {
    float as = (float)(source >> 24) / 255, ab = (float)(destination >> 24) / 255;
    if (as == 0) return destination;
    float s[3], b[3], r[3];
    for (int i = 0; i < 3; i++) {
        int shift = 16 - 8 * i;
        s[i] = (float)((source >> shift) & 0xFF) / 255 / as;
        b[i] = ab > 0 ? (float)((destination >> shift) & 0xFF) / 255 / ab : 0;
    }
    if (mode == BLEND_LIGHTER) {
        uint32_t out = 0;
        for (int shift = 0; shift <= 24; shift += 8) {
            unsigned sum = ((source >> shift) & 0xFF) + ((destination >> shift) & 0xFF);
            out |= (sum > 255 ? 255u : sum) << shift;
        }
        return out;
    }
    if (mode >= BLEND_HUE) {
        memcpy(r, mode == BLEND_LUMINOSITY ? b : s, sizeof r);
        if (mode == BLEND_HUE) { set_saturation(r, saturation(b)); set_luminosity(r, luminosity(b)); }
        else if (mode == BLEND_SATURATION) { memcpy(r, b, sizeof r); set_saturation(r, saturation(s)); set_luminosity(r, luminosity(b)); }
        else if (mode == BLEND_COLOR) set_luminosity(r, luminosity(b));
        else set_luminosity(r, luminosity(s));
    } else {
        for (int i = 0; i < 3; i++) r[i] = blend_channel(mode, b[i], s[i]);
    }
    float ao = as + ab * (1 - as);
    uint32_t out = (uint32_t)(ao * 255 + 0.5f) << 24;
    for (int i = 0; i < 3; i++) {
        float mixed = (1 - ab) * s[i] + ab * r[i];
        float co = as * mixed + ab * b[i] * (1 - as);
        out |= (uint32_t)(fminf(co, 1) * 255 + 0.5f) << (16 - 8 * i);
    }
    return out;
}

// A box blur of the alpha channel, three times, close to the Gaussian of the canvas shadow (sigma = blur / 2).
static void blur_alpha(float *alpha, int width, int height, float blur) {
    int radius = (int)(blur / 2 * 0.9f + 0.5f);
    if (radius < 1) return;
    float *line = malloc(sizeof(float) * (size_t)(width > height ? width : height));
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < height; y++) {
            float *row = alpha + (size_t)y * (size_t)width, sum = 0;
            for (int x = -radius; x <= radius; x++) sum += x >= 0 && x < width ? row[x] : 0;
            for (int x = 0; x < width; x++) {
                line[x] = sum / (float)(2 * radius + 1);
                int out = x - radius, in = x + radius + 1;
                sum += (in < width ? row[in] : 0) - (out >= 0 ? row[out] : 0);
            }
            memcpy(row, line, sizeof(float) * (size_t)width);
        }
        for (int x = 0; x < width; x++) {
            float sum = 0;
            for (int y = -radius; y <= radius; y++) sum += y >= 0 && y < height ? alpha[(size_t)y * (size_t)width + (size_t)x] : 0;
            for (int y = 0; y < height; y++) {
                line[y] = sum / (float)(2 * radius + 1);
                int out = y - radius, in = y + radius + 1;
                sum += (in < height ? alpha[(size_t)in * (size_t)width + (size_t)x] : 0) -
                       (out >= 0 ? alpha[(size_t)out * (size_t)width + (size_t)x] : 0);
            }
            for (int y = 0; y < height; y++) alpha[(size_t)y * (size_t)width + (size_t)x] = line[y];
        }
    }
    free(line);
}

// layerEnd(id, blendMode, globalAlpha, shadowBlur, shadowOffsetX, shadowOffsetY, r, g, b, a): composites the
// layer, first its shadow (when the shadow color is not transparent), then the layer itself with the blend mode.
CANVAS_FUNCTION(js_canvas_layer_end) {
    CANVAS_OR_RETURN();
    if (!canvas->main_canvas) return JS_UNDEFINED;
    enum blend_mode mode = (enum blend_mode)(int)number(ctx, argv[1]);
    float shadow_blur = number(ctx, argv[3]), offset_x = number(ctx, argv[4]), offset_y = number(ctx, argv[5]);
    plutovg_color_t shadow = { number(ctx, argv[6]), number(ctx, argv[7]), number(ctx, argv[8]), number(ctx, argv[9]) };
    int width = canvas->width, height = canvas->height;
    unsigned char *layer = plutovg_surface_get_data(canvas->surface), *target = plutovg_surface_get_data(canvas->main_surface);
    int stride = plutovg_surface_get_stride(canvas->surface);
    if (shadow.a > 0) {
        float *alpha = calloc((size_t)width * (size_t)height, sizeof(float));
        int dx = (int)lroundf(offset_x), dy = (int)lroundf(offset_y);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                int sx = x - dx, sy = y - dy;
                if (sx >= 0 && sx < width && sy >= 0 && sy < height)
                    alpha[(size_t)y * (size_t)width + (size_t)x] =
                        (float)(((uint32_t *)(layer + (size_t)sy * (size_t)stride))[sx] >> 24) / 255;
            }
        blur_alpha(alpha, width, height, shadow_blur);
        for (int y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)(target + (size_t)y * (size_t)stride);
            for (int x = 0; x < width; x++) {
                float a = alpha[(size_t)y * (size_t)width + (size_t)x];
                if (a > 0) row[x] = composite_pixel(row[x], premultiplied_argb(shadow, a), mode == BLEND_LIGHTER ? mode : BLEND_SOURCE_OVER);
            }
        }
        free(alpha);
    }
    for (int y = 0; y < height; y++) {
        uint32_t *source = (uint32_t *)(layer + (size_t)y * (size_t)stride);
        uint32_t *row = (uint32_t *)(target + (size_t)y * (size_t)stride);
        for (int x = 0; x < width; x++)
            if (source[x] >> 24) row[x] = composite_pixel(row[x], source[x], mode);
    }
    plutovg_canvas_destroy(canvas->canvas);
    plutovg_surface_destroy(canvas->surface);
    canvas->surface = canvas->main_surface;
    canvas->canvas = canvas->main_canvas;
    canvas->main_surface = NULL;
    canvas->main_canvas = NULL;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- image data

// getImageData(id, x, y, width, height) returns straight RGBA bytes in an ArrayBuffer; outside pixels are 0.
CANVAS_FUNCTION(js_canvas_get_image_data) {
    CANVAS_OR_RETURN();
    int left = (int)number(ctx, argv[1]), top = (int)number(ctx, argv[2]);
    int width = (int)number(ctx, argv[3]), height = (int)number(ctx, argv[4]);
    if (width <= 0 || height <= 0) return JS_ThrowRangeError(ctx, "the size must not be 0");
    size_t size = (size_t)width * (size_t)height * 4;
    unsigned char *pixels = calloc(size, 1);
    const unsigned char *data = plutovg_surface_get_data(canvas->surface);
    int stride = plutovg_surface_get_stride(canvas->surface);
    for (int y = 0; y < height; y++) {
        int sy = top + y;
        if (sy < 0 || sy >= canvas->height) continue;
        for (int x = 0; x < width; x++) {
            int sx = left + x;
            if (sx < 0 || sx >= canvas->width) continue;
            uint32_t pixel = ((const uint32_t *)(data + (size_t)sy * (size_t)stride))[sx];
            unsigned a = pixel >> 24;
            unsigned char *out = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            if (a) {
                out[0] = (unsigned char)((((pixel >> 16) & 0xFF) * 255 + a / 2) / a);
                out[1] = (unsigned char)((((pixel >> 8) & 0xFF) * 255 + a / 2) / a);
                out[2] = (unsigned char)(((pixel & 0xFF) * 255 + a / 2) / a);
            }
            out[3] = (unsigned char)a;
        }
    }
    JSValue buffer = JS_NewArrayBufferCopy(ctx, pixels, size);
    free(pixels);
    return buffer;
}

// putImageData(id, ArrayBuffer, width, height, dx, dy, dirtyX, dirtyY, dirtyWidth, dirtyHeight) writes pixels
// directly, without transform, clip or compositing, as the canvas API does.
CANVAS_FUNCTION(js_canvas_put_image_data) {
    CANVAS_OR_RETURN();
    size_t size;
    uint8_t *pixels = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!pixels) return JS_EXCEPTION;
    int width = (int)number(ctx, argv[2]), height = (int)number(ctx, argv[3]);
    int dx = (int)number(ctx, argv[4]), dy = (int)number(ctx, argv[5]);
    int dirty_x = (int)number(ctx, argv[6]), dirty_y = (int)number(ctx, argv[7]);
    int dirty_width = (int)number(ctx, argv[8]), dirty_height = (int)number(ctx, argv[9]);
    if ((size_t)width * (size_t)height * 4 > size) return JS_ThrowRangeError(ctx, "the image data is too small");
    unsigned char *data = plutovg_surface_get_data(canvas->surface);
    int stride = plutovg_surface_get_stride(canvas->surface);
    for (int y = dirty_y; y < dirty_y + dirty_height && y < height; y++) {
        int ty = dy + y;
        if (y < 0 || ty < 0 || ty >= canvas->height) continue;
        for (int x = dirty_x; x < dirty_x + dirty_width && x < width; x++) {
            int tx = dx + x;
            if (x < 0 || tx < 0 || tx >= canvas->width) continue;
            const uint8_t *in = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            unsigned a = in[3];
            ((uint32_t *)(data + (size_t)ty * (size_t)stride))[tx] =
                a << 24 | (in[0] * a / 255) << 16 | (in[1] * a / 255) << 8 | (in[2] * a / 255);
        }
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- images and video frames

// Copies straight RGBA pixels into a canvas surface (premultiplied ARGB).
static void fill_surface(struct canvas *canvas, const unsigned char *rgba, int width, int height) {
    create_surface(canvas, width, height);
    unsigned char *data = plutovg_surface_get_data(canvas->surface);
    int stride = plutovg_surface_get_stride(canvas->surface);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const unsigned char *in = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4;
            unsigned a = in[3];
            ((uint32_t *)(data + (size_t)y * (size_t)stride))[x] =
                a << 24 | (in[0] * a / 255) << 16 | (in[1] * a / 255) << 8 | (in[2] * a / 255);
        }
}

static void image_done(int id, bool ok) {
    JSValue global = JS_GetGlobalObject(script);
    JSValue function = JS_GetPropertyStr(script, global, "__imageDone");
    JSValue arguments[4] = { JS_NewInt32(script, id), JS_NewBool(script, ok), JS_NewInt32(script, canvases[id].width),
                             JS_NewInt32(script, canvases[id].height) };
    JSValue result = JS_Call(script, function, global, 4, arguments);
    JS_FreeValue(script, result);
    JS_FreeValue(script, function);
    JS_FreeValue(script, global);
}

static bool decode_into(struct canvas *canvas, const unsigned char *data, size_t length) {
    int width, height, channels;
    unsigned char *rgba = stbi_load_from_memory(data, (int)length, &width, &height, &channels, 4);
    if (!rgba) return false;
    fill_surface(canvas, rgba, width, height);
    stbi_image_free(rgba);
    return true;
}

// imageLoad(src) decodes an image file or downloads an image URL; __imageDone(id, ok, width, height) follows.
CANVAS_FUNCTION(js_canvas_image_load) {
    const char *source = JS_ToCString(ctx, argv[0]);
    if (!source) return JS_EXCEPTION;
    int id = new_canvas(1, 1);
    if (id < 0) {
        JS_FreeCString(ctx, source);
        return JS_ThrowRangeError(ctx, "too many canvases (%d)", MAX_CANVASES);
    }
    if (!strncmp(source, "http://", 7) || !strncmp(source, "https://", 8)) {
        canvases[id].fetch = fetch_start(source);
    } else {
        const char *path = !strncmp(source, "file://", 7) ? source + 7 : source;
        FILE *file = fopen(path, "rb");
        bool ok = false;
        if (file) {
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            unsigned char *data = malloc((size_t)size);
            ok = fread(data, 1, (size_t)size, file) == (size_t)size && decode_into(&canvases[id], data, (size_t)size);
            free(data);
            fclose(file);
        }
        canvases[id].fetch = ok ? -2 : -3; // decoded or failed; the JS side reports it after the current code
    }
    JS_FreeCString(ctx, source);
    return JS_NewInt32(ctx, id);
}

// imageState(id): [state, width, height] with state 0 loading, 1 loaded, 2 failed.
CANVAS_FUNCTION(js_canvas_image_state) {
    CANVAS_OR_RETURN();
    JSValue array = JS_NewArray(ctx);
    int state = canvas->fetch >= 0 ? 0 : canvas->fetch == -3 ? 2 : 1;
    JS_SetPropertyUint32(ctx, array, 0, JS_NewInt32(ctx, state));
    JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx, canvas->width));
    JS_SetPropertyUint32(ctx, array, 2, JS_NewInt32(ctx, canvas->height));
    return array;
}

bool canvas2d_fetch_done(int fetch, bool ok, const char *data, size_t length) {
    for (int id = 0; id < MAX_CANVASES; id++) {
        if (!canvases[id].canvas || canvases[id].fetch != fetch) continue;
        bool decoded = ok && decode_into(&canvases[id], (const unsigned char *)data, length);
        canvases[id].fetch = decoded ? -2 : -3;
        image_done(id, decoded);
        return true;
    }
    return false;
}

// videoFrame(id, videoHandle): copies the newest frame of a video source of src/video.c into canvas `id`.
CANVAS_FUNCTION(js_canvas_video_frame) {
    CANVAS_OR_RETURN();
    int32_t handle = -1;
    JS_ToInt32(ctx, &handle, argv[1]);
    int width, height;
    if (!video_size(handle, &width, &height)) return JS_FALSE;
    unsigned char *rgba = malloc((size_t)width * (size_t)height * 4);
    bool ok = video_copy_frame(handle, rgba);
    if (ok) fill_surface(canvas, rgba, width, height);
    free(rgba);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry canvas_functions[] = {
    JS_CFUNC_DEF("canvasCreate", 2, js_canvas_create),
    JS_CFUNC_DEF("canvasResize", 3, js_canvas_resize),
    JS_CFUNC_DEF("canvasDestroy", 1, js_canvas_destroy),
    JS_CFUNC_DEF("canvasPaint", 11, js_canvas_paint),
    JS_CFUNC_DEF("canvasSave", 1, js_canvas_save),
    JS_CFUNC_DEF("canvasRestore", 1, js_canvas_restore),
    JS_CFUNC_DEF("canvasSetTransform", 7, js_canvas_set_transform),
    JS_CFUNC_DEF("canvasPath", 2, js_canvas_path),
    JS_CFUNC_DEF("canvasFill", 2, js_canvas_fill),
    JS_CFUNC_DEF("canvasStroke", 1, js_canvas_stroke),
    JS_CFUNC_DEF("canvasClip", 2, js_canvas_clip),
    JS_CFUNC_DEF("canvasRect", 6, js_canvas_rect),
    JS_CFUNC_DEF("canvasFont", 5, js_canvas_font),
    JS_CFUNC_DEF("canvasText", 5, js_canvas_text),
    JS_CFUNC_DEF("canvasMeasure", 2, js_canvas_measure),
    JS_CFUNC_DEF("canvasDrawCanvas", 10, js_canvas_draw_canvas),
    JS_CFUNC_DEF("canvasUpload", 2, js_canvas_upload),
    JS_CFUNC_DEF("canvasGradient", 9, js_canvas_gradient),
    JS_CFUNC_DEF("canvasConic", 5, js_canvas_conic),
    JS_CFUNC_DEF("canvasPattern", 3, js_canvas_pattern),
    JS_CFUNC_DEF("canvasDash", 3, js_canvas_dash),
    JS_CFUNC_DEF("canvasLayerBegin", 1, js_canvas_layer_begin),
    JS_CFUNC_DEF("canvasLayerEnd", 10, js_canvas_layer_end),
    JS_CFUNC_DEF("canvasGetImageData", 5, js_canvas_get_image_data),
    JS_CFUNC_DEF("canvasPutImageData", 10, js_canvas_put_image_data),
    JS_CFUNC_DEF("canvasImageLoad", 1, js_canvas_image_load),
    JS_CFUNC_DEF("canvasImageState", 1, js_canvas_image_state),
    JS_CFUNC_DEF("canvasVideoFrame", 2, js_canvas_video_frame),
};

void canvas2d_install(JSContext *ctx, JSValue native) {
    script = ctx;
    JS_SetPropertyFunctionList(ctx, native, canvas_functions, sizeof canvas_functions / sizeof canvas_functions[0]);
}
