// Starts src/programs.c next to the window of src/window.c, as src/main.c does, and checks that the compile method
// leaves the window alone (no second window, the swap interval of the window stays), that a program links, and that
// a broken program reports its shader log. Run with `make check` (it needs a GPU or a software renderer).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/native.h"
#include "../src/window.h"

#define VSYNC_INTERVAL 1

static const char *vertex_source = "#version 300 es\n"
                                   "in vec2 position;\n"
                                   "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char *fragment_source = "#version 300 es\n"
                                     "precision mediump float;\n"
                                     "out vec4 color;\n"
                                     "void main() { color = vec4(1.0); }\n";
static const char *broken_source = "#version 300 es\n"
                                   "precision mediump float;\n"
                                   "out vec4 color;\n"
                                   "void main() { color = undefined_name; }\n";

static int failures;

static void expect(bool ok, const char *what) {
    printf("programs_test: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    SDL_Window *window = window_create("hydra-native programs test", 64, 64, SDL_WINDOW_HIDDEN, true);
    if (!window) return 1;
    programs_start();
    printf("programs_test: method %s on %s\n", programs_method(), (const char *)glGetString(GL_RENDERER));

    int window_count = 0, interval = -1;
    SDL_free(SDL_GetWindows(&window_count));
    SDL_GL_GetSwapInterval(&interval);
    expect(window_count == 1, "one window");
    expect(interval == VSYNC_INTERVAL, "the swap interval of the window stays");

    char *error = NULL;
    GLuint program = program_create(vertex_source, fragment_source);
    expect(program_status(program, true, &error) == PROGRAM_LINKED, "a program links");
    free(error);
    error = NULL;
    GLuint broken = program_create(vertex_source, broken_source);
    expect(program_status(broken, true, &error) == PROGRAM_FAILED, "a broken program fails");
    expect(error && !strncmp(error, "fragment shader: ", strlen("fragment shader: ")), "the error names the shader");
    if (error) printf("programs_test: error %s", error);
    free(error);

    programs_stop();
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("programs_test: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
