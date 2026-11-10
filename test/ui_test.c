// Checks the key table and the overlay commands without a window: key events in, evaluated code and file content out.
// The overlay drawing is replaced by stubs. Run with `make test`.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/overlay.h"
#include "../src/ui.h"

static int failures;
static char evaluated[4096];
static char next_error[256];

#define EXPECT_STRING(actual, expected)                                                                    \
    do {                                                                                                   \
        if (strcmp(actual, expected) != 0) {                                                               \
            fprintf(stderr, "%s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, actual, expected); \
            failures++;                                                                                    \
        }                                                                                                  \
    } while (0)

// ---------------------------------------------------------------- stubs

bool overlay_init(const char *font_path, float font_size) { return true; }
bool overlay_set_font_size(float font_size) { return true; }
float overlay_font_size(void) { return 20; }
float overlay_char_width(void) { return 10; }
float overlay_line_height(void) { return 20; }
void overlay_begin(void) {}
void overlay_rect(float x, float y, float width, float height, uint32_t color) {}
void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {}
void overlay_end(int width, int height) {}

static char *evaluate(const char *code, size_t length, const char *name) {
    snprintf(evaluated, sizeof evaluated, "%.*s", (int)length, code);
    if (!next_error[0]) return NULL;
    char *error = strdup(next_error);
    next_error[0] = '\0';
    return error;
}

// Stands in for js/main.js and js/editor-actions.js: __format makes upper case, the others return fixed code.
static char *call(const char *function, const char *argument) {
    if (!strcmp(function, "__randomExample")) return strdup("noise().out()");
    if (!strcmp(function, "__mutate")) return strdup("noise(9).out()");
    if (!strcmp(function, "__mutateUndo")) return strdup("noise().out()");
    if (!strcmp(function, "__completionNames")) return strdup("noise\nosc\nout");
    if (!strcmp(function, "__urlForCode")) return strdup("https://hydra.ojack.xyz/?code=x");
    char *text = strdup(argument);
    for (char *c = text; *c; c++) *c = (char)toupper((unsigned char)*c);
    return text;
}

static void nothing(void) {}
static bool never(void) { return false; }

// ---------------------------------------------------------------- input helpers

static void key(SDL_Keycode code, Uint16 modifiers) {
    SDL_KeyboardEvent event = { .type = SDL_KEYDOWN, .state = SDL_PRESSED };
    event.keysym.sym = code;
    event.keysym.mod = modifiers;
    ui_key(&event);
}

static void type(const char *text) {
    for (const char *c = text; *c; c++) {
        if (*c == '\n') {
            key(SDLK_RETURN, 0);
        } else {
            char one[2] = { *c, '\0' };
            key((SDL_Keycode)*c, 0);
            ui_text_input(one);
        }
    }
}

static char *read_file(const char *path) {
    static char text[4096];
    FILE *file = fopen(path, "rb");
    size_t length = file ? fread(text, 1, sizeof text - 1, file) : 0;
    if (file) fclose(file);
    text[length] = '\0';
    return text;
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    fputs(text, file);
    fclose(file);
}

int main(void) {
    char path[] = "/tmp/hydra-native-ui-test-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    write_file(path, "osc(10)\n  .out()\n\nnoise().out()");
    static const struct ui_host host = { evaluate, nothing, never, nothing, call, nothing };
    ui_init("unused.ttf", 20, path, &host);
    ui_draw(800, 600, 0);

    key(SDLK_RETURN, KMOD_LALT); // the block around the cursor (line 0)
    EXPECT_STRING(evaluated, "osc(10)\n  .out()");
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc(10)");
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LSHIFT);
    EXPECT_STRING(evaluated, "osc(10)\n  .out()\n\nnoise().out()");

    key(SDLK_END, KMOD_LCTRL); // to the end, then type a new line
    type("\nshape(4).out(o1)");
    key(SDLK_RETURN, KMOD_LSHIFT);
    EXPECT_STRING(evaluated, "shape(4).out(o1)");

    key(SDLK_SLASH, KMOD_LCTRL); // comment the last line, then save
    key(SDLK_s, KMOD_LCTRL);
    EXPECT_STRING(read_file(path), "osc(10)\n  .out()\n\nnoise().out()\n// shape(4).out(o1)");

    key(SDLK_z, KMOD_LCTRL); // undo the comment, then hide the code: typing and editing keys stop working
    key(SDLK_h, KMOD_LCTRL | KMOD_LSHIFT);
    type("xyz");
    key(SDLK_BACKSPACE, 0);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT); // evaluation still works
    EXPECT_STRING(evaluated, "osc(10)\n  .out()\n\nnoise().out()\nshape(4).out(o1)");
    key(SDLK_h, KMOD_LCTRL | KMOD_LSHIFT);

    // --watch: unsaved edits stay, a saved buffer takes the new file content and evaluates it.
    write_file(path, "solid().out()");
    ui_file_changed();
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "osc(10)\n  .out()\n\nnoise().out()\nshape(4).out(o1)");
    key(SDLK_s, KMOD_LCTRL);
    write_file(path, "solid().out()");
    evaluated[0] = '\0';
    ui_file_changed();
    EXPECT_STRING(evaluated, "solid().out()");

    // An error leaves the buffer as it is.
    snprintf(next_error, sizeof next_error, "ReferenceError: 'x' is not defined\nstack");
    key(SDLK_RETURN, KMOD_LCTRL);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "solid().out()");

    // Shift+Ctrl+F and Shift+Alt+F format through the JS host (js-beautify); here a stub makes upper case.
    key(SDLK_f, KMOD_LCTRL | KMOD_LSHIFT);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "SOLID().OUT()");

    // The toolbar actions: clear all evaluates hush, an example replaces the code and runs it, the mutator replaces it.
    key(SDLK_DELETE, KMOD_LCTRL | KMOD_LSHIFT);
    EXPECT_STRING(evaluated, "hush(); speed = 1");
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "\n \n // Type some code on a new line (such as \"osc().out()\"), and press CTRL+shift+enter");
    key(SDLK_F7, 0);
    EXPECT_STRING(evaluated, "noise().out()");
    key(SDLK_F8, 0);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "noise(9).out()");
    key(SDLK_F8, KMOD_LSHIFT);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "noise().out()");

    // Find, replace, go to line, command palette and autocomplete, all through the key table.
    key(SDLK_a, KMOD_LCTRL);
    type("osc(1).out()\nosc(2).out()\nnoise().out()");
    key(SDLK_h, KMOD_LCTRL);
    type("osc");
    key(SDLK_TAB, 0);
    type("shape");
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT); // in the prompt: replace all
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()");
    key(SDLK_g, KMOD_LCTRL);
    type("2");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "shape(2).out()");
    key(SDLK_f, KMOD_LCTRL);
    type("noise");
    key(SDLK_RETURN, 0);
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, KMOD_LCTRL); // evaluates the selected match
    EXPECT_STRING(evaluated, "noise");
    key(SDLK_p, KMOD_LCTRL | KMOD_LSHIFT);
    type("evaluate all");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()");

    key(SDLK_END, KMOD_LCTRL);
    type("\nno");
    key(SDLK_RETURN, 0); // the completion list is open: Enter inserts "noise"
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");

    // Two cursors type into two lines.
    key(SDLK_HOME, KMOD_LCTRL);
    key(SDLK_DOWN, KMOD_LCTRL | KMOD_LSHIFT);
    type("x");
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "xshape(1).out()\nxshape(2).out()\nnoise().out()\nnoise");

    // A two-key command: Ctrl+K, then Ctrl+U makes the word upper case.
    key(SDLK_a, KMOD_LCTRL);
    type("noise");
    key(SDLK_k, KMOD_LCTRL);
    key(SDLK_LCTRL, KMOD_LCTRL); // a modifier alone keeps the pending command
    key(SDLK_u, KMOD_LCTRL);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "NOISE");

    unlink(path);
    if (failures) {
        fprintf(stderr, "ui_test: %d failures\n", failures);
        return 1;
    }
    printf("ui_test: ok\n");
    return 0;
}
