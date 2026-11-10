// Opens a hidden window with the OpenGL ES 3.0 context of src/window.c (Mesa on Linux, ANGLE on macOS and
// Windows), draws a triangle with a GLSL ES 3.00 shader into a framebuffer, as hydra passes do, and reads the
// pixel back. Run with `make check` (it needs a GPU or a software renderer).
#include <stdio.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "../src/window.h"

#define SIZE 16
#define EXPECTED_RED 64
#define EXPECTED_GREEN 128
#define EXPECTED_BLUE 192
#define TOLERANCE 2

static const char *vertex_source = "#version 300 es\n"
                                   "in vec2 position;\n"
                                   "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char *fragment_source = "#version 300 es\n"
                                     "precision mediump float;\n"
                                     "uniform vec4 color;\n"
                                     "out vec4 out_color;\n"
                                     "void main() { out_color = color; }\n";

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "window_test: shader: %s\n", log);
    }
    return shader;
}

int main(int argc, char **argv) {
    SDL_Window *window = window_create("hydra-native window test", SIZE * 4, SIZE * 4, SDL_WINDOW_HIDDEN, false);
    if (!window) return 1;
    printf("window_test: %s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "color"), EXPECTED_RED / 255.0f, EXPECTED_GREEN / 255.0f,
                EXPECTED_BLUE / 255.0f, 1.0f);
    // The one triangle that covers the viewport, as in src/main.c.
    static const GLfloat triangle[] = { -2, 0, 0, -2, 2, 2 };
    GLuint buffer, vertex_array;
    glGenVertexArrays(1, &vertex_array);
    glBindVertexArray(vertex_array);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glViewport(0, 0, SIZE, SIZE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4] = { 0 };
    glReadPixels(SIZE / 2, SIZE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("window_test: pixel %d %d %d %d, GL error 0x%x\n", pixel[0], pixel[1], pixel[2], pixel[3], glGetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    int expected[3] = { EXPECTED_RED, EXPECTED_GREEN, EXPECTED_BLUE };
    for (int i = 0; i < 3; i++)
        if (pixel[i] < expected[i] - TOLERANCE || pixel[i] > expected[i] + TOLERANCE) {
            printf("window_test: FAIL\n");
            return 1;
        }
    printf("window_test: ok\n");
    return 0;
}
