// WebGL 1 on the GLES 3 context of hydra-native, for p5.js in WEBGL mode (js/webgl.js wraps these calls).
// Each WebGL context draws into a framebuffer of its own (color texture, depth and stencil) and keeps its vertex
// state in a vertex array object of its own, so that the hydra passes and the overlay keep theirs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "canvas2d.h"
#include "native.h"
#include "webgl.h"

#define MAX_CONTEXTS 16
#define MAX_LOG 4096
// WebGL constants that GLES does not have.
#define WEBGL_UNPACK_FLIP_Y 0x9240
#define WEBGL_UNPACK_PREMULTIPLY_ALPHA 0x9241
#define WEBGL_UNPACK_COLORSPACE_CONVERSION 0x9243

#define MAX_TEXTURE_UNITS 32
#define CAPABILITY_COUNT 8
static const GLenum capabilities[CAPABILITY_COUNT] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST,
                                                       GL_STENCIL_TEST, GL_POLYGON_OFFSET_FILL, GL_DITHER,
                                                       GL_SAMPLE_ALPHA_TO_COVERAGE };

// The GL state of a WebGL context outside of its vertex array object; hydra changes it between the calls of
// p5, so activate() sets it again.
struct state {
    bool enabled[CAPABILITY_COUNT];
    GLint viewport[4], scissor[4];
    GLenum blend_equation_rgb, blend_equation_alpha, blend_source_rgb, blend_destination_rgb, blend_source_alpha,
        blend_destination_alpha;
    float blend_color[4], clear_color[4], clear_depth, line_width, depth_range[2], polygon_offset[2];
    GLint clear_stencil;
    GLenum depth_function, cull_face, front_face;
    bool depth_mask, color_mask[4];
    GLenum stencil_function, stencil_fail, stencil_depth_fail, stencil_pass;
    GLint stencil_reference;
    GLuint stencil_value_mask, stencil_write_mask;
    GLuint program, array_buffer, renderbuffer, framebuffer;
    GLenum active_texture;
    GLuint textures[MAX_TEXTURE_UNITS];
};

struct context {
    bool used;
    GLuint framebuffer, texture, depth_stencil, vertex_array;
    int width, height;
    bool flip_y, premultiply_alpha;
    GLint unpack_alignment;
    struct state state;
};

static struct context contexts[MAX_CONTEXTS];
static int current = -1;

static int to_int(JSContext *ctx, JSValueConst value) {
    int32_t result = 0;
    JS_ToInt32(ctx, &result, value);
    return result;
}

static float to_float(JSContext *ctx, JSValueConst value) {
    double result = 0;
    JS_ToFloat64(ctx, &result, value);
    return (float)result;
}

void webgl_release(void) {
    if (current < 0) return;
    current = -1;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void allocate(struct context *context) {
    glBindTexture(GL_TEXTURE_2D, context->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, context->width, context->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, context->depth_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, context->width, context->height);
    glBindFramebuffer(GL_FRAMEBUFFER, context->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, context->texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, context->depth_stencil);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void default_state(struct context *context) {
    struct state *state = &context->state;
    memset(state, 0, sizeof *state);
    state->enabled[6] = true; // GL_DITHER
    state->viewport[2] = state->scissor[2] = context->width;
    state->viewport[3] = state->scissor[3] = context->height;
    state->blend_equation_rgb = state->blend_equation_alpha = GL_FUNC_ADD;
    state->blend_source_rgb = state->blend_source_alpha = GL_ONE;
    state->blend_destination_rgb = state->blend_destination_alpha = GL_ZERO;
    state->clear_depth = state->line_width = state->depth_range[1] = 1;
    state->depth_function = GL_LESS;
    state->cull_face = GL_BACK;
    state->front_face = GL_CCW;
    state->depth_mask = true;
    for (int i = 0; i < 4; i++) state->color_mask[i] = true;
    state->stencil_function = GL_ALWAYS;
    state->stencil_fail = state->stencil_depth_fail = state->stencil_pass = GL_KEEP;
    state->stencil_value_mask = state->stencil_write_mask = 0xFFFFFFFFu;
    state->active_texture = GL_TEXTURE0;
}

static void apply_state(const struct context *context) {
    const struct state *state = &context->state;
    for (int i = 0; i < CAPABILITY_COUNT; i++) (state->enabled[i] ? glEnable : glDisable)(capabilities[i]);
    glViewport(state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
    glScissor(state->scissor[0], state->scissor[1], state->scissor[2], state->scissor[3]);
    glBlendEquationSeparate(state->blend_equation_rgb, state->blend_equation_alpha);
    glBlendFuncSeparate(state->blend_source_rgb, state->blend_destination_rgb, state->blend_source_alpha,
                        state->blend_destination_alpha);
    glBlendColor(state->blend_color[0], state->blend_color[1], state->blend_color[2], state->blend_color[3]);
    glClearColor(state->clear_color[0], state->clear_color[1], state->clear_color[2], state->clear_color[3]);
    glClearDepthf(state->clear_depth);
    glClearStencil(state->clear_stencil);
    glDepthFunc(state->depth_function);
    glDepthMask(state->depth_mask);
    glDepthRangef(state->depth_range[0], state->depth_range[1]);
    glColorMask(state->color_mask[0], state->color_mask[1], state->color_mask[2], state->color_mask[3]);
    glCullFace(state->cull_face);
    glFrontFace(state->front_face);
    glLineWidth(state->line_width);
    glPolygonOffset(state->polygon_offset[0], state->polygon_offset[1]);
    glStencilFunc(state->stencil_function, state->stencil_reference, state->stencil_value_mask);
    glStencilOp(state->stencil_fail, state->stencil_depth_fail, state->stencil_pass);
    glStencilMask(state->stencil_write_mask);
    glUseProgram(state->program);
    glBindBuffer(GL_ARRAY_BUFFER, state->array_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, state->renderbuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, state->framebuffer ? state->framebuffer : context->framebuffer);
    for (int unit = MAX_TEXTURE_UNITS - 1; unit >= 0; unit--) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        glBindTexture(GL_TEXTURE_2D, state->textures[unit]);
    }
    glActiveTexture(state->active_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, context->unpack_alignment);
}

// Makes context `id` current: its framebuffer stands for the default framebuffer of WebGL.
static struct context *activate(JSContext *ctx, JSValueConst value) {
    int id = to_int(ctx, value);
    if (id < 0 || id >= MAX_CONTEXTS || !contexts[id].used) return NULL;
    if (current != id) {
        current = id;
        glBindVertexArray(contexts[id].vertex_array);
        apply_state(&contexts[id]);
    }
    return &contexts[id];
}

static void set_capability(struct state *state, GLenum capability, bool on) {
    for (int i = 0; i < CAPABILITY_COUNT; i++)
        if (capabilities[i] == capability) state->enabled[i] = on;
}

#define WEBGL_FUNCTION(name) static JSValue name(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
#define ACTIVATE()                                                  \
    struct context *context = activate(ctx, argv[0]);              \
    if (!context) return JS_ThrowInternalError(ctx, "no WebGL context"); \
    (void)context
#define INT(n) to_int(ctx, argv[n])
#define FLOAT(n) to_float(ctx, argv[n])

WEBGL_FUNCTION(js_context_create) {
    for (int id = 0; id < MAX_CONTEXTS; id++) {
        if (contexts[id].used) continue;
        struct context *context = &contexts[id];
        *context = (struct context){ .used = true, .width = INT(0) > 0 ? INT(0) : 1, .height = INT(1) > 0 ? INT(1) : 1,
                                     .unpack_alignment = 4 };
        glGenFramebuffers(1, &context->framebuffer);
        glGenTextures(1, &context->texture);
        glGenRenderbuffers(1, &context->depth_stencil);
        glGenVertexArrays(1, &context->vertex_array);
        allocate(context);
        default_state(context);
        current = -1;
        webgl_release();
        return JS_NewInt32(ctx, id);
    }
    return JS_ThrowRangeError(ctx, "too many WebGL contexts (%d)", MAX_CONTEXTS);
}

WEBGL_FUNCTION(js_context_resize) {
    ACTIVATE();
    context->width = INT(1) > 0 ? INT(1) : 1;
    context->height = INT(2) > 0 ? INT(2) : 1;
    allocate(context);
    return JS_UNDEFINED;
}

WEBGL_FUNCTION(js_context_destroy) {
    ACTIVATE();
    webgl_release();
    glDeleteFramebuffers(1, &context->framebuffer);
    glDeleteTextures(1, &context->texture);
    glDeleteRenderbuffers(1, &context->depth_stencil);
    glDeleteVertexArrays(1, &context->vertex_array);
    context->used = false;
    return JS_UNDEFINED;
}

// copyTo(id, texture): the picture of the context into a texture of hydra, upside down, because WebGL draws
// from the bottom row while canvas images (and hydra's sources) start with the top row.
WEBGL_FUNCTION(js_context_copy_to) {
    ACTIVATE();
    GLuint texture = (GLuint)INT(1), target;
    webgl_release();
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, context->width, context->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &target);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, context->framebuffer);
    glBlitFramebuffer(0, 0, context->width, context->height, 0, context->height, context->width, 0, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &target);
    return JS_UNDEFINED;
}

// readInto(id, canvasId, premultipliedAlpha): copies the drawing buffer into a 2D canvas (rows from the top), for
// drawImage, patterns and texImage2D of a WebGL canvas.
WEBGL_FUNCTION(js_context_read_into) {
    ACTIVATE();
    GLint read_framebuffer;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, context->framebuffer);
    unsigned char *pixels = malloc((size_t)context->width * (size_t)context->height * 4);
    glReadPixels(0, 0, context->width, context->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_framebuffer);
    canvas2d_put_rgba(INT(1), pixels, context->width, context->height, JS_ToBool(ctx, argv[2]), true);
    free(pixels);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- objects

#define CREATE(name, call)                     \
    WEBGL_FUNCTION(name) {                     \
        ACTIVATE();                            \
        GLuint object = 0;                     \
        call;                                  \
        return JS_NewInt32(ctx, (int)object);  \
    }

CREATE(js_create_buffer, glGenBuffers(1, &object))
CREATE(js_create_texture, glGenTextures(1, &object))
CREATE(js_create_framebuffer, glGenFramebuffers(1, &object))
CREATE(js_create_renderbuffer, glGenRenderbuffers(1, &object))
CREATE(js_create_program, object = glCreateProgram())
CREATE(js_create_shader, object = glCreateShader((GLenum)INT(1)))

#define DELETE(name, call)                     \
    WEBGL_FUNCTION(name) {                     \
        ACTIVATE();                            \
        GLuint object = (GLuint)INT(1);        \
        call;                                  \
        return JS_UNDEFINED;                   \
    }

DELETE(js_delete_buffer, glDeleteBuffers(1, &object))
DELETE(js_delete_texture, glDeleteTextures(1, &object))
DELETE(js_delete_framebuffer, glDeleteFramebuffers(1, &object))
DELETE(js_delete_renderbuffer, glDeleteRenderbuffers(1, &object))
DELETE(js_delete_program, glDeleteProgram(object))
DELETE(js_delete_shader, glDeleteShader(object))

// bindFramebuffer(id, target, framebuffer): framebuffer 0 is the context's own framebuffer.
WEBGL_FUNCTION(js_bind_framebuffer) {
    ACTIVATE();
    GLuint framebuffer = (GLuint)INT(2);
    glBindFramebuffer((GLenum)INT(1), framebuffer ? framebuffer : context->framebuffer);
    context->state.framebuffer = framebuffer;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- calls with numbers only

// call(id, functionIndex, ...numbers): the WebGL calls whose arguments are all numbers or object ids.
enum simple_call {
    CALL_ACTIVE_TEXTURE, CALL_ATTACH_SHADER, CALL_BIND_BUFFER, CALL_BIND_TEXTURE, CALL_BIND_RENDERBUFFER,
    CALL_BLEND_COLOR, CALL_BLEND_EQUATION, CALL_BLEND_EQUATION_SEPARATE, CALL_BLEND_FUNC, CALL_BLEND_FUNC_SEPARATE,
    CALL_CLEAR, CALL_CLEAR_COLOR, CALL_CLEAR_DEPTH, CALL_CLEAR_STENCIL, CALL_COLOR_MASK, CALL_COMPILE_SHADER,
    CALL_CULL_FACE, CALL_DEPTH_FUNC, CALL_DEPTH_MASK, CALL_DEPTH_RANGE, CALL_DETACH_SHADER, CALL_DISABLE,
    CALL_DISABLE_VERTEX_ATTRIB_ARRAY, CALL_DRAW_ARRAYS, CALL_DRAW_ELEMENTS, CALL_ENABLE, CALL_ENABLE_VERTEX_ATTRIB_ARRAY,
    CALL_FINISH, CALL_FLUSH, CALL_FRAMEBUFFER_RENDERBUFFER, CALL_FRAMEBUFFER_TEXTURE_2D, CALL_FRONT_FACE,
    CALL_GENERATE_MIPMAP, CALL_HINT, CALL_LINE_WIDTH, CALL_LINK_PROGRAM, CALL_POLYGON_OFFSET, CALL_RENDERBUFFER_STORAGE,
    CALL_SCISSOR, CALL_STENCIL_FUNC, CALL_STENCIL_MASK, CALL_STENCIL_OP, CALL_TEX_PARAMETER_F, CALL_TEX_PARAMETER_I,
    CALL_UNIFORM_1F, CALL_UNIFORM_2F, CALL_UNIFORM_3F, CALL_UNIFORM_4F, CALL_UNIFORM_1I, CALL_UNIFORM_2I,
    CALL_UNIFORM_3I, CALL_UNIFORM_4I, CALL_USE_PROGRAM, CALL_VALIDATE_PROGRAM, CALL_VERTEX_ATTRIB_1F,
    CALL_VERTEX_ATTRIB_2F, CALL_VERTEX_ATTRIB_3F, CALL_VERTEX_ATTRIB_4F, CALL_VERTEX_ATTRIB_POINTER, CALL_VIEWPORT,
};

WEBGL_FUNCTION(js_call) {
    ACTIVATE();
    float f[8] = { 0 };
    int i[8] = { 0 };
    for (int k = 0; k < 8 && k + 2 < argc; k++) {
        f[k] = FLOAT(k + 2);
        i[k] = INT(k + 2);
    }
    struct state *state = &context->state;
    enum simple_call call = (enum simple_call)INT(1);
    // Keep the state that activate() must set again.
    switch (call) {
    case CALL_ACTIVE_TEXTURE: state->active_texture = (GLenum)i[0]; break;
    case CALL_BIND_BUFFER: if (i[0] == GL_ARRAY_BUFFER) state->array_buffer = (GLuint)i[1]; break;
    case CALL_BIND_TEXTURE: {
        int unit = (int)(state->active_texture - GL_TEXTURE0);
        if (i[0] == GL_TEXTURE_2D && unit >= 0 && unit < MAX_TEXTURE_UNITS) state->textures[unit] = (GLuint)i[1];
        break;
    }
    case CALL_BIND_RENDERBUFFER: state->renderbuffer = (GLuint)i[1]; break;
    case CALL_BLEND_COLOR: memcpy(state->blend_color, f, sizeof state->blend_color); break;
    case CALL_BLEND_EQUATION: state->blend_equation_rgb = state->blend_equation_alpha = (GLenum)i[0]; break;
    case CALL_BLEND_EQUATION_SEPARATE:
        state->blend_equation_rgb = (GLenum)i[0];
        state->blend_equation_alpha = (GLenum)i[1];
        break;
    case CALL_BLEND_FUNC:
        state->blend_source_rgb = state->blend_source_alpha = (GLenum)i[0];
        state->blend_destination_rgb = state->blend_destination_alpha = (GLenum)i[1];
        break;
    case CALL_BLEND_FUNC_SEPARATE:
        state->blend_source_rgb = (GLenum)i[0];
        state->blend_destination_rgb = (GLenum)i[1];
        state->blend_source_alpha = (GLenum)i[2];
        state->blend_destination_alpha = (GLenum)i[3];
        break;
    case CALL_CLEAR_COLOR: memcpy(state->clear_color, f, sizeof state->clear_color); break;
    case CALL_CLEAR_DEPTH: state->clear_depth = f[0]; break;
    case CALL_CLEAR_STENCIL: state->clear_stencil = i[0]; break;
    case CALL_COLOR_MASK: for (int k = 0; k < 4; k++) state->color_mask[k] = i[k] != 0; break;
    case CALL_CULL_FACE: state->cull_face = (GLenum)i[0]; break;
    case CALL_DEPTH_FUNC: state->depth_function = (GLenum)i[0]; break;
    case CALL_DEPTH_MASK: state->depth_mask = i[0] != 0; break;
    case CALL_DEPTH_RANGE: state->depth_range[0] = f[0]; state->depth_range[1] = f[1]; break;
    case CALL_DISABLE: set_capability(state, (GLenum)i[0], false); break;
    case CALL_ENABLE: set_capability(state, (GLenum)i[0], true); break;
    case CALL_FRONT_FACE: state->front_face = (GLenum)i[0]; break;
    case CALL_LINE_WIDTH: state->line_width = f[0]; break;
    case CALL_POLYGON_OFFSET: state->polygon_offset[0] = f[0]; state->polygon_offset[1] = f[1]; break;
    case CALL_SCISSOR: memcpy(state->scissor, i, sizeof state->scissor); break;
    case CALL_STENCIL_FUNC:
        state->stencil_function = (GLenum)i[0];
        state->stencil_reference = i[1];
        state->stencil_value_mask = (GLuint)i[2];
        break;
    case CALL_STENCIL_MASK: state->stencil_write_mask = (GLuint)i[0]; break;
    case CALL_STENCIL_OP:
        state->stencil_fail = (GLenum)i[0];
        state->stencil_depth_fail = (GLenum)i[1];
        state->stencil_pass = (GLenum)i[2];
        break;
    case CALL_USE_PROGRAM: state->program = (GLuint)i[0]; break;
    case CALL_VIEWPORT: memcpy(state->viewport, i, sizeof state->viewport); break;
    default: break;
    }
    switch (call) {
    case CALL_ACTIVE_TEXTURE: glActiveTexture((GLenum)i[0]); break;
    case CALL_ATTACH_SHADER: glAttachShader((GLuint)i[0], (GLuint)i[1]); break;
    case CALL_BIND_BUFFER: glBindBuffer((GLenum)i[0], (GLuint)i[1]); break;
    case CALL_BIND_TEXTURE: glBindTexture((GLenum)i[0], (GLuint)i[1]); break;
    case CALL_BIND_RENDERBUFFER: glBindRenderbuffer((GLenum)i[0], (GLuint)i[1]); break;
    case CALL_BLEND_COLOR: glBlendColor(f[0], f[1], f[2], f[3]); break;
    case CALL_BLEND_EQUATION: glBlendEquation((GLenum)i[0]); break;
    case CALL_BLEND_EQUATION_SEPARATE: glBlendEquationSeparate((GLenum)i[0], (GLenum)i[1]); break;
    case CALL_BLEND_FUNC: glBlendFunc((GLenum)i[0], (GLenum)i[1]); break;
    case CALL_BLEND_FUNC_SEPARATE: glBlendFuncSeparate((GLenum)i[0], (GLenum)i[1], (GLenum)i[2], (GLenum)i[3]); break;
    case CALL_CLEAR: glClear((GLbitfield)i[0]); break;
    case CALL_CLEAR_COLOR: glClearColor(f[0], f[1], f[2], f[3]); break;
    case CALL_CLEAR_DEPTH: glClearDepthf(f[0]); break;
    case CALL_CLEAR_STENCIL: glClearStencil(i[0]); break;
    case CALL_COLOR_MASK: glColorMask((GLboolean)i[0], (GLboolean)i[1], (GLboolean)i[2], (GLboolean)i[3]); break;
    case CALL_COMPILE_SHADER: glCompileShader((GLuint)i[0]); break;
    case CALL_CULL_FACE: glCullFace((GLenum)i[0]); break;
    case CALL_DEPTH_FUNC: glDepthFunc((GLenum)i[0]); break;
    case CALL_DEPTH_MASK: glDepthMask((GLboolean)i[0]); break;
    case CALL_DEPTH_RANGE: glDepthRangef(f[0], f[1]); break;
    case CALL_DETACH_SHADER: glDetachShader((GLuint)i[0], (GLuint)i[1]); break;
    case CALL_DISABLE: glDisable((GLenum)i[0]); break;
    case CALL_DISABLE_VERTEX_ATTRIB_ARRAY: glDisableVertexAttribArray((GLuint)i[0]); break;
    case CALL_DRAW_ARRAYS: glDrawArrays((GLenum)i[0], i[1], i[2]); break;
    case CALL_DRAW_ELEMENTS: glDrawElements((GLenum)i[0], i[1], (GLenum)i[2], (const void *)(intptr_t)i[3]); break;
    case CALL_ENABLE: glEnable((GLenum)i[0]); break;
    case CALL_ENABLE_VERTEX_ATTRIB_ARRAY: glEnableVertexAttribArray((GLuint)i[0]); break;
    case CALL_FINISH: glFinish(); break;
    case CALL_FLUSH: glFlush(); break;
    case CALL_FRAMEBUFFER_RENDERBUFFER:
        glFramebufferRenderbuffer((GLenum)i[0], (GLenum)i[1], (GLenum)i[2], (GLuint)i[3]);
        break;
    case CALL_FRAMEBUFFER_TEXTURE_2D:
        glFramebufferTexture2D((GLenum)i[0], (GLenum)i[1], (GLenum)i[2], (GLuint)i[3], i[4]);
        break;
    case CALL_FRONT_FACE: glFrontFace((GLenum)i[0]); break;
    case CALL_GENERATE_MIPMAP: glGenerateMipmap((GLenum)i[0]); break;
    case CALL_HINT: glHint((GLenum)i[0], (GLenum)i[1]); break;
    case CALL_LINE_WIDTH: glLineWidth(f[0]); break;
    case CALL_LINK_PROGRAM: glLinkProgram((GLuint)i[0]); break;
    case CALL_POLYGON_OFFSET: glPolygonOffset(f[0], f[1]); break;
    case CALL_RENDERBUFFER_STORAGE: {
        // DEPTH_STENCIL of WebGL is no GLES format.
        GLenum format = i[1] == 0x84F9 ? GL_DEPTH24_STENCIL8 : (GLenum)i[1];
        glRenderbufferStorage((GLenum)i[0], format, i[2], i[3]);
        break;
    }
    case CALL_SCISSOR: glScissor(i[0], i[1], i[2], i[3]); break;
    case CALL_STENCIL_FUNC: glStencilFunc((GLenum)i[0], i[1], (GLuint)i[2]); break;
    case CALL_STENCIL_MASK: glStencilMask((GLuint)i[0]); break;
    case CALL_STENCIL_OP: glStencilOp((GLenum)i[0], (GLenum)i[1], (GLenum)i[2]); break;
    case CALL_TEX_PARAMETER_F: glTexParameterf((GLenum)i[0], (GLenum)i[1], f[2]); break;
    case CALL_TEX_PARAMETER_I: glTexParameteri((GLenum)i[0], (GLenum)i[1], i[2]); break;
    case CALL_UNIFORM_1F: glUniform1f(i[0], f[1]); break;
    case CALL_UNIFORM_2F: glUniform2f(i[0], f[1], f[2]); break;
    case CALL_UNIFORM_3F: glUniform3f(i[0], f[1], f[2], f[3]); break;
    case CALL_UNIFORM_4F: glUniform4f(i[0], f[1], f[2], f[3], f[4]); break;
    case CALL_UNIFORM_1I: glUniform1i(i[0], i[1]); break;
    case CALL_UNIFORM_2I: glUniform2i(i[0], i[1], i[2]); break;
    case CALL_UNIFORM_3I: glUniform3i(i[0], i[1], i[2], i[3]); break;
    case CALL_UNIFORM_4I: glUniform4i(i[0], i[1], i[2], i[3], i[4]); break;
    case CALL_USE_PROGRAM: glUseProgram((GLuint)i[0]); break;
    case CALL_VALIDATE_PROGRAM: glValidateProgram((GLuint)i[0]); break;
    case CALL_VERTEX_ATTRIB_1F: glVertexAttrib1f((GLuint)i[0], f[1]); break;
    case CALL_VERTEX_ATTRIB_2F: glVertexAttrib2f((GLuint)i[0], f[1], f[2]); break;
    case CALL_VERTEX_ATTRIB_3F: glVertexAttrib3f((GLuint)i[0], f[1], f[2], f[3]); break;
    case CALL_VERTEX_ATTRIB_4F: glVertexAttrib4f((GLuint)i[0], f[1], f[2], f[3], f[4]); break;
    case CALL_VERTEX_ATTRIB_POINTER:
        glVertexAttribPointer((GLuint)i[0], i[1], (GLenum)i[2], (GLboolean)i[3], i[4], (const void *)(intptr_t)i[5]);
        break;
    case CALL_VIEWPORT: glViewport(i[0], i[1], i[2], i[3]); break;
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- data, shaders and queries

static void *typed_array_data(JSContext *ctx, JSValueConst value, size_t *size) {
    size_t offset, length, element;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &element);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        uint8_t *data = JS_GetArrayBuffer(ctx, size, value); // a plain ArrayBuffer
        return data;
    }
    size_t buffer_size;
    uint8_t *data = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
    JS_FreeValue(ctx, buffer);
    *size = length;
    return data ? data + offset : NULL;
}

// bufferData(id, target, dataOrSize, usage)
WEBGL_FUNCTION(js_buffer_data) {
    ACTIVATE();
    if (JS_IsNumber(argv[2])) {
        glBufferData((GLenum)INT(1), INT(2), NULL, (GLenum)INT(3));
        return JS_UNDEFINED;
    }
    size_t size = 0;
    void *data = typed_array_data(ctx, argv[2], &size);
    glBufferData((GLenum)INT(1), (GLsizeiptr)size, data, (GLenum)INT(3));
    return JS_UNDEFINED;
}

WEBGL_FUNCTION(js_buffer_sub_data) {
    ACTIVATE();
    size_t size = 0;
    void *data = typed_array_data(ctx, argv[3], &size);
    glBufferSubData((GLenum)INT(1), INT(2), (GLsizeiptr)size, data);
    return JS_UNDEFINED;
}

WEBGL_FUNCTION(js_shader_source) {
    ACTIVATE();
    const char *source = JS_ToCString(ctx, argv[2]);
    glShaderSource((GLuint)INT(1), 1, &source, NULL);
    JS_FreeCString(ctx, source);
    return JS_UNDEFINED;
}

WEBGL_FUNCTION(js_shader_parameter) {
    ACTIVATE();
    GLint value = 0;
    glGetShaderiv((GLuint)INT(1), (GLenum)INT(2), &value);
    return JS_NewInt32(ctx, value);
}

WEBGL_FUNCTION(js_program_parameter) {
    ACTIVATE();
    GLint value = 0;
    glGetProgramiv((GLuint)INT(1), (GLenum)INT(2), &value);
    return JS_NewInt32(ctx, value);
}

WEBGL_FUNCTION(js_shader_info_log) {
    ACTIVATE();
    char log[MAX_LOG] = "";
    glGetShaderInfoLog((GLuint)INT(1), sizeof log, NULL, log);
    return JS_NewString(ctx, log);
}

WEBGL_FUNCTION(js_program_info_log) {
    ACTIVATE();
    char log[MAX_LOG] = "";
    glGetProgramInfoLog((GLuint)INT(1), sizeof log, NULL, log);
    return JS_NewString(ctx, log);
}

WEBGL_FUNCTION(js_bind_attrib_location) {
    ACTIVATE();
    const char *name = JS_ToCString(ctx, argv[3]);
    glBindAttribLocation((GLuint)INT(1), (GLuint)INT(2), name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

WEBGL_FUNCTION(js_attrib_location) {
    ACTIVATE();
    const char *name = JS_ToCString(ctx, argv[2]);
    GLint location = glGetAttribLocation((GLuint)INT(1), name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, location);
}

WEBGL_FUNCTION(js_uniform_location) {
    ACTIVATE();
    const char *name = JS_ToCString(ctx, argv[2]);
    GLint location = glGetUniformLocation((GLuint)INT(1), name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, location);
}

// activeInfo(id, program, index, uniform) returns [name, size, type] of an active uniform or attribute.
WEBGL_FUNCTION(js_active_info) {
    ACTIVATE();
    char name[256] = "";
    GLint size = 0;
    GLenum type = 0;
    if (JS_ToBool(ctx, argv[3])) glGetActiveUniform((GLuint)INT(1), (GLuint)INT(2), sizeof name, NULL, &size, &type, name);
    else glGetActiveAttrib((GLuint)INT(1), (GLuint)INT(2), sizeof name, NULL, &size, &type, name);
    JSValue array = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, array, 0, JS_NewString(ctx, name));
    JS_SetPropertyUint32(ctx, array, 1, JS_NewInt32(ctx, size));
    JS_SetPropertyUint32(ctx, array, 2, JS_NewInt32(ctx, (int)type));
    return array;
}

// uniformv(id, kind, location, data) with kind 0..3 for float vectors of 1..4, 4..7 for int vectors,
// 8 for mat2, 9 for mat3, 10 for mat4.
WEBGL_FUNCTION(js_uniformv) {
    ACTIVATE();
    int kind = INT(1), location = INT(2);
    JSValue length_value = JS_GetPropertyStr(ctx, argv[3], "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    float *floats = malloc(sizeof(float) * (size_t)(length > 0 ? length : 1));
    int *ints = malloc(sizeof(int) * (size_t)(length > 0 ? length : 1));
    for (int32_t k = 0; k < length; k++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[3], (uint32_t)k);
        floats[k] = to_float(ctx, item);
        ints[k] = to_int(ctx, item);
        JS_FreeValue(ctx, item);
    }
    switch (kind) {
    case 0: glUniform1fv(location, length, floats); break;
    case 1: glUniform2fv(location, length / 2, floats); break;
    case 2: glUniform3fv(location, length / 3, floats); break;
    case 3: glUniform4fv(location, length / 4, floats); break;
    case 4: glUniform1iv(location, length, ints); break;
    case 5: glUniform2iv(location, length / 2, ints); break;
    case 6: glUniform3iv(location, length / 3, ints); break;
    case 7: glUniform4iv(location, length / 4, ints); break;
    case 8: glUniformMatrix2fv(location, length / 4, GL_FALSE, floats); break;
    case 9: glUniformMatrix3fv(location, length / 9, GL_FALSE, floats); break;
    default: glUniformMatrix4fv(location, length / 16, GL_FALSE, floats); break;
    }
    free(floats);
    free(ints);
    return JS_UNDEFINED;
}

// The WebGL pixel store flags for flipping and premultiplying uploads stay in the context.
WEBGL_FUNCTION(js_pixel_store) {
    ACTIVATE();
    GLenum name = (GLenum)INT(1);
    int value = INT(2);
    if (name == WEBGL_UNPACK_FLIP_Y) context->flip_y = value != 0;
    else if (name == WEBGL_UNPACK_PREMULTIPLY_ALPHA) context->premultiply_alpha = value != 0;
    else if (name != WEBGL_UNPACK_COLORSPACE_CONVERSION) {
        glPixelStorei(name, value);
        if (name == GL_UNPACK_ALIGNMENT) context->unpack_alignment = value;
    }
    return JS_UNDEFINED;
}

static void flip_rows(unsigned char *pixels, int width, int height, int bytes_per_pixel) {
    size_t row = (size_t)width * (size_t)bytes_per_pixel;
    unsigned char *swap = malloc(row);
    for (int y = 0; y < height / 2; y++) {
        memcpy(swap, pixels + (size_t)y * row, row);
        memcpy(pixels + (size_t)y * row, pixels + (size_t)(height - 1 - y) * row, row);
        memcpy(pixels + (size_t)(height - 1 - y) * row, swap, row);
    }
    free(swap);
}

// texImage2D(id, target, level, internalFormat, width, height, format, type, data or null): the typed-array form.
WEBGL_FUNCTION(js_tex_image_2d) {
    ACTIVATE();
    int width = INT(4), height = INT(5);
    size_t size = 0;
    unsigned char *data = JS_IsNull(argv[8]) || JS_IsUndefined(argv[8]) ? NULL : typed_array_data(ctx, argv[8], &size);
    unsigned char *copy = NULL;
    if (data && context->flip_y && INT(7) == GL_UNSIGNED_BYTE && INT(6) == GL_RGBA && size >= (size_t)width * (size_t)height * 4) {
        copy = malloc((size_t)width * (size_t)height * 4);
        memcpy(copy, data, (size_t)width * (size_t)height * 4);
        flip_rows(copy, width, height, 4);
        data = copy;
    }
    glTexImage2D((GLenum)INT(1), INT(2), INT(3), width, height, 0, (GLenum)INT(6), (GLenum)INT(7), data);
    free(copy);
    return JS_UNDEFINED;
}

// texImageSource(id, target, level, internalFormat, format, type, canvasHandle): the image-source form, for 2D
// canvases and images of js/canvas.js; the pixel store flags apply.
WEBGL_FUNCTION(js_tex_image_source) {
    ACTIVATE();
    int width, height;
    unsigned char *pixels = canvas2d_rgba(INT(6), context->premultiply_alpha, &width, &height);
    if (!pixels) return JS_ThrowTypeError(ctx, "texImage2D: the source has no pixels");
    if (context->flip_y) flip_rows(pixels, width, height, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D((GLenum)INT(1), INT(2), INT(3), width, height, 0, (GLenum)INT(4), (GLenum)INT(5), pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, context->unpack_alignment);
    free(pixels);
    return JS_UNDEFINED;
}

// readPixels(id, x, y, width, height, format, type, Uint8Array)
WEBGL_FUNCTION(js_read_pixels) {
    ACTIVATE();
    size_t size = 0;
    void *data = typed_array_data(ctx, argv[7], &size);
    if (!data || size < (size_t)INT(3) * (size_t)INT(4) * 4) return JS_ThrowRangeError(ctx, "readPixels: the array is too small");
    glReadPixels(INT(1), INT(2), INT(3), INT(4), (GLenum)INT(5), (GLenum)INT(6), data);
    return JS_UNDEFINED;
}

// parameter(id, name) returns a number, or an array for the parameters with several values.
WEBGL_FUNCTION(js_parameter) {
    ACTIVATE();
    GLenum name = (GLenum)INT(1);
    GLint values[4] = { 0 };
    int count = name == GL_VIEWPORT || name == GL_SCISSOR_BOX ? 4 : name == GL_MAX_VIEWPORT_DIMS ? 2 : 1;
    if (name == GL_COLOR_CLEAR_VALUE || name == GL_DEPTH_RANGE || name == GL_ALIASED_LINE_WIDTH_RANGE ||
        name == GL_ALIASED_POINT_SIZE_RANGE || name == GL_DEPTH_CLEAR_VALUE || name == GL_LINE_WIDTH) {
        GLfloat floats[4] = { 0 };
        glGetFloatv(name, floats);
        count = name == GL_COLOR_CLEAR_VALUE ? 4 : name == GL_DEPTH_CLEAR_VALUE || name == GL_LINE_WIDTH ? 1 : 2;
        if (count == 1) return JS_NewFloat64(ctx, floats[0]);
        JSValue array = JS_NewArray(ctx);
        for (int i = 0; i < count; i++) JS_SetPropertyUint32(ctx, array, (uint32_t)i, JS_NewFloat64(ctx, floats[i]));
        return array;
    }
    if (name == GL_VERSION || name == GL_SHADING_LANGUAGE_VERSION || name == GL_VENDOR || name == GL_RENDERER) {
        const char *text = name == GL_VERSION ? "WebGL 1.0 (hydra-native on OpenGL ES 3.0)"
                         : name == GL_SHADING_LANGUAGE_VERSION ? "WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0)"
                                                               : (const char *)glGetString(name);
        return JS_NewString(ctx, text ? text : "");
    }
    glGetIntegerv(name, values);
    if (count == 1) return JS_NewInt32(ctx, values[0]);
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) JS_SetPropertyUint32(ctx, array, (uint32_t)i, JS_NewInt32(ctx, values[i]));
    return array;
}

WEBGL_FUNCTION(js_is_enabled) {
    ACTIVATE();
    return JS_NewBool(ctx, glIsEnabled((GLenum)INT(1)));
}

WEBGL_FUNCTION(js_error) {
    ACTIVATE();
    return JS_NewInt32(ctx, (int)glGetError());
}

static const JSCFunctionListEntry webgl_functions[] = {
    JS_CFUNC_DEF("glContextCreate", 2, js_context_create),
    JS_CFUNC_DEF("glContextResize", 3, js_context_resize),
    JS_CFUNC_DEF("glContextDestroy", 1, js_context_destroy),
    JS_CFUNC_DEF("glContextCopyTo", 2, js_context_copy_to),
    JS_CFUNC_DEF("glContextReadInto", 3, js_context_read_into),
    JS_CFUNC_DEF("glCreateBuffer", 1, js_create_buffer),
    JS_CFUNC_DEF("glCreateTexture", 1, js_create_texture),
    JS_CFUNC_DEF("glCreateFramebuffer", 1, js_create_framebuffer),
    JS_CFUNC_DEF("glCreateRenderbuffer", 1, js_create_renderbuffer),
    JS_CFUNC_DEF("glCreateProgram", 1, js_create_program),
    JS_CFUNC_DEF("glCreateShader", 2, js_create_shader),
    JS_CFUNC_DEF("glDeleteBuffer", 2, js_delete_buffer),
    JS_CFUNC_DEF("glDeleteTexture", 2, js_delete_texture),
    JS_CFUNC_DEF("glDeleteFramebuffer", 2, js_delete_framebuffer),
    JS_CFUNC_DEF("glDeleteRenderbuffer", 2, js_delete_renderbuffer),
    JS_CFUNC_DEF("glDeleteProgram", 2, js_delete_program),
    JS_CFUNC_DEF("glDeleteShader", 2, js_delete_shader),
    JS_CFUNC_DEF("glBindFramebuffer", 3, js_bind_framebuffer),
    JS_CFUNC_DEF("glCall", 10, js_call),
    JS_CFUNC_DEF("glBufferData", 4, js_buffer_data),
    JS_CFUNC_DEF("glBufferSubData", 4, js_buffer_sub_data),
    JS_CFUNC_DEF("glShaderSource", 3, js_shader_source),
    JS_CFUNC_DEF("glShaderParameter", 3, js_shader_parameter),
    JS_CFUNC_DEF("glProgramParameter", 3, js_program_parameter),
    JS_CFUNC_DEF("glShaderInfoLog", 2, js_shader_info_log),
    JS_CFUNC_DEF("glProgramInfoLog", 2, js_program_info_log),
    JS_CFUNC_DEF("glBindAttribLocation", 4, js_bind_attrib_location),
    JS_CFUNC_DEF("glAttribLocation", 3, js_attrib_location),
    JS_CFUNC_DEF("glUniformLocation", 3, js_uniform_location),
    JS_CFUNC_DEF("glActiveInfo", 4, js_active_info),
    JS_CFUNC_DEF("glUniformv", 4, js_uniformv),
    JS_CFUNC_DEF("glPixelStore", 3, js_pixel_store),
    JS_CFUNC_DEF("glTexImage2D", 9, js_tex_image_2d),
    JS_CFUNC_DEF("glTexImageSource", 7, js_tex_image_source),
    JS_CFUNC_DEF("glReadPixels", 8, js_read_pixels),
    JS_CFUNC_DEF("glParameter", 2, js_parameter),
    JS_CFUNC_DEF("glIsEnabled", 2, js_is_enabled),
    JS_CFUNC_DEF("glError", 1, js_error),
};

void webgl_install(JSContext *ctx, JSValue native) {
    JS_SetPropertyFunctionList(ctx, native, webgl_functions, sizeof webgl_functions / sizeof webgl_functions[0]);
}
