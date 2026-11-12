// Checks the key table and the overlay commands without a window: key events in, evaluated code and file content out.
// The overlay drawing is replaced by stubs. Run with `make test`.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "../src/overlay.h"
#include "../src/settings.h"
#include "../src/signature.h"
#include "../src/ui.h"

static int failures;
static char evaluated[4096];
static char next_error[256];
static char patch_bay_server[256]; // of the last set_server call of the host
static char drawn[16384]; // all texts of the last frame, one per line
static char language_now[16]; // the argument of the last __setLanguage
static float copy_x, copy_y; // where the "copy" button of the settings panel shows its text
#define STREAM_ID "stream456-a-long-id-of-the-signalling-server"

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

bool overlay_init(const char *font_path, float font_size) { return true; }
static float font_size = 20; // clamped like src/overlay.c
bool overlay_set_font_size(float size) {
    font_size = size;
    if (size < OVERLAY_MIN_FONT_SIZE) font_size = OVERLAY_MIN_FONT_SIZE;
    if (size > OVERLAY_MAX_FONT_SIZE) font_size = OVERLAY_MAX_FONT_SIZE;
    return true;
}
bool overlay_set_font(const char *font_path) { return true; }
float overlay_font_size(void) { return font_size; }
float overlay_char_width(void) { return 10; }
float overlay_line_height(void) { return 20; }
static enum overlay_layer current_layer = OVERLAY_TOP; // of overlay_set_layer()
// The texts of the last frame with their positions, for clicks on panel buttons.
#define MAX_DRAWN_TEXTS 512
static struct drawn_text {
    char text[64];
    float x, y;
    enum overlay_layer layer;
    uint32_t color;
} drawn_texts[MAX_DRAWN_TEXTS];
static size_t drawn_text_count;
void overlay_begin(void) {
    drawn[0] = '\0';
    drawn_text_count = 0;
}
void overlay_rect(float x, float y, float width, float height, uint32_t color) {}
static char bar_text[512]; // the last drawn text that starts with "find: "
void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color) {
    if (length > 6 && !strncmp(text, "find: ", 6)) snprintf(bar_text, sizeof bar_text, "%.*s", (int)length, text);
    if (drawn_text_count < MAX_DRAWN_TEXTS) {
        snprintf(drawn_texts[drawn_text_count].text, sizeof drawn_texts[0].text, "%.*s", (int)length, text);
        drawn_texts[drawn_text_count].layer = current_layer;
        drawn_texts[drawn_text_count].color = color;
        drawn_texts[drawn_text_count].x = x;
        drawn_texts[drawn_text_count++].y = y;
    }
    if (length == 4 && !strncmp(text, "copy", 4)) {
        copy_x = x;
        copy_y = y;
    }
    size_t used = strlen(drawn);
    snprintf(drawn + used, sizeof drawn - used, "%.*s\n", (int)length, text);
}
void overlay_end(int width, int height) {}
void overlay_set_layer(enum overlay_layer layer) { current_layer = layer; }
static bool difference_layers[OVERLAY_LAYER_COUNT];
void overlay_set_difference(enum overlay_layer layer, bool difference) { difference_layers[layer] = difference; }
void overlay_image(float x, float y, float width, float height, unsigned texture) {}
unsigned thumbnail_texture(const char *url) { return 0; }
float overlay_text_run(float x, float y, const char *text, size_t length, uint32_t color) { return 0; }
float overlay_text_width(const char *text, size_t length) { return (float)length * 10; }
size_t overlay_text_columns(const char *text, size_t length) { return length; }

// The definitions of hydra-synth in the format of __functionSignatures (js/main.js); "wobble" comes with setFunction.
#define SIGNATURES "osc\tsrc\tfrequency:float:60\tsync:float:0.1\toffset:float:0\n" \
                   "noise\tsrc\tscale:float:10\toffset:float:0.1\n" \
                   "src\tsrc\ttex:sampler2D:\nmodulate\tcombineCoord\ttexture:vec4:\tamount:float:0.1\n"               \
                   "rotate\tcoord\tangle:float:10\tspeed:float:0\n"                                                 \
                   "initCam\tsource\tindex?:number:\tparams?:object:\n"                                         \
                   "render\tsynth\tsource?:HydraSource:\nsetBins\taudio\tnumBins:number:\n"
static bool set_function_called;

static char *evaluate(const char *code, size_t length, const char *name) {
    snprintf(evaluated, sizeof evaluated, "%.*s", (int)length, code);
    if (strstr(evaluated, "setFunction")) set_function_called = true;
    if (!next_error[0]) return NULL;
    char *error = strdup(next_error);
    next_error[0] = '\0';
    return error;
}

static char evaluated_file[256]; // the path of the last evaluate_file call
static char *evaluate_file(const char *path) {
    snprintf(evaluated_file, sizeof evaluated_file, "%s", path);
    return evaluate("", 0, path);
}

// Stands in for js/main.js and js/editor-actions.js: fixed code, or the argument in upper case.
static char *call(const char *function, const char *argument) {
    if (!strcmp(function, "__randomExample")) return strdup("noise().out()");
    if (!strcmp(function, "__mutate")) return strdup("noise(9).out()");
    if (!strcmp(function, "__mutateUndo")) return strdup("noise().out()");
    if (!strcmp(function, "__completionNames")) return strdup("noise\nosc\nout\nmodulate");
    if (!strcmp(function, "__functionSignatures"))
        return strdup(set_function_called ? SIGNATURES "wobble\tcolor\tamount:float:0.5\n" : SIGNATURES);
    if (!strcmp(function, "__numberHelpers")) return strdup("cc[]\ntime");
    if (!strcmp(function, "__propertyNames")) return strdup(!strcmp(argument, "Math") ? "PI\nsin()" : "");
    if (!strcmp(function, "__languages")) return strdup("en\tEnglish\t1\nde\tDeutsch\t0");
    if (!strcmp(function, "__setLanguage")) {
        snprintf(language_now, sizeof language_now, "%s", argument);
        return strdup(argument);
    }
    if (!strcmp(function, "__urlForCode")) return strdup("https://hydra.ojack.xyz/?code=x");
    if (!strcmp(function, "__setPatchBayServer")) return strdup("");
    if (!strcmp(function, "__patchBayStreamId")) return strdup(patch_bay_server[0] ? STREAM_ID : "");
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

// The window and audio of the host, as main.c gives them, recorded for the checks of the settings rows.
static bool fullscreen_now, vsync_now = true;
static int audio_gain_now = 100;
static int dialogs; // the file dialogs that the host showed
static char chosen_path[256]; // the path that the next dialog answers
static char audio_now[256] = "(not set)";
static void toggle_fullscreen(void) { fullscreen_now = !fullscreen_now; }
static bool is_fullscreen(void) { return fullscreen_now; }
static void set_vsync(bool vsync) { vsync_now = vsync; }
static void set_audio_gain(int percent) { audio_gain_now = percent; }
static void set_server(const char *url) { snprintf(patch_bay_server, sizeof patch_bay_server, "%s", url); }
static int stream_rate_now, stream_max_rate_now;
static void set_stream_rates(int rate, int max_rate) {
    stream_rate_now = rate;
    stream_max_rate_now = max_rate;
}

// The file dialogs answer at once with chosen_path; ui_test opens no real dialog.
static void choose_file(enum file_dialog dialog) {
    dialogs++;
    switch (dialog) {
    case FILE_DIALOG_OPEN: ui_open_file(chosen_path); break;
    case FILE_DIALOG_SAVE: ui_save_as(chosen_path); break;
    case FILE_DIALOG_SETUP: ui_set_setup_file(chosen_path); break;
    }
}
static int audio_devices(struct audio_device *devices, int capacity) {
    snprintf(devices[0].name, sizeof devices[0].name, "mic-a");
    snprintf(devices[0].label, sizeof devices[0].label, "Microphone A");
    snprintf(devices[1].name, sizeof devices[1].name, AUDIO_SYSTEM_OUTPUT);
    snprintf(devices[1].label, sizeof devices[1].label, "system output");
    return 2;
}
static void set_audio(const char *device) { snprintf(audio_now, sizeof audio_now, "%s", device); }
static int quits; // the calls of the quit function of the host
static void quit(void) { quits++; }
static const struct ui_host test_host = { evaluate, evaluate_file, toggle_fullscreen, is_fullscreen, quit, call,
                                          nothing, share, set_title, set_vsync, audio_devices, set_audio,
                                          set_audio_gain, choose_file, set_server, set_stream_rates };

// ---------------------------------------------------------------- input helpers

static void key(SDL_Keycode code, Uint16 modifiers) {
    SDL_KeyboardEvent event = { .type = SDL_EVENT_KEY_DOWN, .down = true };
    event.key = code;
    event.mod = modifiers;
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

// The path hydra-native-ui-test-NAME-PID.EXTENSION in the temporary folder, with "/" before the name. Windows uses
// the working directory (the build folder in ctest): the settings menu cuts the long paths of %TEMP%, so the test
// could not click them.
static void temporary_path(char *path, size_t size, const char *name, const char *extension) {
#if defined(_WIN32)
    const char *folder = ".";
#else
    const char *folder = "/tmp";
#endif
    snprintf(path, size, "%s/hydra-native-ui-test-%s-%d%s", folder, name, (int)getpid(), extension);
}

static void temporary_file(char *path, size_t size, const char *name) {
    temporary_path(path, size, name, ".js");
    write_file(path, "");
}

// Clicks the `occurrence`-th (from 0) drawn text that equals `text`; false when the last frame has none.
static bool click_text(const char *text, int occurrence) {
    for (size_t i = 0; i < drawn_text_count; i++) {
        if (strcmp(drawn_texts[i].text, text) != 0 || occurrence-- > 0) continue;
        ui_mouse_button(drawn_texts[i].x + 3, drawn_texts[i].y + 3, true, 1, 0);
        ui_mouse_button(drawn_texts[i].x + 3, drawn_texts[i].y + 3, false, 1, 0);
        return true;
    }
    fprintf(stderr, "no drawn text \"%s\" (occurrence %d)\n", text, occurrence);
    return false;
}

// The overlay settings: the opacity and the blend modes of the code boxes and text, from the file and from the
// settings menu, applied at once and saved.
static void test_overlay_settings(const char *settings_file) {
    struct settings loaded;
    char error[SETTINGS_TEXT];
    write_file(settings_file, "{ \"codeBackgroundOpacity\": 40, \"codeBackgroundBlend\": \"difference\" }\n");
    settings_defaults(&loaded);
    EXPECT(loaded.code_background_opacity == SETTINGS_THEME_OPACITY && loaded.code_text_blend == CODE_BLEND_NORMAL);
    EXPECT(settings_load(&loaded, settings_file, error, sizeof error));
    EXPECT(loaded.code_background_opacity == 40 && loaded.code_background_blend == CODE_BLEND_DIFFERENCE &&
           loaded.code_text_blend == CODE_BLEND_NORMAL);

    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 600, 0);
    EXPECT(!difference_layers[OVERLAY_CODE_TEXT]);
    // The toolbar draws in the layers of the code, so it follows the same blend.
    int toolbar_texts = 0;
    for (size_t i = 0; i < drawn_text_count; i++)
        if (!strcmp(drawn_texts[i].text, "run") && drawn_texts[i].layer == OVERLAY_CODE_TEXT) toolbar_texts++;
    EXPECT(toolbar_texts == 1);
    EXPECT(click_text("difference", 1)); // the row "text blend"
    EXPECT(click_text("+", 1)); // "background opacity" (after "list opacity"), one step up from the theme value
    ui_draw(800, 600, 0);
    EXPECT(difference_layers[OVERLAY_CODE_TEXT] && !difference_layers[OVERLAY_CODE_BACKGROUND]);
    EXPECT(click_text("difference", 0)); // the row "background blend"
    ui_draw(800, 600, 0);
    EXPECT(difference_layers[OVERLAY_CODE_BACKGROUND]);
    key(SDLK_ESCAPE, 0);
    settings_defaults(&loaded);
    EXPECT(settings_load(&loaded, settings_file, error, sizeof error));
    EXPECT(loaded.code_text_blend == CODE_BLEND_DIFFERENCE && loaded.code_background_blend == CODE_BLEND_DIFFERENCE &&
           loaded.code_background_opacity > 0 && loaded.code_background_opacity <= 100);
    EXPECT_CONTAINS(read_file(settings_file), "\"codeTextBlend\": \"difference\"");
}

// The drawn text of the last frame that ends with `suffix`, such as the "%" of the opacity field, or "".
static const char *text_ending(const char *suffix) {
    for (size_t i = 0; i < drawn_text_count; i++) {
        size_t length = strlen(drawn_texts[i].text), suffix_length = strlen(suffix);
        if (length >= suffix_length && !strcmp(drawn_texts[i].text + length - suffix_length, suffix))
            return drawn_texts[i].text;
    }
    return "";
}

// The first drawn text of the last frame that starts with `prefix`, or NULL.
static const struct drawn_text *text_starting(const char *prefix) {
    for (size_t i = 0; i < drawn_text_count; i++)
        if (!strncmp(drawn_texts[i].text, prefix, strlen(prefix))) return &drawn_texts[i];
    return NULL;
}

// The first number of the settings menu: the opacity of the autocomplete list.
static int saved_opacity(const char *settings_file) {
    struct settings loaded;
    char error[SETTINGS_TEXT];
    settings_defaults(&loaded);
    settings_load(&loaded, settings_file, error, sizeof error);
    return loaded.completion_opacity;
}

// The numbers of the settings menu as text fields: typing, Enter, Escape, the clamp, the arrows, the wheel, an
// invalid entry, and a unit after the number.
static void test_number_fields(const char *settings_file) {
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 1200, 0);
    EXPECT(click_text(text_ending(" %"), 0));
    type("45");
    key(SDLK_RETURN, 0);
    EXPECT(saved_opacity(settings_file) == 45);
    ui_draw(800, 1200, 0);
    EXPECT_CONTAINS(drawn, "45 %\n");

    EXPECT(click_text("45 %", 0));
    type("250%"); // above the range: 100
    key(SDLK_RETURN, 0);
    EXPECT(saved_opacity(settings_file) == 100);

    ui_draw(800, 1200, 0);
    EXPECT(click_text("100 %", 0));
    type("abc"); // no number: the field stays open with the text, nothing changes
    key(SDLK_RETURN, 0);
    ui_draw(800, 1200, 0);
    EXPECT_CONTAINS(drawn, "abc|\n");
    EXPECT(saved_opacity(settings_file) == 100);
    key(SDLK_ESCAPE, 0); // cancels the field; the settings menu stays open
    ui_draw(800, 1200, 0);
    EXPECT(strstr(drawn, "abc") == NULL);
    EXPECT_CONTAINS(drawn, "100 %\n");
    EXPECT(saved_opacity(settings_file) == 100);

    EXPECT(click_text("100 %", 0));
    key(SDLK_DOWN, 0); // applied at once
    EXPECT(saved_opacity(settings_file) == 99);
    key(SDLK_DOWN, SDL_KMOD_LSHIFT);
    EXPECT(saved_opacity(settings_file) == 89);
    key(SDLK_UP, 0);
    EXPECT(saved_opacity(settings_file) == 90);
    ui_wheel(-1);
    EXPECT(saved_opacity(settings_file) == 89);
    key(SDLK_BACKSPACE, 0); // the selected text goes
    type("7");
    key(SDLK_RETURN, 0);
    EXPECT(saved_opacity(settings_file) == 7);

    ui_draw(800, 1200, 0);
    EXPECT(click_text(text_ending(" px"), 0));
    type("30px");
    key(SDLK_RETURN, 0);
    EXPECT(overlay_font_size() == 30);
    ui_draw(800, 1200, 0);
    EXPECT(click_text("30 px", 0));
    type("2"); // below the range
    key(SDLK_RETURN, 0);
    EXPECT(overlay_font_size() == OVERLAY_MIN_FONT_SIZE);
    overlay_set_font_size(20);
    key(SDLK_ESCAPE, 0); // closes the settings menu
    ui_draw(800, 1200, 0);
    EXPECT(strstr(drawn, "code display") == NULL);
}

static struct settings saved_settings(const char *settings_file) {
    struct settings loaded;
    char error[SETTINGS_TEXT];
    settings_defaults(&loaded);
    settings_load(&loaded, settings_file, error, sizeof error);
    return loaded;
}

// The rows of the settings menu for the options that also exist as flags: each applies at once (or says that it
// applies at the next start) and goes into settings.json.
static void test_settings_rows(const char *settings_file) {
    write_file(settings_file, "{}\n"); // a file before was broken on purpose, and a broken file is not overwritten
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    EXPECT(click_text("vsync off", 0));
    EXPECT(!vsync_now && !saved_settings(settings_file).vsync);
    EXPECT(click_text("fullscreen", 0));
    EXPECT(fullscreen_now && saved_settings(settings_file).fullscreen);
    ui_draw(800, 900, 0);
    EXPECT(click_text("fullscreen", 0)); // selected now: a click leaves fullscreen
    EXPECT(!fullscreen_now && !saved_settings(settings_file).fullscreen);

    ui_draw(800, 900, 0);
    EXPECT(click_text("100 %", 0)); // the input gain
    type("250");
    key(SDLK_RETURN, 0);
    EXPECT(audio_gain_now == 250 && saved_settings(settings_file).audio_gain == 250);
    ui_draw(800, 900, 0);
    EXPECT(click_text("250 %", 0));
    type("900%"); // above the range: 400
    key(SDLK_RETURN, 0);
    EXPECT(audio_gain_now == 400 && saved_settings(settings_file).audio_gain == 400);

    ui_draw(800, 900, 0);
    EXPECT(click_text("60 fps", 0)); // stream rate out
    type("30 fps");
    key(SDLK_RETURN, 0);
    EXPECT(stream_rate_now == 30 && saved_settings(settings_file).stream_rate == 30);
    ui_draw(800, 900, 0);
    EXPECT(click_text("60 fps", 0)); // stream rate in (max)
    type("0"); // below the range: 1
    key(SDLK_RETURN, 0);
    EXPECT(stream_max_rate_now == 1 && saved_settings(settings_file).stream_max_rate == 1);
    ui_draw(800, 900, 0);
    EXPECT(click_text("+", 3)); // of stream rate in (max), after the two opacities and the stream rate out
    EXPECT(stream_max_rate_now == 6 && saved_settings(settings_file).stream_max_rate == 6);
    EXPECT(stream_rate_now == 30 && saved_settings(settings_file).stream_rate == 30);

    ui_draw(800, 900, 0);
    EXPECT(click_text("Microphone A", 0));
    EXPECT_STRING(audio_now, "mic-a");
    EXPECT_STRING(saved_settings(settings_file).audio_source, "mic-a");
    ui_draw(800, 900, 0);
    EXPECT(click_text("system output", 0));
    EXPECT_STRING(audio_now, AUDIO_SYSTEM_OUTPUT);
    ui_draw(800, 900, 0);
    EXPECT(click_text("none", 1)); // the first "none" is of the whitespace row
    EXPECT_STRING(audio_now, AUDIO_NONE);
    EXPECT_STRING(saved_settings(settings_file).audio_source, AUDIO_NONE);

    ui_draw(800, 900, 0);
    EXPECT(click_text("Deutsch", 0));
    EXPECT_STRING(language_now, "de");
    EXPECT_STRING(saved_settings(settings_file).language, "de");

    ui_draw(800, 900, 0);
    EXPECT(click_text("hidden", 0)); // code at start
    EXPECT(!saved_settings(settings_file).code_visible);
    ui_draw(800, 900, 0);
    EXPECT(click_text("on", 0)); // reload on file change, the first row under "file"
    EXPECT(ui_settings()->watch && saved_settings(settings_file).watch);

    ui_draw(800, 900, 0);
    EXPECT(click_text("translation URL...", 0)); // a prompt; the menu closes
    type("http://127.0.0.1:9/editor.json");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(saved_settings(settings_file).l10n_url, "http://127.0.0.1:9/editor.json");

    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    EXPECT(click_text("file...", 0)); // the font path, for systems without a font list
    type("/tmp/some-font.ttf"); // replaces the selected path
    key(SDLK_RETURN, 0);
    EXPECT_STRING(saved_settings(settings_file).font, "/tmp/some-font.ttf");
}

// The "file" row and Ctrl+S with the file dialogs of the host (stubs here): open a sketch, save to a new file when
// none is open, the file name in the window title, and the reload row, which is dim without a file.
static void test_files(const char *settings_file) {
    char other[256];
    temporary_file(other, sizeof other, "other");
    write_file(other, "noise(3).out()");
    snprintf(chosen_path, sizeof chosen_path, "%s", other);
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    EXPECT(click_text(ui_file_path(), 0));
    type("d"); // the open file has unsaved edits of the tests before
    EXPECT(dialogs == 1);
    EXPECT_STRING(ui_file_path(), other);
    EXPECT_STRING(evaluated, "noise(3).out()"); // the opened sketch runs
    EXPECT_CONTAINS(title, strrchr(other, '/') + 1);
    type("x");
    key(SDLK_S, SDL_KMOD_LCTRL); // saves to the open file
    EXPECT(dialogs == 1);
    EXPECT_STRING(read_file(other), "xnoise(3).out()");
    unlink(other);

    // Without a file (a start without a file argument): no reload, and Ctrl+S asks where to save.
    struct settings defaults;
    settings_defaults(&defaults);
    ui_init(&defaults, settings_file, NULL, &test_host);
    EXPECT(ui_file_path() == NULL);
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    // The file row shows a dim placeholder in the color of the dim "on", and a click on it opens the dialog.
    const struct drawn_text *placeholder = text_starting("choose a file...");
    const struct drawn_text *dim = text_starting("on");
    EXPECT(placeholder && dim && placeholder->color == dim->color);
    EXPECT(placeholder && click_text(placeholder->text, 0));
    EXPECT(dialogs == 2); // its answer is the deleted file of above, which does not open
    EXPECT(ui_file_path() == NULL);
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    EXPECT(click_text("on", 0)); // the dim "on" of the reload has no button
    EXPECT(!ui_settings()->watch);
    key(SDLK_ESCAPE, 0);
    ui_set_code("osc(4).out()");
    char saved[256];
    temporary_file(saved, sizeof saved, "saved");
    snprintf(chosen_path, sizeof chosen_path, "%s", saved);
    key(SDLK_S, SDL_KMOD_LCTRL);
    EXPECT(dialogs == 3);
    EXPECT_STRING(ui_file_path(), saved);
    EXPECT_STRING(read_file(saved), "osc(4).out()");
    EXPECT_CONTAINS(title, strrchr(saved, '/') + 1);

    // With the file open, the toolbar says "reset" and opens a list: its "reset" brings back the saved code, and its
    // "clear" closes the file. Ctrl+Shift+Delete resets.
    ui_draw(800, 600, 0);
    EXPECT(click_text("reset", 0));
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "reset\nclear\n");
    EXPECT(click_text("reset", 1)); // the entry below the button
    EXPECT_STRING(evaluated, "osc(4).out()");
    type("x");
    key(SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("d");
    EXPECT_STRING(ui_file_path(), saved);
    EXPECT_STRING(evaluated, "osc(4).out()");
    ui_draw(800, 600, 0);
    EXPECT(click_text("reset", 0));
    key(SDLK_DOWN, 0);
    key(SDLK_RETURN, 0);
    EXPECT(ui_file_path() == NULL);
    EXPECT_STRING(evaluated, "hush(); speed = 1");
    // An example closes the file, so Ctrl+S cannot overwrite it with the example.
    key(SDLK_F7, 0);
    EXPECT(ui_file_path() == NULL);
    ui_draw(800, 600, 0);
    EXPECT(click_text("clear", 0) || !click_text("reset", 0));
    char after_example[256];
    temporary_file(after_example, sizeof after_example, "example");
    snprintf(chosen_path, sizeof chosen_path, "%s", after_example);
    key(SDLK_S, SDL_KMOD_LCTRL);
    EXPECT(dialogs == 4); // asks for a file instead of saving to the old one
    EXPECT_STRING(read_file(saved), "osc(4).out()");
    unlink(saved);
    unlink(after_example);
}

// The "setup file" row: the chosen file runs and goes into settings.json, and the editor shows it as a dim line
// above the code. The line is no part of the code: the first code row stays line 0, and saving leaves it out. A
// click on it runs the setup file again; "clear" removes the setting and the line.
static void test_setup_file(const char *settings_file) {
    char folder[200], sketch[256], setup[256];
    temporary_path(folder, sizeof folder, "setup", "");
    EXPECT(SDL_CreateDirectory(folder));
    snprintf(sketch, sizeof sketch, "%s/sketch.js", folder);
    snprintf(setup, sizeof setup, "%s/setup.js", folder);
    write_file(sketch, "osc(1).out()\nnoise().out()");
    write_file(setup, "midi = {}");
    struct settings defaults;
    settings_defaults(&defaults);
    ui_init(&defaults, settings_file, sketch, &test_host);
    ui_draw(800, 600, 0);
    EXPECT(text_starting("await loadScript") == NULL);
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    snprintf(chosen_path, sizeof chosen_path, "%s", setup);
    snprintf(next_error, sizeof next_error, "ReferenceError: 'midi' is not defined");
    const struct drawn_text *placeholder = text_starting("choose a setup file...");
    EXPECT(placeholder && click_text(placeholder->text, 0));
    EXPECT_STRING(evaluated_file, setup);
    EXPECT_STRING(saved_settings(settings_file).setup_file, setup);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "setup: ReferenceError: 'midi' is not defined");

    const char *line = "await loadScript(\"setup.js\")"; // relative to the folder of the sketch
    EXPECT_CONTAINS(drawn, line);
    EXPECT(click_text("osc(1).out()", 0)); // the first code row, below the setup line
    type("x");
    EXPECT_STRING(ui_code(), "xosc(1).out()\nnoise().out()");
    key(SDLK_S, SDL_KMOD_LCTRL);
    EXPECT_STRING(read_file(sketch), "xosc(1).out()\nnoise().out()");
    evaluated_file[0] = '\0';
    ui_draw(800, 600, 0);
    EXPECT(click_text(line, 0));
    EXPECT_STRING(evaluated_file, setup);
    EXPECT_STRING(ui_code(), "xosc(1).out()\nnoise().out()");

    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 900, 0);
    EXPECT(click_text("clear", 0));
    EXPECT_STRING(saved_settings(settings_file).setup_file, "");
    ui_draw(800, 600, 0);
    EXPECT(strstr(drawn, "await loadScript") == NULL);
    unlink(sketch);
    unlink(setup);
    rmdir(folder);
}

// The question before the unsaved changes of the open file are lost: Escape keeps them and the file, s saves them
// and then resets, d discards them and resets. Quit asks too. Without changes, nothing asks.
static void test_unsaved_question(const char *settings_file) {
    char sketch[256];
    temporary_path(sketch, sizeof sketch, "unsaved", ".js");
    write_file(sketch, "osc(1).out()");
    struct settings defaults;
    settings_defaults(&defaults);
    ui_init(&defaults, settings_file, sketch, &test_host);
    type("x");
    key(SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT); // reset
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "unsaved changes in hydra-native-ui-test-unsaved-");
    EXPECT_CONTAINS(drawn, ".js: s save, d discard, Escape cancel");
    key(SDLK_ESCAPE, 0);
    ui_draw(800, 600, 0);
    EXPECT(strstr(drawn, "unsaved changes") == NULL);
    EXPECT_STRING(ui_code(), "xosc(1).out()");
    EXPECT_STRING(ui_file_path(), sketch);
    EXPECT_STRING(read_file(sketch), "osc(1).out()");

    key(SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("s");
    EXPECT_STRING(read_file(sketch), "xosc(1).out()");
    EXPECT_STRING(evaluated, "xosc(1).out()"); // the reset runs the saved file

    type("y");
    key(SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("d");
    EXPECT_STRING(ui_code(), "xosc(1).out()");
    EXPECT_STRING(read_file(sketch), "xosc(1).out()");

    int quits_before = quits;
    type("z");
    key(SDLK_Q, SDL_KMOD_LCTRL);
    EXPECT(quits == quits_before);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "unsaved changes in");
    type("d");
    EXPECT(quits == quits_before + 1);

    key(SDLK_S, SDL_KMOD_LCTRL); // saved: nothing asks
    ui_quit(); // the close button of the window
    EXPECT(quits == quits_before + 2);
    unlink(sketch);
}

// The path of the newest recovery file (src/settings.h) in `path`, and the number of all of them.
static size_t newest_recovery(char *path, size_t size) {
    size_t count;
    char **paths = recovery_list(&count);
    snprintf(path, size, "%s", count ? paths[0] : "");
    for (size_t index = 0; index < count; index++) free(paths[index]);
    free(paths);
    return count;
}

// The recovery stores resolved paths; on macOS /tmp is a link to /private/tmp, and Windows writes backslashes.
static char *resolved_path(const char *path) {
#if defined(_WIN32)
    return _fullpath(NULL, path, 0);
#else
    return realpath(path, NULL);
#endif
}

static bool same_path(const char *first, const char *second) {
    char *first_resolved = resolved_path(first), *second_resolved = resolved_path(second);
    bool same = first_resolved && second_resolved ? !strcmp(first_resolved, second_resolved) : !strcmp(first, second);
    free(first_resolved);
    free(second_resolved);
    return same;
}

static void expect_recovery(const char *path, const char *code, const char *file) {
    char *saved_file = NULL, *saved_code = recovery_load(path, &saved_file);
    EXPECT(saved_code && !strcmp(saved_code, code) && saved_file && same_path(saved_file, file));
    if (!saved_code) fprintf(stderr, "no recovery in %s\n", path);
    free(saved_code);
    free(saved_file);
}

// The recovery files in the temporary XDG_CONFIG_HOME. Each run writes its own file, named by the time of its first
// edit, and two runs of the same minute do not clash. A start restores the newest file of its sketch, or the newest
// of all without a file, also the recovery.js of older versions; the bar counts the kept ones. Ctrl+S removes the
// restored file and the file of the run, and leaves the others.
static void test_recovery(const char *settings_file) {
    char sketch[256], other[256], another[256];
    temporary_path(sketch, sizeof sketch, "recovery", ".js");
    temporary_path(other, sizeof other, "old", ".js");
    temporary_path(another, sizeof another, "another", ".js");
    write_file(sketch, "osc(1).out()");
    write_file(other, "noise(1).out()");
    char old[256], first[256], second[256], path[256];
    snprintf(old, sizeof old, "%s", settings_file);
    snprintf(strrchr(old, '/'), sizeof old - (size_t)(strrchr(old, '/') - old), "/recovery.js");
    EXPECT(recovery_save(old, other, "noise(2).out()"));
    // The times of the files, oldest first, so that the order does not depend on the seconds of the run.
    struct utimbuf times[] = { { 0, 0 }, { 1000, 1000 }, { 2000, 2000 } };
    utime(old, &times[0]);
    struct settings defaults;
    settings_defaults(&defaults);

    ui_init(&defaults, settings_file, sketch, &test_host);
    ui_draw(800, 600, 10000);
    EXPECT_CONTAINS(drawn, "1 recovery file kept next to settings.json");
    char earlier[32], later[32];
    time_t edited = time(NULL);
    strftime(earlier, sizeof earlier, "recovery-%y%m%d-%H%M.js", localtime(&edited));
    type("a");
    ui_draw(800, 600, 11000);
    edited = time(NULL);
    strftime(later, sizeof later, "recovery-%y%m%d-%H%M.js", localtime(&edited));
    ui_draw(800, 600, 12000);
    EXPECT(newest_recovery(first, sizeof first) == 1); // not yet: the edit is one second old
    ui_draw(800, 600, 13000);
    EXPECT(newest_recovery(first, sizeof first) == 2);
    const char *name = strrchr(first, '/') + 1;
    EXPECT(!strcmp(name, earlier) || !strcmp(name, later));
    expect_recovery(first, "aosc(1).out()", sketch);
    utime(first, &times[1]);

    // A restart after a crash, with another file: it keeps both files and writes a third one.
    ui_init(&defaults, settings_file, another, &test_host);
    ui_draw(800, 600, 20000);
    EXPECT_CONTAINS(drawn, "2 recovery files kept next to settings.json");
    type("b");
    ui_draw(800, 600, 21000);
    ui_draw(800, 600, 23000);
    EXPECT(newest_recovery(path, sizeof path) == 3);
    snprintf(second, sizeof second, "%s", strcmp(path, first) ? path : "");
    EXPECT(second[0]);
    expect_recovery(first, "aosc(1).out()", sketch);
    expect_recovery(second, "b", another);
    utime(second, &times[2]);

    // A restart with the sketch restores its file; Ctrl+S removes it and the file of this run.
    ui_init(&defaults, settings_file, sketch, &test_host);
    EXPECT_STRING(ui_code(), "aosc(1).out()");
    EXPECT(ui_file_path() && same_path(ui_file_path(), sketch));
    EXPECT(ui_modified());
    ui_draw(800, 600, 30000);
    EXPECT_CONTAINS(drawn, "restored the unsaved changes of hydra-native-ui-test-recovery-");
    EXPECT_CONTAINS(drawn, "2 recovery files kept");
    type("x");
    ui_draw(800, 600, 31000);
    ui_draw(800, 600, 33000);
    EXPECT(newest_recovery(path, sizeof path) == 4);
    expect_recovery(path, "xaosc(1).out()", sketch);
    key(SDLK_S, SDL_KMOD_LCTRL);
    EXPECT_STRING(read_file(sketch), "xaosc(1).out()");
    EXPECT(access(first, F_OK) != 0 && access(path, F_OK) != 0);
    EXPECT(newest_recovery(path, sizeof path) == 2);
    EXPECT_STRING(path, second);

    // Without a file, the newest of all comes back; recovery.js comes back with its file.
    ui_init(&defaults, settings_file, NULL, &test_host);
    EXPECT_STRING(ui_code(), "b");
    EXPECT(ui_file_path() && same_path(ui_file_path(), another));
    ui_init(&defaults, settings_file, other, &test_host);
    EXPECT_STRING(ui_code(), "noise(2).out()");
    key(SDLK_S, SDL_KMOD_LCTRL);
    EXPECT(access(old, F_OK) != 0);
    unlink(second);
    unlink(another);
    unlink(sketch);
    unlink(other);
}

// The call around the cursor: nested calls, commas in arrays, arrow functions and strings.
static void expect_call(const char *text, const char *name, size_t argument, bool direct) {
    struct signature list[8];
    size_t count = signatures_parse(SIGNATURES, list, 8);
    struct call_context call;
    const struct signature *found = NULL;
    const char *cursor = strchr(text, '|');
    char code[256];
    snprintf(code, sizeof code, "%.*s%s", (int)(cursor - text), text, cursor + 1);
    bool inside = signature_call_at(code, (size_t)(cursor - text), list, count, &call, &found);
    if (!name) {
        EXPECT(!inside);
        return;
    }
    if (!inside) {
        fprintf(stderr, "no call found in \"%s\", expected %s\n", text, name);
        failures++;
        return;
    }
    if (strcmp(found->name, name) || call.argument != argument || call.direct != direct) {
        fprintf(stderr, "in \"%s\": got %s argument %zu direct %d, expected %s argument %zu direct %d\n", text,
                found->name,
                call.argument, call.direct, name, argument, direct);
        failures++;
    }
}

// Typed autocomplete, signature help and the tab stops of an inserted call (src/signature.c and src/ui.c).
static void test_signatures(void) {
    expect_call("osc(|", "osc", 0, true);
    expect_call("osc(1, noise(3, |))", "noise", 1, true);
    expect_call("osc(1, noise(3, 4), |)", "osc", 2, true);
    expect_call("osc([1, 2, |3], 0.1)", "osc", 0, false);
    expect_call("osc([1, 2, 3], |0.1)", "osc", 1, true);
    expect_call("osc(() => Math.sin(time, |2), 0.1)", "osc", 0, false);
    expect_call("osc((a, b) => a, 0.1, |)", "osc", 2, true);
    expect_call("osc(\"a,b\", |)", "osc", 1, true);
    expect_call("osc(1 /* a, b */, |)", "osc", 1, true);
    expect_call("osc().modulate(o1, |", "modulate", 1, true);
    expect_call("osc(1).out(|)", NULL, 0, false);
    struct signature list[8];
    size_t count = signatures_parse(SIGNATURES, list, 8);
    char text[128];
    signature_snippet(signature_find(list, count, "modulate", 8), text, sizeof text);
    EXPECT_STRING(text, "modulate(${o0}, ${0.1})");

    // The tests before leave the trigger "while typing"; these use Ctrl+Space.
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("autocomplete on Ctrl+Space or while");
    key(SDLK_RETURN, 0);

    // Signature help on Ctrl+Shift+Space: the current parameter follows the cursor between the commas.
    ui_set_code("osc(10, 0.2).modulate(o1, 0.3).out()");
    key(SDLK_HOME, SDL_KMOD_LCTRL);
    for (int i = 0; i < 27; i++) key(SDLK_RIGHT, 0); // after "modulate(o1, "
    key(SDLK_SPACE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "modulate(texture, amount = 0.1)\namount: float, default 0.1\n");
    for (int i = 0; i < 5; i++) key(SDLK_LEFT, 0);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "texture: vec4 (texture)");
    key(SDLK_HOME, 0);
    for (int i = 0; i < 8; i++) key(SDLK_RIGHT, 0); // after "osc(10, "
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "osc(frequency = 60, sync = 0.1, offset = 0)\nsync: float, default 0.1\n");
    ui_set_code("s0.initCam()");
    key(SDLK_END, 0);
    key(SDLK_LEFT, 0);
    key(SDLK_SPACE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT); // a function outside the glsl list (js/api-signatures.js)
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "initCam(index?, params?)\nindex?: number\n");
    key(SDLK_END, 0); // outside every call: the help closes
    ui_draw(800, 600, 0);
    EXPECT(strstr(drawn, "frequency = 60") == NULL);

    // Completions by type: textures, numbers, and transforms after a ".".
    ui_set_code("osc().modulate()");
    key(SDLK_END, 0);
    key(SDLK_LEFT, 0);
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "o0\no1\no2\no3\ns0\ns1\ns2\ns3\nsrc\nosc\nnoise\n");
    EXPECT(strstr(drawn, "rotate\n") == NULL);
    key(SDLK_ESCAPE, 0);
    type("o2, ");
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "0.1\n() => \n[ ]\ncc[]\ntime\n");
    key(SDLK_DOWN, 0);
    key(SDLK_DOWN, 0);
    key(SDLK_DOWN, 0);
    key(SDLK_RETURN, 0); // cc[] in an arrow function, the index selected
    type("3");
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc().modulate(o2, () => cc[3])");

    // The types decide: src() takes outputs and sources only, s0. and a. have their own methods, Math. gets its
    // properties from the runtime and no hydra names, render() takes an output, and a statement does not start with a transform.
    struct { const char *code; const char *present; const char *absent; } typed[] = {
        { "src(|)", "o0\no1\no2\no3\ns0\ns1\ns2\ns3\n", "osc\n" },
        { "s0.|", "initCam\n", "modulate\n" },
        { "a.|", "setBins\n", "modulate\n" },
        { "Math.|", "PI\nsin\n", "modulate\n" },
        { "render(|)", "o0\no1\no2\no3\n", "s0\n" },
        { "mod|", NULL, "modulate\n" },
        { "osc().mod|", "modulate\n", NULL },
        { "speed = 2; osc(sp|)", "speed\n", NULL }, // a variable of the sketch fits a number
        { "osc().modulate(); mod|", NULL, "modulate\n" }, // a transform of the file needs a chain
    };
    for (size_t i = 0; i < sizeof typed / sizeof typed[0]; i++) {
        const char *bar = strchr(typed[i].code, '|');
        char code[64];
        snprintf(code, sizeof code, "%.*s%s", (int)(bar - typed[i].code), typed[i].code, bar + 1);
        ui_set_code(code);
        key(SDLK_HOME, 0);
        for (const char *c = typed[i].code; c < bar; c++) key(SDLK_RIGHT, 0);
        key(SDLK_SPACE, SDL_KMOD_LCTRL);
        ui_draw(800, 600, 0);
        if (typed[i].present) EXPECT_CONTAINS(drawn, typed[i].present);
        if (typed[i].absent && strstr(drawn, typed[i].absent)) {
            fprintf(stderr, "%s:%d: %s offers %s", __FILE__, __LINE__, typed[i].code, typed[i].absent);
            failures++;
        }
        key(SDLK_ESCAPE, 0);
    }

    // A function of a global object goes in as a call with the cursor inside the parentheses.
    ui_set_code("Math.s");
    key(SDLK_END, 0);
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, 0);
    type("time");
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "Math.sin(time)");

    // Tab stops: the transform with its defaults, Tab and Shift+Tab between the arguments, the last Tab leaves.
    ui_set_code("osc().");
    key(SDLK_END, 0);
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "modulate\nrotate\nout\n");
    EXPECT(strstr(drawn, "noise\n") == NULL);
    key(SDLK_RETURN, 0); // modulate
    type("s1");
    key(SDLK_TAB, 0);
    type("0.25");
    key(SDLK_TAB, SDL_KMOD_LSHIFT);
    type("o3");
    key(SDLK_TAB, 0);
    key(SDLK_TAB, 0);
    type(".out()");
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc().modulate(o3, 0.25).out()");
    key(SDLK_HOME, 0);
    type("rot");
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_ESCAPE, 0); // closes the list
    ui_set_code("osc().rot");
    key(SDLK_END, 0);
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, 0);
    key(SDLK_ESCAPE, 0); // ends the tab stops, the selected angle stays
    type("1");
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc().rotate(1, 0)");

    // A function of setFunction appears after the evaluation.
    ui_set_code("setFunction({ name: 'wobble' })\nosc().wob");
    key(SDLK_HOME, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    key(SDLK_END, SDL_KMOD_LCTRL);
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, 0);
    key(SDLK_ESCAPE, 0); // ends the tab stops
    key(SDLK_ESCAPE, 0); // clears the selection
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc().wobble(0.5)");
}

#define LONG_SERVER "http://127.0.0.1:9/a-long-path-of-a-custom-server"

// Random text with a fixed seed for the two parsers of outside input: a settings file and the code around the
// cursor. They must not read or write outside their buffers (the sanitizers check that) and must keep their limits.
#define RANDOM_INPUTS 3000

static void test_random_input(const char *settings_file) {
    static const char *const json[] = { "{", "}", "[", "]", ":", ",", "\"", "\\", "theme", "themes", "keybindings",
                                        "fontSize", "audioGain", "autocompleteOpacity", "base", "#ff00", "-1", "1e99",
                                        "true", "null", "\xFF", " ", "Ctrl+", "\n", "join_lines", "\"a\":\"b\"" };
    static const char *const code[] = { "osc(", "noise(", ")", ",", ".", "'", "\"", "`", "${", "}", "[", "]", " ",
                                        "\\", "=>", "//", "/*", "\n", "wobble", "1", "\xE6\x97\xA5" };
    struct signature list[8];
    size_t count = signatures_parse(SIGNATURES, list, 8);
    srand(1);
    for (int i = 0; i < RANDOM_INPUTS; i++) {
        char text[512] = "";
        const char *const *pieces = i % 2 ? json : code;
        unsigned piece_count = i % 2 ? sizeof json / sizeof json[0] : sizeof code / sizeof code[0];
        for (int k = (int)((unsigned)rand() % 40); k > 0 && strlen(text) < sizeof text - 32; k--)
            strcat(text, pieces[(unsigned)rand() % piece_count]);
        if (i % 2) {
            struct settings loaded;
            char error[SETTINGS_TEXT];
            write_file(settings_file, text);
            settings_defaults(&loaded);
            settings_load(&loaded, settings_file, error, sizeof error);
            EXPECT(loaded.theme_count <= MAX_CUSTOM_THEMES && loaded.keybinding_count <= MAX_KEYBINDINGS);
            EXPECT(loaded.completion_opacity >= 0 && loaded.completion_opacity <= 100);
            EXPECT(loaded.audio_gain >= 0 && loaded.audio_gain <= SETTINGS_MAX_AUDIO_GAIN);
        } else {
            struct call_context call;
            const struct signature *found = NULL;
            size_t offset = (unsigned)rand() % (strlen(text) + 1);
            if (signature_call_at(text, offset, list, count, &call, &found))
                EXPECT(found && call.name_start <= offset);
        }
    }
}

int main(void) {
    // The dummy video driver gives SDL a clipboard without a window.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    EXPECT(SDL_Init(SDL_INIT_VIDEO));
    char path[256];
    temporary_path(path, sizeof path, "sketch", "");
    write_file(path, "osc(10)\n  .out()\n\nnoise().out()");
    // The settings file lives in a temporary XDG_CONFIG_HOME, never in the real configuration of the user.
    char config[256], settings_file[256];
    temporary_path(config, sizeof config, "config", "");
    EXPECT(SDL_CreateDirectory(config));
#if defined(_WIN32)
    _putenv_s("XDG_CONFIG_HOME", config); // getenv of the C runtime does not see SetEnvironmentVariable of SDL
#else
    setenv("XDG_CONFIG_HOME", config, 1);
#endif
    EXPECT(settings_path(settings_file, sizeof settings_file));
    EXPECT_STRING(settings_file + strlen(config), "/hydra-native/settings.json");
    char error[SETTINGS_TEXT];
    // An empty settings object writes the directory and the file.
    struct settings stored;
    settings_defaults(&stored);
    EXPECT(settings_save(&stored, settings_file, error, sizeof error));
    write_file(settings_file,
               "{\n  \"fontSize\": 24,\n  \"theme\": \"mine\",\n  \"galleryServer\": \"http://127.0.0.1:9\",\n"
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
    snprintf(loaded.theme, sizeof loaded.theme, "contrast");
    ui_init(&loaded, settings_file, path, &test_host);
    ui_draw(800, 600, 0);

    key(SDLK_RETURN, SDL_KMOD_LALT); // the block around the cursor (line 0)
    EXPECT_STRING(evaluated, "osc(10)\n  .out()");
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc(10)");
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    EXPECT_STRING(evaluated, "osc(10)\n  .out()\n\nnoise().out()");

    key(SDLK_END, SDL_KMOD_LCTRL); // to the end, then type a new line
    type("\nshape(4).out(o1)");
    key(SDLK_RETURN, SDL_KMOD_LSHIFT);
    EXPECT_STRING(evaluated, "shape(4).out(o1)");

    key(SDLK_SLASH, SDL_KMOD_LCTRL); // comment the last line, then save
    key(SDLK_S, SDL_KMOD_LCTRL);
    EXPECT_STRING(read_file(path), "osc(10)\n  .out()\n\nnoise().out()\n// shape(4).out(o1)");

    key(SDLK_Z, SDL_KMOD_LCTRL); // undo the comment, then hide the code: typing and editing keys stop working
    key(SDLK_H, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("xyz");
    key(SDLK_BACKSPACE, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT); // evaluation still works
    EXPECT_STRING(evaluated, "osc(10)\n  .out()\n\nnoise().out()\nshape(4).out(o1)");
    key(SDLK_H, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);

    // --watch: unsaved edits stay, a saved buffer takes the new file content and evaluates it.
    write_file(path, "solid().out()");
    ui_file_changed();
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "osc(10)\n  .out()\n\nnoise().out()\nshape(4).out(o1)");
    key(SDLK_S, SDL_KMOD_LCTRL);
    write_file(path, "solid().out()");
    evaluated[0] = '\0';
    ui_file_changed();
    EXPECT_STRING(evaluated, "solid().out()");

    // An error leaves the buffer as it is.
    snprintf(next_error, sizeof next_error, "ReferenceError: 'x' is not defined\nstack");
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "solid().out()");

    // Shift+Ctrl+F formats with editor_format: the spaces at the line start go.
    key(SDLK_HOME, 0);
    type("   ");
    key(SDLK_F, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "solid().out()");

    // The toolbar actions. With an open file, clear all resets to the saved file and runs it again; first it asks,
    // because the formatted code is not saved, and d discards it.
    key(SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("d");
    EXPECT_STRING(ui_file_path(), path);
    EXPECT_STRING(evaluated, read_file(path)); // the saved state, which an earlier Ctrl+S wrote
    // An example replaces the code, runs it and closes the file, whose content it is not.
    key(SDLK_F7, 0);
    EXPECT_STRING(evaluated, "noise().out()");
    EXPECT(ui_file_path() == NULL);
    // Without a file, clear all evaluates hush and empties the code.
    key(SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    EXPECT_STRING(evaluated, "hush(); speed = 1");
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated,
                  "\n \n // Type some code on a new line (such as \"osc().out()\"), and press CTRL+shift+enter");
    key(SDLK_F7, 0);
    EXPECT_STRING(evaluated, "noise().out()");
    key(SDLK_F8, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "noise(9).out()");
    key(SDLK_F8, SDL_KMOD_LSHIFT);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "noise().out()");

    ui_open_file(path); // the example closed the file; the steps below need one open
    // Find, replace, go to line, command palette and autocomplete, all through the key table.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("osc(1).out()\nosc(2).out()\nnoise().out()");
    key(SDLK_H, SDL_KMOD_LCTRL);
    type("osc");
    key(SDLK_TAB, 0);
    type("shape");
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT); // in the prompt: replace all
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()");
    EXPECT_CONTAINS(title, " - https://hydra.ojack.xyz/?code=x"); // the file name, then the URL of evaluate all
    key(SDLK_G, SDL_KMOD_LCTRL);
    type("2");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "shape(2).out()");
    // Ctrl+Alt+Enter replaces all only in the replace prompt; in another prompt it evaluates all, and the code stays.
    key(SDLK_G, SDL_KMOD_LCTRL);
    type("2");
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    key(SDLK_ESCAPE, 0);
    EXPECT_STRING(ui_code(), "shape(1).out()\nshape(2).out()\nnoise().out()");
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()");
    // The palette reaches the commands of the autocomplete list; without a list they do nothing.
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("next completion");
    key(SDLK_RETURN, 0);
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("insert the completion");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(ui_code(), "shape(1).out()\nshape(2).out()\nnoise().out()");
    key(SDLK_F, SDL_KMOD_LCTRL);
    type("noise");
    key(SDLK_RETURN, 0);
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL); // evaluates the selected match
    EXPECT_STRING(evaluated, "noise");

    // The find bar of the browser: no case, a count of the matches, Enter and Shift+Enter around the end,
    // Escape keeps the match selected, F3 opens it again with the last query.
    key(SDLK_HOME, SDL_KMOD_LCTRL);
    key(SDLK_F, SDL_KMOD_LCTRL);
    type("SHAPE(");
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(bar_text, "   1/2");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, 0);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(bar_text, "   1/2");
    key(SDLK_RETURN, SDL_KMOD_LSHIFT);
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "shape(");
    key(SDLK_F3, 0);
    key(SDLK_ESCAPE, 0);
    key(SDLK_RIGHT, 0);
    type("9");
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "shape(91).out()\nshape(2).out()\nnoise().out()");
    key(SDLK_Z, SDL_KMOD_LCTRL);
    key(SDLK_F, SDL_KMOD_LCTRL);
    type("xyz");
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(bar_text, "   0/0");
    key(SDLK_ESCAPE, 0);
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("evaluate all");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()");

    // Autocomplete opens only with Ctrl+Space (the default trigger): after typing "no", Enter makes a new line.
    key(SDLK_END, SDL_KMOD_LCTRL);
    type("\nno");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "shape(1).out()\nshape(2).out()\nnoise().out()\nno\n");
    key(SDLK_BACKSPACE, 0);
    key(SDLK_SPACE, SDL_KMOD_LCTRL); // opens the list
    type("i"); // the open list follows the typed word
    key(SDLK_RETURN, 0); // Enter inserts "noise"
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");

    // Two cursors type into two lines.
    key(SDLK_HOME, SDL_KMOD_LCTRL);
    key(SDLK_DOWN, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("x");
    key(SDLK_ESCAPE, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "xshape(1).out()\nxshape(2).out()\nnoise().out()\nnoise");

    // A two-key command: Ctrl+K, then Ctrl+U makes the word upper case.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("noise");
    key(SDLK_K, SDL_KMOD_LCTRL);
    key(SDLK_LCTRL, SDL_KMOD_LCTRL); // a modifier alone keeps the pending command
    key(SDLK_U, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "NOISE");

    // Ctrl+Shift+G evaluates, asks for a name like the confirmation prompt of the web editor, then shares.
    key(SDLK_G, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("my name");
    key(SDLK_RETURN, 0);
    EXPECT_STRING(shared_name, "my name");

    // The "keybindings" of the file: Ctrl+Alt+J joins lines, and Ctrl+J (its default key) does nothing now.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("osc()\n.out()");
    key(SDLK_HOME, SDL_KMOD_LCTRL);
    key(SDLK_J, SDL_KMOD_LCTRL);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "osc()\n.out()");
    key(SDLK_J, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    key(SDLK_RETURN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    EXPECT_STRING(evaluated, "osc() .out()");

    // The settings menu saves at once: the next theme after "contrast" is "solarized"; the font size and the other
    // keys of the file stay.
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
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
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("autocomplete as a list or as ghost");
    key(SDLK_RETURN, 0);
    settings_defaults(&loaded);
    settings_load(&loaded, settings_file, error, sizeof error);
    EXPECT(loaded.autocomplete_style == AUTOCOMPLETE_GHOST);

    // Ghost text (the style is "ghost" now): Tab accepts the rest of the best candidate.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("n");
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");
    // Alt+] and Alt+[ cycle through the candidates "osc" and "out".
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("o");
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_RIGHTBRACKET, SDL_KMOD_LALT);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "out");
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("o");
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_RIGHTBRACKET, SDL_KMOD_LALT);
    key(SDLK_LEFTBRACKET, SDL_KMOD_LALT);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "osc");
    // Escape dismisses it, and Tab then indents.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("o");
    key(SDLK_SPACE, SDL_KMOD_LCTRL);
    key(SDLK_ESCAPE, 0);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_CONTAINS(evaluated, "o");
    EXPECT(strcmp(evaluated, "osc") != 0);
    // Without Ctrl+Space, typing shows nothing; with the trigger "while typing" it does.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("no");
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT(strcmp(evaluated, "noise") != 0);
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("autocomplete on Ctrl+Space or while");
    key(SDLK_RETURN, 0);
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("no");
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT_STRING(evaluated, "noise");
    // Another edit (a cursor motion) hides the suggestion.
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("no");
    key(SDLK_LEFT, 0);
    key(SDLK_TAB, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
    EXPECT(strcmp(evaluated, "noise") != 0);
    // The list style with the trigger "while typing": the list opens on its own, Enter inserts.
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("autocomplete as a list or as ghost");
    key(SDLK_RETURN, 0);
    key(SDLK_A, SDL_KMOD_LCTRL);
    type("no");
    key(SDLK_RETURN, 0);
    key(SDLK_RETURN, SDL_KMOD_LCTRL);
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

    // The gallery and patch bay server of the settings menu: a URL starts the patch bay at once, the panel shows
    // the stream id, Ctrl+Alt+I copies it, and an empty URL stops it again; the file keeps the choice.
    write_file(settings_file, "{}\n");
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("set the gallery and patch bay");
    key(SDLK_RETURN, 0);
    type(LONG_SERVER); // replaces the selected URL of the file loaded at the start
    key(SDLK_RETURN, 0);
    EXPECT_STRING(patch_bay_server, LONG_SERVER);
    settings_defaults(&loaded);
    settings_load(&loaded, settings_file, error, sizeof error);
    EXPECT_STRING(loaded.gallery_server, LONG_SERVER);
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "stream id " STREAM_ID);
    // The copy button wraps into the panel (800 px wide here), and a click on it copies and keeps the panel open.
    EXPECT(copy_x > 0 && copy_x < 760);
    SDL_SetClipboardText("");
    ui_mouse_button(copy_x + 5, copy_y + 5, true, 1, 0);
    ui_mouse_button(copy_x + 5, copy_y + 5, false, 1, 0);
    char *clipboard = SDL_GetClipboardText();
    EXPECT_STRING(clipboard, STREAM_ID);
    SDL_free(clipboard);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "stream id " STREAM_ID);
    key(SDLK_ESCAPE, 0);
    key(SDLK_I, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    ui_draw(800, 600, 0);
    EXPECT_CONTAINS(drawn, "copied the stream id stream456");
    key(SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    type("set the gallery and patch bay");
    key(SDLK_RETURN, 0);
    key(SDLK_BACKSPACE, 0); // the prompt starts with the URL, selected
    key(SDLK_RETURN, 0);
    EXPECT_STRING(patch_bay_server, "");
    key(SDLK_COMMA, SDL_KMOD_LCTRL);
    ui_draw(800, 600, 0);
    EXPECT(strstr(drawn, "stream id") == NULL);
    key(SDLK_ESCAPE, 0);

    test_signatures();
    test_overlay_settings(settings_file);
    test_number_fields(settings_file);
    test_settings_rows(settings_file);
    test_files(settings_file);
    test_setup_file(settings_file);
    test_unsaved_question(settings_file);
    test_recovery(settings_file);
    test_random_input(settings_file);

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
