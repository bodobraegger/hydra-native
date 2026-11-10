// Checks the key table and the overlay commands without a window: key events in, evaluated code and file content out.
// The overlay drawing is replaced by stubs. Run with `make test`.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/overlay.h"
#include "../src/settings.h"
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

#define EXPECT(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_CONTAINS(actual, part)                                                                     \
    do {                                                                                                  \
        if (!strstr(actual, part)) {                                                                      \
            fprintf(stderr, "%s:%d: got \"%s\", expected \"%s\" in it\n", __FILE__, __LINE__, actual, part); \
            failures++;                                                                                   \
        }                                                                                                 \
    } while (0)

// ---------------------------------------------------------------- stubs

bool overlay_init(const char *font_path, const char *emoji_font_path, float font_size) { return true; }
bool overlay_set_font_size(float font_size) { return true; }
bool overlay_set_font(const char *font_path) { return true; }
float overlay_font_size(void) { return 20; }
float overlay_char_width(void) { return 10; }
float overlay_line_height(void) { return 20; }
void overlay_begin(void) {}
void overlay_rect(float x, float y, float width, float height, uint32_t color) {}
static char bar_text[512]; // the last drawn text that starts with "find: "
void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {
    if (length > 6 && !strncmp(text, "find: ", 6)) snprintf(bar_text, sizeof bar_text, "%.*s", (int)length, text);
}
void overlay_end(int width, int height) {}
void overlay_difference_begin(void) {}
void overlay_difference_end(float x, float y, float width, float height) {}
int overlay_columns(int codepoint) { return 1; }
void overlay_image(float x, float y, float width, float height, unsigned texture) {}
unsigned thumbnail_texture(const char *url) { return 0; }
float overlay_text_shaped(float x, float y, const char *text, size_t length, bool right_to_left, uint32_t color) { return 0; }
void overlay_text_fitted(float x, float y, const char *text, size_t length, float width, uint32_t color) {}
float overlay_text_shaped_width(const char *text, size_t length, bool right_to_left) { return (float)length * 10; }
size_t overlay_text_columns(const char *text, size_t length) { return length; }

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

static char shared_name[64];
static bool share(const char *code, const char *name) {
    snprintf(shared_name, sizeof shared_name, "%s", name);
    return true;
}

static char title[4096];
static void set_title(const char *url) { snprintf(title, sizeof title, "%s", url); }
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
    // The settings file lives in a temporary XDG_CONFIG_HOME, never in the real configuration of the user.
    char config[] = "/tmp/hydra-native-ui-test-config-XXXXXX", settings_file[256];
    EXPECT(mkdtemp(config) != NULL);
    setenv("XDG_CONFIG_HOME", config, 1);
    EXPECT(settings_path(settings_file, sizeof settings_file));
    EXPECT_STRING(settings_file + strlen(config), "/hydra-native/settings.json");
    char error[SETTINGS_TEXT];
    // An empty settings object writes the directory and the file.
    struct settings stored;
    settings_defaults(&stored);
    EXPECT(settings_save(&stored, settings_file, error, sizeof error));
    write_file(settings_file, "{\n  \"fontSize\": 24,\n  \"theme\": \"mine\",\n  \"galleryServer\": \"http://127.0.0.1:9\",\n"
                              "  \"themes\": { \"mine\": { \"base\": \"solarized\", \"text\": \"#ff0000\" } },\n"
                              "  \"keybindings\": { \"join_lines\": \"Ctrl+Alt+J\" },\n"
                              "  \"somethingElse\": { \"kept\": [1, 2] }\n}\n");
    struct settings loaded;
    settings_defaults(&loaded);
    EXPECT(settings_load(&loaded, settings_file, error, sizeof error));
    EXPECT(loaded.font_size == 24 && !strcmp(loaded.theme, "mine") && loaded.autocomplete_style == AUTOCOMPLETE_LIST &&
           loaded.autocomplete_trigger == AUTOCOMPLETE_MANUAL && loaded.render_whitespace == WHITESPACE_ALL);
    EXPECT_STRING(loaded.gallery_server, "http://127.0.0.1:9");
    EXPECT(loaded.theme_count == 1 && !strcmp(loaded.themes[0].base, "solarized") && loaded.keybinding_count == 1);
    struct settings active = loaded;
    snprintf(active.theme, sizeof active.theme, "contrast"); // like --theme contrast for one run
    static const struct ui_host host = { evaluate, nothing, never, nothing, call, nothing, share, set_title };
    ui_init(&loaded, &active, settings_file, NULL, path, &host);
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
    EXPECT_STRING(title, "https://hydra.ojack.xyz/?code=x"); // evaluate all puts the URL into the title
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

    // The find bar of the browser: no case, a count of the matches, Enter and Shift+Enter around the end,
    // Escape keeps the match selected, F3 opens it again with the last query.
    key(SDLK_HOME, KMOD_LCTRL);
    key(SDLK_f, KMOD_LCTRL);
    type("SHAPE(");
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(bar_text, "   1/2");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, 0);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(bar_text, "   1/2");
    key(SDLK_RETURN, KMOD_LSHIFT);
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "shape(");
    key(SDLK_F3, 0);
    key(SDLK_ESCAPE, 0);
    key(SDLK_RIGHT, 0);
    type("9");
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "shape(91).out()\nshape(2).out()\nnoise().out()");
    key(SDLK_z, KMOD_LCTRL);
    key(SDLK_f, KMOD_LCTRL);
    type("xyz");
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(bar_text, "   0/0");
    key(SDLK_ESCAPE, 0);
    key(SDLK_p, KMOD_LCTRL | KMOD_LSHIFT);
    type("evaluate all");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()");

    // Autocomplete opens only with Ctrl+Space (the default trigger): after typing "no", Enter makes a new line.
    key(SDLK_END, KMOD_LCTRL);
    type("\nno");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()\nno\n");
    key(SDLK_BACKSPACE, 0);
    key(SDLK_SPACE, KMOD_LCTRL); // opens the list
    type("i"); // the open list follows the typed word
    key(SDLK_RETURN, 0); // Enter inserts "noise"
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

    // Ctrl+Shift+G evaluates, asks for a name like the confirmation prompt of the web editor, then shares.
    key(SDLK_g, KMOD_LCTRL | KMOD_LSHIFT);
    type("my name");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(shared_name, "my name");

    // The "keybindings" of the file: Ctrl+Alt+J joins lines, and Ctrl+J (its default key) does nothing now.
    key(SDLK_a, KMOD_LCTRL);
    type("osc()\n.out()");
    key(SDLK_HOME, KMOD_LCTRL);
    key(SDLK_j, KMOD_LCTRL);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "osc()\n.out()");
    key(SDLK_j, KMOD_LCTRL | KMOD_LALT);
    key(SDLK_RETURN, KMOD_LCTRL | KMOD_LALT);
    EXPECT_STRING(evaluated, "osc() .out()");

    // The settings menu saves at once: the next theme after "contrast" (the theme of this run) is "solarized"; the
    // stored font size stays, the option of the run is not saved, and the other keys of the file stay.
    key(SDLK_p, KMOD_LCTRL | KMOD_LSHIFT);
    type("next color theme");
    key(SDLK_RETURN, 0);
    settings_defaults(&loaded);
    EXPECT(settings_load(&loaded, settings_file, error, sizeof error));
    EXPECT_STRING(loaded.theme, "solarized");
    EXPECT(loaded.font_size == 24 && loaded.theme_count == 1 && loaded.keybinding_count == 1);
    FILE *saved = fopen(settings_file, "r");
    char content[4096] = "";
    if (saved) {
        content[fread(content, 1, sizeof content - 1, saved)] = '\0';
        fclose(saved);
    }
    EXPECT_CONTAINS(content, "\"somethingElse\"");
    EXPECT_CONTAINS(content, "\"galleryServer\": \"http://127.0.0.1:9\"");
    char temporary[300];
    snprintf(temporary, sizeof temporary, "%s.tmp", settings_file);
    EXPECT(access(temporary, F_OK) != 0); // written through a temporary file and rename()
    key(SDLK_p, KMOD_LCTRL | KMOD_LSHIFT);
    type("autocomplete as a list or as ghost");
    key(SDLK_RETURN, 0);
    settings_defaults(&loaded);
    settings_load(&loaded, settings_file, error, sizeof error);
    EXPECT(loaded.autocomplete_style == AUTOCOMPLETE_GHOST);

    // Ghost text (the style is "ghost" now): Tab accepts the rest of the best candidate.
    key(SDLK_a, KMOD_LCTRL);
    type("n");
    key(SDLK_SPACE, KMOD_LCTRL);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");
    // Alt+] and Alt+[ cycle through the candidates "osc" and "out".
    key(SDLK_a, KMOD_LCTRL);
    type("o");
    key(SDLK_SPACE, KMOD_LCTRL);
    key(SDLK_RIGHTBRACKET, KMOD_LALT);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "out");
    key(SDLK_a, KMOD_LCTRL);
    type("o");
    key(SDLK_SPACE, KMOD_LCTRL);
    key(SDLK_RIGHTBRACKET, KMOD_LALT);
    key(SDLK_LEFTBRACKET, KMOD_LALT);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc");
    // Escape dismisses it, and Tab then indents.
    key(SDLK_a, KMOD_LCTRL);
    type("o");
    key(SDLK_SPACE, KMOD_LCTRL);
    key(SDLK_ESCAPE, 0);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_CONTAINS(evaluated, "o");
    EXPECT(strcmp(evaluated, "osc") != 0);
    // Without Ctrl+Space, typing shows nothing; with the trigger "while typing" it does.
    key(SDLK_a, KMOD_LCTRL);
    type("no");
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT(strcmp(evaluated, "noise") != 0);
    key(SDLK_p, KMOD_LCTRL | KMOD_LSHIFT);
    type("autocomplete on Ctrl+Space or while");
    key(SDLK_RETURN, 0);
    key(SDLK_a, KMOD_LCTRL);
    type("no");
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");
    // Another edit (a cursor motion) hides the suggestion.
    key(SDLK_a, KMOD_LCTRL);
    type("no");
    key(SDLK_LEFT, 0);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT(strcmp(evaluated, "noise") != 0);
    // The list style with the trigger "while typing": the list opens on its own, Enter inserts.
    key(SDLK_p, KMOD_LCTRL | KMOD_LSHIFT);
    type("autocomplete as a list or as ghost");
    key(SDLK_RETURN, 0);
    key(SDLK_a, KMOD_LCTRL);
    type("no");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");
    settings_defaults(&loaded);
    settings_load(&loaded, settings_file, error, sizeof error);
    EXPECT(loaded.autocomplete_style == AUTOCOMPLETE_LIST && loaded.autocomplete_trigger == AUTOCOMPLETE_TYPING);

    // A file that does not parse: the defaults stay, the error names the line, and nothing overwrites the file.
    write_file(settings_file, "{\n  \"theme\": \"paper\",\n  broken\n}\n");
    settings_defaults(&loaded);
    EXPECT(!settings_load(&loaded, settings_file, error, sizeof error));
    fprintf(stderr, "settings error: %s\n", error);
    EXPECT_CONTAINS(error, "line 3");
    EXPECT_STRING(loaded.theme, "hydra");
    EXPECT(!settings_save(&loaded, settings_file, error, sizeof error));
    saved = fopen(settings_file, "r");
    content[fread(content, 1, sizeof content - 1, saved)] = '\0';
    fclose(saved);
    EXPECT_CONTAINS(content, "broken");

    unlink(settings_file);
    *strrchr(settings_file, '/') = '\0';
    rmdir(settings_file);
    rmdir(config);
    unlink(path);
    if (failures) {
        fprintf(stderr, "ui_test: %d failures\n", failures);
        return 1;
    }
    printf("ui_test: ok\n");
    return 0;
}
