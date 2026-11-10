// The 2D canvas of js/canvas.js (CanvasRenderingContext2D for hydra-text, hydra-canvas and initCanvas),
// drawn with plutovg (MIT, see THIRD_PARTY.md). Fonts come from fontconfig through fc-match.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "canvas2d.h"
#include "plutovg.h"

#define MAX_CANVASES 256
#define MAX_FONTS 32
#define MAX_FONT_NAME 256

struct canvas {
    plutovg_surface_t *surface;
    plutovg_canvas_t *canvas;
    int width, height;
};

struct font {
    char name[MAX_FONT_NAME]; // family, bold and italic as one key
    plutovg_font_face_t *face;
};

static struct canvas canvases[MAX_CANVASES];
static struct font fonts[MAX_FONTS];
static int font_count;
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

CANVAS_FUNCTION(js_canvas_create) {
    for (int id = 0; id < MAX_CANVASES; id++) {
        if (canvases[id].canvas) continue;
        create_surface(&canvases[id], (int)number(ctx, argv[0]), (int)number(ctx, argv[1]));
        return JS_NewInt32(ctx, id);
    }
    return JS_ThrowRangeError(ctx, "too many canvases (%d)", MAX_CANVASES);
}

CANVAS_FUNCTION(js_canvas_resize) {
    CANVAS_OR_RETURN();
    create_surface(canvas, (int)number(ctx, argv[1]), (int)number(ctx, argv[2]));
    return JS_UNDEFINED;
}

CANVAS_FUNCTION(js_canvas_destroy) {
    CANVAS_OR_RETURN();
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
};

void canvas2d_install(JSContext *ctx, JSValue native) {
    JS_SetPropertyFunctionList(ctx, native, canvas_functions, sizeof canvas_functions / sizeof canvas_functions[0]);
}
