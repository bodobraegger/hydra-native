#define _GNU_SOURCE
#include "ui.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#include "editor.h"
#include "keymap.h"
#include "overlay.h"
#include "signature.h"
#include "thumbnails.h"
#include "unicode.h"

#define MARGIN 12.0f
#define TEXT_PADDING 3.0f
#define CURSOR_WIDTH 2.0f
#define BAR_PADDING 4.0f
#define FLASH_MS 300.0 // like flashCode() of the web editor
#define INFO_MESSAGE_MS 2000.0
#define RECOVERY_DELAY_MS 2000.0 // after the last edit
#define CURSOR_BLINK_MS 530.0
#define WHEEL_ROWS 3
#define FONT_SIZE_STEP 2.0f
#define MAX_MESSAGE 512
#define MAX_PROMPT 256
#define MAX_COMPLETIONS 24
#define MAX_NAME_COMPLETIONS 12 // the list of names and words; typed lists (textures) may be longer
#define MAX_SIGNATURES 512
#define MAX_SNIPPET 512
#define OPACITY_STEP 10 // percent, for the - and + of the opacities in the settings menu
#define GAIN_STEP 10 // percent, for the - and + of "input gain"
#define MIN_COMPLETION_PREFIX 2
#define MAX_PALETTE_ROWS 12
#define MAX_ROWS_PER_LINE 1024
// The box of the autocomplete list and the signature help draws with blend mode difference over the hydra output
// (blue over black), then a dark scrim keeps the white text readable over bright visuals; the same in every theme.
// The setting "autocompleteOpacity" scales the alpha of both.
#define COMPLETION_BOX_RGB 0x0000FF00u
#define COMPLETION_SCRIM_ALPHA 0xA0u
#define COMPLETION_SELECTED_COLOR 0x6060FF80u
#define COMPLETION_TEXT_COLOR 0xFFFFFFFFu

// A color theme of the settings menu; colors are RGBA. The code floats over the visuals, so backgrounds are
// translucent in the dark themes.
struct theme {
    const char *name;
    uint32_t line_background, selection, flash, bracket, cursor, text, keyword, number, string, comment, function;
    uint32_t bar_background, error, info, toolbar, popup_background, popup_selected, help_background, help_keys;
    uint32_t bookmark, match, match_active, match_text, ghost;
    uint32_t brackets[3]; // bracket pair colors by nesting depth, like VS Code
    uint32_t whitespace; // the dots and arrows of rendered whitespace
};

static const struct theme builtin_themes[] = {
    { "hydra", 0x000000B8u, 0x3D6FD6C0u, 0xFFFFFF70u, 0xFFFFFF55u, 0xFFFFFFFFu, 0xF2F2F2FFu, 0xFF7AC6FFu, 0xFFB86CFFu,
      0xB5E655FFu, 0xA0A0A0FFu, 0x6FDCF2FFu, 0x000000D8u, 0xFF6B6BFFu, 0xE6E6E6FFu, 0x000000B8u, 0x101010F0u,
      0x3D6FD6FFu, 0x000000E0u, 0x6FDCF2FFu, 0x6FDCF2FFu, 0xFFFF00FFu, 0xFF9632FFu, 0x000000FFu, 0xF2F2F280u,
      { 0xFFD700FFu, 0xDA70D6FFu, 0x179FFFFFu }, 0xFFFFFF48u },
    { "contrast", 0x000000FFu, 0x0050FFFFu, 0xFFFFFFA0u, 0xFFFFFF90u, 0xFFFF00FFu, 0xFFFFFFFFu, 0xFF5FFFFFu,
      0xFFAF00FFu,
      0x5FFF5FFFu, 0xBCBCBCFFu, 0x5FFFFFFFu, 0x000000FFu, 0xFF3030FFu, 0xFFFFFFFFu, 0x000000FFu, 0x000000FFu,
      0x0050FFFFu, 0x000000F0u, 0x5FFFFFFFu, 0x5FFFFFFFu, 0xFFFF00FFu, 0xFF9632FFu, 0x000000FFu, 0xFFFFFF90u,
      { 0xFFFF00FFu, 0xFF5FFFFFu, 0x5FAFFFFFu }, 0xFFFFFF70u },
    { "solarized", 0x002B36D0u, 0x268BD2A0u, 0xEEE8D570u, 0x93A1A160u, 0xEEE8D5FFu, 0x93A1A1FFu, 0x859900FFu,
      0xD33682FFu,
      0x2AA198FFu, 0x586E75FFu, 0x268BD2FFu, 0x002B36E8u, 0xDC322FFFu, 0xEEE8D5FFu, 0x073642D0u, 0x073642F0u,
      0x268BD2FFu, 0x002B36F0u, 0xB58900FFu, 0xB58900FFu, 0xB58900FFu, 0xCB4B16FFu, 0x002B36FFu, 0x93A1A180u,
      { 0xB58900FFu, 0x6C71C4FFu, 0x2AA198FFu }, 0x586E75A0u },
    { "paper", 0xFFFFFFD8u, 0xA8C7FAD0u, 0x00000040u, 0x00000040u, 0x000000FFu, 0x1F1F1FFFu, 0xA626A4FFu, 0x986801FFu,
      0x50A14FFFu, 0x8A8A8AFFu, 0x4078F2FFu, 0xFFFFFFE8u, 0xCA1243FFu, 0x1F1F1FFFu, 0xFFFFFFD8u, 0xF5F5F5F8u,
      0xA8C7FAFFu, 0xFFFFFFF0u, 0x4078F2FFu, 0x4078F2FFu, 0xFFFF00FFu, 0xFF9632FFu, 0x000000FFu, 0x1F1F1F70u,
      { 0x986801FFu, 0xA626A4FFu, 0x4078F2FFu }, 0x00000038u },
};
#define BUILTIN_THEME_COUNT (sizeof builtin_themes / sizeof builtin_themes[0])

// The built-in themes, then the "themes" of the settings file.
static struct theme themes[BUILTIN_THEME_COUNT + MAX_CUSTOM_THEMES];
static size_t theme_count;
static const struct theme *theme = &builtin_themes[0];

static const struct theme *find_theme(const char *name) {
    for (size_t i = 0; i < theme_count; i++)
        if (!strcmp(themes[i].name, name)) return &themes[i];
    return NULL;
}

// The color names of a custom theme in the settings file.
static const struct {
    const char *name;
    size_t offset;
} theme_color_names[] = {
#define THEME_COLOR(json, field) { json, offsetof(struct theme, field) }
    THEME_COLOR("lineBackground", line_background), THEME_COLOR("selection", selection), THEME_COLOR("flash", flash),
    THEME_COLOR("bracket", bracket), THEME_COLOR("cursor", cursor), THEME_COLOR("text", text),
    THEME_COLOR("keyword", keyword),
    THEME_COLOR("number", number), THEME_COLOR("string", string), THEME_COLOR("comment", comment),
    THEME_COLOR("function", function), THEME_COLOR("barBackground", bar_background), THEME_COLOR("error", error),
    THEME_COLOR("info", info), THEME_COLOR("toolbar", toolbar), THEME_COLOR("popupBackground", popup_background),
    THEME_COLOR("popupSelected", popup_selected), THEME_COLOR("helpBackground", help_background),
    THEME_COLOR("helpKeys", help_keys), THEME_COLOR("bookmark", bookmark), THEME_COLOR("match", match),
    THEME_COLOR("matchActive", match_active), THEME_COLOR("matchText", match_text), THEME_COLOR("ghost", ghost),
    THEME_COLOR("whitespace", whitespace),
#undef THEME_COLOR
};

// "#rrggbb" or "#rrggbbaa" as RGBA; false for other text.
static bool parse_color(const char *text, uint32_t *color) {
    size_t length = strlen(text);
    if (text[0] != '#' || (length != 7 && length != 9) || strspn(text + 1, "0123456789abcdefABCDEF") != length - 1)
        return false;
    *color = (uint32_t)strtoul(text + 1, NULL, 16);
    if (length == 7) *color = *color << 8 | 0xFF;
    return true;
}

// Builds the list of themes; problems go into `error` (the first one).
static void load_themes(const struct settings *settings, char *error, size_t size) {
    memcpy(themes, builtin_themes, sizeof builtin_themes);
    theme_count = BUILTIN_THEME_COUNT;
    for (size_t i = 0; i < settings->theme_count; i++) {
        const struct custom_theme *custom = &settings->themes[i];
        const struct theme *base = custom->base[0] ? find_theme(custom->base) : &builtin_themes[0];
        struct theme *built = &themes[theme_count];
        *built = base ? *base : builtin_themes[0];
        built->name = custom->name;
        size_t bracket = 0;
        for (size_t k = 0; k < custom->color_count; k++) {
            const struct settings_pair *pair = &custom->colors[k];
            uint32_t color, *target = NULL;
            if (!strcmp(pair->key, "brackets")) target = bracket < 3 ? &built->brackets[bracket++] : NULL;
            for (size_t n = 0; n < sizeof theme_color_names / sizeof theme_color_names[0] && !target; n++)
                if (!strcmp(theme_color_names[n].name, pair->key))
                    target = (uint32_t *)((char *)built + theme_color_names[n].offset);
            if (target && parse_color(pair->value, &color)) *target = color;
            else if (!error[0])
                snprintf(error, size, "theme %s: %s \"%s\" is no known color", custom->name, pair->key, pair->value);
        }
        if (!find_theme(custom->name)) theme_count++;
        else if (!error[0]) snprintf(error, size, "theme %s: the name is taken", custom->name);
    }
}

// The text of clear() in the web editor.
#define CLEARED_CODE "\n \n // Type some code on a new line (such as \"osc().out()\"), and press CTRL+shift+enter"

static const char *const keywords[] = {
    "const", "let", "var", "function", "return", "if", "else", "for", "while", "do", "new", "true", "false",
    "null", "undefined", "this", "of", "in", "typeof", "async", "await", "class", "break", "continue", "switch",
    "case", "default", "try", "catch", "throw",
};

enum prompt_kind {
    PROMPT_NONE,
    PROMPT_FIND,
    PROMPT_REPLACE,
    PROMPT_GO_TO_LINE,
    PROMPT_PALETTE,
    PROMPT_SHARE,
    PROMPT_SERVER,
    PROMPT_FONT_PATH,
    PROMPT_L10N_URL,
    PROMPT_UNSAVED, // the question before an action loses the unsaved changes of the open file
};

// The server of the deployed web editor (VITE_SERVER_URL of hydra.ojack.xyz) for gallery and patch bay.
#define WEB_EDITOR_SERVER "https://api.hydrasynth.xyz"

// The prompt() text of showConfirmation() in gallery-store.js of the web editor.
static const char *const share_confirmation[] = {
    "HYDRA SKETCH GALLERY",
    "///////////////////////////////////////",
    "\xF0\x9F\x8E\xA8 Add your sketch to the gallery of hydra sketches at https://botsin.space/@hydra.",
    "\xE2\x9C\x8D\xEF\xB8\x8F Type your name, mastodon handle, or a short description below, and then press 'OK' to "
        "share.",
    "\xE2\x80\xBC\xEF\xB8\x8F Make sure you are ready to share - there is no undo button!",
    "\xF0\x9F\x92\x96 Thank you for sharing! You are also warmly invited to join the the live coding server on the "
    "fediverse at https://social.toplap.org/.",
};

static struct ui_host host;
static struct settings settings;
static char *settings_file;
static struct editor editor;
static char *file_path;
static void show_title(const char *url);
static bool visible = true;
static bool wrap = true; // the web editor wraps lines (CodeMirror lineWrapping)
static size_t scroll_row, scroll_column; // scroll_column only without wrap
static int last_width, last_height;
static size_t flash_start, flash_end;
static double flash_until, message_until, now, last_activity;
static char message[MAX_MESSAGE];
static bool message_is_error;
static bool evaluating;
static char console_message[MAX_MESSAGE];
static bool suppress_text; // the key press before this text input was a command
static bool help_visible;
static bool info_visible;
static bool extensions_visible;
static bool settings_visible;
static bool gallery_enabled; // the web editor shows "share" only with a gallery server

// The drop-down list of the toolbar button "reset", which shows with an open file: the entry, its tooltip and its
// command.
static const struct {
    const char *label, *title;
    enum command command;
} reset_menu[] = {
    { "reset", "load the saved file again and run it", COMMAND_CLEAR_ALL },
    { "clear", "close the file and empty the code", COMMAND_CLEAR_AND_CLOSE },
};
#define RESET_MENU_COUNT (sizeof reset_menu / sizeof reset_menu[0])
#define RESET_BUTTON_TITLE "reset to the saved file, or clear"
static bool reset_menu_open;
static size_t reset_menu_selected;
#define MAX_PANEL_BUTTONS 256
#define EXTENSION_ROWS 3

// What a click on a button of the extensions panel or the settings menu does.
enum panel_action {
    ACTION_EXTENSION_CATEGORY, // the argument is the index of the category
    ACTION_EXTENSION_ADD, // the index of the extension, also for the next two
    ACTION_EXTENSION_DOCS,
    ACTION_EXTENSION_EXAMPLE, // `second` is the index of the example
    ACTION_OPEN_FILE,
    ACTION_WATCH, // 0 or 1, as for the other switches
    ACTION_THEME, // the index of the theme
    ACTION_AUTOCOMPLETE_STYLE, // the value of the enum of the setting, as for the other choices
    ACTION_AUTOCOMPLETE_TRIGGER,
    ACTION_SIGNATURE_HELP,
    ACTION_EDIT_NUMBER, // the number_kind that becomes a text field
    ACTION_STEP_NUMBER, // the number_kind; `second` is the change
    ACTION_BACKGROUND_BLEND,
    ACTION_TEXT_BLEND,
    ACTION_CODE_AT_START,
    ACTION_WHITESPACE,
    ACTION_WRAP,
    ACTION_SERVER_OFF,
    ACTION_SERVER_WEB_EDITOR,
    ACTION_SERVER_CUSTOM,
    ACTION_COPY_STREAM_ID,
    ACTION_FULLSCREEN,
    ACTION_VSYNC,
    ACTION_AUDIO_DEFAULT,
    ACTION_AUDIO_NONE,
    ACTION_AUDIO, // the index of the audio input
    ACTION_LANGUAGE, // the index of the language
    ACTION_L10N_URL,
    ACTION_FONT_FILE,
    ACTION_FONT, // the index of the font
};

// Clickable areas of the extensions and settings panels.
static struct panel_button {
    float x, y, width, height;
    enum panel_action action;
    int argument, second;
} panel_buttons[MAX_PANEL_BUTTONS];
static size_t panel_button_count;

// The index of the first panel button at (x, y), or panel_button_count.
static size_t panel_button_at(float x, float y) {
    size_t i = 0;
    while (i < panel_button_count && (x < panel_buttons[i].x || x > panel_buttons[i].x + panel_buttons[i].width ||
                                      y < panel_buttons[i].y || y > panel_buttons[i].y + panel_buttons[i].height))
        i++;
    return i;
}
static size_t info_scroll;
static float pointer_x, pointer_y; // the last mouse position, for toolbar tooltips

#define MAX_LANGUAGES 16
#define INFO_COLUMNS 72
static struct {
    char code[8];
    float x, width;
} language_buttons[MAX_LANGUAGES];
static size_t language_button_count;
static SDL_Keycode chord_prefix; // the first key of a two-key command, or 0
static char composition[64]; // the text of an IME composition, shown at the cursor
static uint32_t *colors;
static size_t colors_capacity;

static struct {
    enum prompt_kind kind;
    char query[MAX_PROMPT];
    char replacement[MAX_PROMPT];
    int field; // 0 query, 1 replacement
    bool query_selected; // the next typed text replaces the query, as in the find bar of the browser
    size_t palette_index;
} prompt;
static char last_find_query[MAX_PROMPT];
static struct editor_range *matches; // all matches of the find query while find or replace is open
static size_t match_count, active_match; // active_match is match_count when no match is selected

static struct {
    bool open;
    char *items[MAX_COMPLETIONS];
    char *inserts[MAX_COMPLETIONS]; // a snippet such as "modulate(${o0}, ${0.1})", or NULL to insert the item
    size_t count;
    size_t limit;
    size_t selected;
    size_t prefix_length;
} completion;

// The hydra functions of the running hydra-synth (also those of setFunction), loaded again after each evaluation.
static struct signature signatures[MAX_SIGNATURES];
static size_t signature_count;
static bool signatures_stale = true;
static bool signature_help_open;

// The tab stops of an inserted call: Tab and Shift+Tab select its arguments until the cursor leaves the current one.
static struct {
    bool active;
    size_t starts[MAX_SIGNATURE_INPUTS], ends[MAX_SIGNATURE_INPUTS];
    size_t count, current;
    size_t exit; // the end of the inserted text, where the last Tab goes
    unsigned generation; // counts insertions, so that the edit of an insertion does not move its own stops
} snippet;

static struct {
    bool dragging;
    bool column; // Alt+drag selects a rectangle
    size_t anchor_line, anchor_column;
} mouse;

// Visual rows: the first row of each line, recomputed when the text or the width changes.
static struct {
    size_t *first_row;
    size_t line_count;
    size_t total_rows;
    unsigned revision;
    size_t wrap_columns;
    bool valid;
} layout;

// ---------------------------------------------------------------- layout

static float bar_height(void) { return overlay_line_height() + 2 * BAR_PADDING; }

static size_t visible_rows(void) {
    float height = (float)last_height - 2 * MARGIN - bar_height();
    return height > overlay_line_height() ? (size_t)(height / overlay_line_height()) : 1;
}

static size_t visible_columns(void) {
    float width = (float)last_width - 2 * MARGIN;
    return width > overlay_char_width() ? (size_t)(width / overlay_char_width()) : 1;
}

static void update_layout(void) {
    editor.wrap_columns = wrap ? visible_columns() : 0;
    if (layout.valid && layout.revision == editor.revision && layout.wrap_columns == editor.wrap_columns &&
        layout.line_count == editor.line_count)
        return;
    layout.first_row = realloc(layout.first_row, (editor.line_count + 1) * sizeof *layout.first_row);
    size_t rows = 0;
    for (size_t line = 0; line < editor.line_count; line++) {
        layout.first_row[line] = rows;
        rows += editor_wrap_rows(&editor, line, NULL, 0);
    }
    layout.first_row[editor.line_count] = rows;
    layout.total_rows = rows;
    layout.line_count = editor.line_count;
    layout.revision = editor.revision;
    layout.wrap_columns = editor.wrap_columns;
    layout.valid = true;
}

static size_t line_of_row(size_t row) {
    size_t low = 0, high = layout.line_count - 1;
    while (low < high) {
        size_t middle = (low + high + 1) / 2;
        if (layout.first_row[middle] <= row) low = middle;
        else high = middle - 1;
    }
    return low;
}

static size_t global_row(size_t offset, size_t *x) {
    size_t row;
    editor_caret_position(&editor, offset, &row, x);
    return layout.first_row[editor_line_of(&editor, offset)] + row;
}

static void reveal_cursor(void) {
    update_layout();
    size_t x, row = global_row(editor.cursor, &x);
    size_t rows = visible_rows(), columns = visible_columns();
    if (row < scroll_row) scroll_row = row;
    if (row >= scroll_row + rows) scroll_row = row - rows + 1;
    if (wrap) scroll_column = 0;
    else {
        if (x < scroll_column) scroll_column = x;
        if (x + 1 >= scroll_column + columns) scroll_column = x + 2 - columns;
    }
    last_activity = now;
}

static void point_to_position(float x, float y, size_t *line, size_t *row, size_t *column) {
    update_layout();
    float row_position = (y - MARGIN) / overlay_line_height();
    float column_position = (x - MARGIN) / overlay_char_width() + 0.5f;
    size_t global = scroll_row + (row_position > 0 ? (size_t)row_position : 0);
    if (global >= layout.total_rows) global = layout.total_rows - 1;
    *line = line_of_row(global);
    *row = global - layout.first_row[*line];
    *column = scroll_column + (column_position > 0 ? (size_t)column_position : 0);
}

static size_t offset_at_point(float x, float y) {
    size_t line, row, column;
    point_to_position(x, y, &line, &row, &column);
    return editor_offset_at_visual(&editor, line, row, column);
}

// ---------------------------------------------------------------- messages

void ui_show_message(const char *text, bool error) {
    // The bar has one line: the first line of the text.
    size_t length = strcspn(text, "\n");
    if (length >= MAX_MESSAGE) length = MAX_MESSAGE - 1;
    memcpy(message, text, length);
    message[length] = '\0';
    message_is_error = error;
    message_until = error ? 0 : now + INFO_MESSAGE_MS;
}

void ui_console_message(const char *text) {
    if (evaluating && !console_message[0]) snprintf(console_message, sizeof console_message, "%s", text);
}

// ---------------------------------------------------------------- evaluation and files

// Evaluates code and returns NULL, or the error, or the first console warning or error of the evaluation
// (hydra reports shader compile errors with console.warn). The result is malloc'd.
static char *evaluate_code(const char *code, size_t length, const char *name) {
    evaluating = true;
    console_message[0] = '\0';
    char *error = host.evaluate(code, length, name);
    evaluating = false;
    signatures_stale = true; // the code may have called setFunction
    if (!error && console_message[0]) error = strdup(console_message);
    return error;
}

static void show_result(const char *prefix, char *error) {
    if (error) {
        char text[MAX_MESSAGE];
        snprintf(text, sizeof text, "%s%s", prefix, error);
        ui_show_message(text, true);
    } else if (message_is_error) {
        message[0] = '\0';
    }
}

static bool evaluate_range(size_t start, size_t end) {
    flash_start = start;
    flash_end = end;
    flash_until = now + FLASH_MS;
    if (start == end) return true;
    char *code = strndup(editor.text + start, end - start);
    char *error = evaluate_code(code, end - start, "<editor>");
    show_result("", error);
    bool ok = !error;
    free(error);
    free(code);
    return ok;
}

// Like 'gallery: save to URL' of the web editor, which puts the code into the address bar; here the window title
// shows the URL.
static void update_url(void) {
    char *url = host.call("__urlForCode", editor.text);
    if (url) show_title(url);
    free(url);
}

// Evaluates all code and updates the URL after a run without errors, as the web editor does.
static void evaluate_all(void) {
    if (evaluate_range(0, editor.length)) update_url();
}

char *ui_evaluate_socket(const char *code, size_t length) {
    char *error = evaluate_code(code, length, "<socket>");
    show_result("socket: ", error);
    return error;
}

static char *read_whole_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 65536, used = 0, count;
    char *text = malloc(capacity);
    while ((count = fread(text + used, 1, capacity - used, file)) > 0) {
        // The editor and the host functions take the code as a C string, so a NUL byte of a file does not enter.
        for (size_t i = used, end = used + count; i < end; i++)
            if (text[i]) text[used++] = text[i];
        if (used == capacity) text = realloc(text, capacity *= 2);
    }
    fclose(file);
    *length = used;
    return text;
}

// The name of a file without its folder; NULL for NULL.
static const char *base_name(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

// The window title: the name of the open file and the hydra.ojack.xyz URL of the code, as the tab and the address
// bar of the browser show them. `url` NULL keeps the last URL.
static void show_title(const char *url) {
    static char last_url[8192];
    if (url) snprintf(last_url, sizeof last_url, "%s", url);
    const char *name = base_name(file_path);
    char title[sizeof last_url + 512];
    if (name && last_url[0]) snprintf(title, sizeof title, "%s - %s", name, last_url);
    else snprintf(title, sizeof title, "%s", name ? name : last_url[0] ? last_url : "hydra-native");
    host.set_title(title);
}

// The recovery file (src/settings.h) holds the unsaved code for a start after a crash. It follows the edits after
// RECOVERY_DELAY_MS; saving, discarding and quitting remove it.
static struct {
    char path[PATH_MAX]; // "" without a settings folder
    unsigned revision; // the editor revision that the file, or its absence, shows
    unsigned seen_revision;
    double edited; // when seen_revision changed
    bool restored; // this start shows its code
    bool kept; // it holds the changes of another file, and this run leaves it as it is
} recovery;

// The recovery names the file with its absolute path, so that a start in another folder finds it. A file that does
// not exist yet keeps its path as it is.
static char *absolute_path(const char *path) {
    char *resolved = realpath(path, NULL);
    return resolved ? resolved : strdup(path);
}

static bool same_file(const char *first, const char *second) {
    char *first_path = absolute_path(first), *second_path = absolute_path(second);
    bool same = !strcmp(first_path, second_path);
    free(first_path);
    free(second_path);
    return same;
}

static void remove_recovery(void) {
    if (recovery.path[0] && !recovery.kept) unlink(recovery.path);
    recovery.revision = recovery.seen_revision = editor.revision;
}

// Each frame: writes the unsaved code when the edits pause, or removes the file when nothing is unsaved.
static void update_recovery(void) {
    if (editor.revision != recovery.seen_revision) {
        recovery.seen_revision = editor.revision;
        recovery.edited = now;
    }
    if (!recovery.path[0] || recovery.kept || editor.revision == recovery.revision ||
        now - recovery.edited < RECOVERY_DELAY_MS)
        return;
    if (!editor.modified) return remove_recovery();
    recovery.revision = editor.revision;
    char *file = absolute_path(file_path ? file_path : "");
    recovery_save(recovery.path, file, editor.text); // after a failed write the edits go on without a recovery
    free(file);
}

static void keep_recovery(const char *file) {
    recovery.kept = true;
    char text[MAX_MESSAGE + sizeof recovery.path];
    snprintf(text, sizeof text, "%s keeps the unsaved changes of %s: start without a file to restore them",
             base_name(recovery.path), file[0] ? base_name(file) : "code without a file");
    ui_show_message(text, true);
}

// At the start: the recovered code replaces the code of `path` when it belongs to that file, or when no file is
// given. It comes with its file as the open file and counts as unsaved.
static void restore_recovery(const char *path) {
    char *file, *code = recovery_load(recovery.path, &file);
    if (!code) return;
    if (path && (!file[0] || !same_file(file, path))) {
        keep_recovery(file);
    } else {
        free(file_path);
        file_path = file[0] ? strdup(file) : NULL;
        editor_set_text(&editor, code, strlen(code));
        editor.modified = true;
        recovery.restored = true;
        char text[MAX_MESSAGE];
        snprintf(text, sizeof text, "restored the unsaved changes of %s", file[0] ? base_name(file) : "the code");
        ui_show_message(text, true);
        show_title(NULL);
    }
    free(file);
    free(code);
}

static void save(void) {
    if (!file_path) {
        host.choose_file(true); // the answer comes through ui_save_as()
        return;
    }
    char *temporary;
    if (asprintf(&temporary, "%s.hydra-native-save", file_path) < 0) return;
    FILE *file = fopen(temporary, "wb");
    bool ok = file && fwrite(editor.text, 1, editor.length, file) == editor.length;
    if (file && fclose(file) != 0) ok = false;
    if (ok && rename(temporary, file_path) != 0) ok = false;
    char text[MAX_MESSAGE];
    if (ok) {
        editor.modified = false;
        remove_recovery();
        snprintf(text, sizeof text, "saved %s", file_path);
    } else {
        snprintf(text, sizeof text, "cannot save %s: %s", file_path, strerror(errno));
        unlink(temporary);
    }
    ui_show_message(text, !ok);
    free(temporary);
}

// The file replaces the code (Ctrl+Z brings the old code back), runs, and becomes the open file.
static void open_file(const char *path) {
    size_t length;
    char *text = read_whole_file(path, &length);
    if (!text) {
        char message[MAX_MESSAGE];
        snprintf(message, sizeof message, "cannot open %s: %s", path, strerror(errno));
        return ui_show_message(message, true);
    }
    free(file_path);
    file_path = strdup(path);
    editor_replace_text(&editor, text, length);
    editor_set_cursor(&editor, 0, false);
    editor.modified = false;
    free(text);
    evaluate_all();
    show_title(NULL);
}

// The answer of the save dialog of Ctrl+S without an open file: the code goes there, and it becomes the open file.
void ui_save_as(const char *path) {
    free(file_path);
    file_path = strdup(path);
    save();
    show_title(NULL);
}

const char *ui_file_path(void) { return file_path; }

void ui_file_changed(void) {
    if (!file_path) return;
    size_t length;
    char *text = read_whole_file(file_path, &length);
    if (!text) return;
    bool same = length == editor.length && memcmp(text, editor.text, length) == 0;
    if (!same && editor.modified) {
        ui_show_message("the file changed on disk; the unsaved edits stay (Ctrl+S overwrites the file)", true);
    } else if (!same) {
        editor_set_text(&editor, text, length);
        evaluate_range(0, editor.length);
    }
    free(text);
}

// ---------------------------------------------------------------- clipboard

static void copy_selection(bool cut) {
    if (editor_selection_count(&editor) == 1 && !editor_has_selection(&editor)) {
        // Like Pulsar: without a selection, copy and cut take the whole line.
        size_t line = editor_line_of(&editor, editor.cursor);
        editor_set_cursor(&editor, editor_line_start(&editor, line), false);
        size_t end = line + 1 < editor.line_count ? editor_line_start(&editor, line + 1) : editor.length;
        editor_set_cursor(&editor, end, true);
    }
    size_t length;
    char *text = editor_selected_text(&editor, &length);
    SDL_SetClipboardText(text);
    free(text);
    if (cut) editor_delete_selection(&editor);
    else editor_move(&editor, MOTION_RIGHT, false);
}

static void paste(void) {
    char *text = SDL_GetClipboardText();
    if (!text) return;
    size_t used = 0;
    for (size_t i = 0; text[i]; i++)
        if (text[i] != '\r') text[used++] = text[i];
    if (used) editor_insert(&editor, text, used, EDIT_OTHER);
    SDL_free(text);
}

// ---------------------------------------------------------------- signatures, snippets and autocomplete

static void load_signatures(void) {
    if (!signatures_stale) return;
    signatures_stale = false;
    char *text = host.call("__functionSignatures", "");
    signature_count = text ? signatures_parse(text, signatures, MAX_SIGNATURES) : 0;
    free(text);
}

static const struct signature *call_at(size_t offset, struct call_context *call) {
    load_signatures();
    const struct signature *found;
    return signature_call_at(editor.text, offset, signatures, signature_count, call, &found) ? found : NULL;
}

static void select_tab_stop(size_t index) {
    snippet.current = index;
    editor_set_cursor(&editor, snippet.starts[index], false);
    editor_set_cursor(&editor, snippet.ends[index], true);
}

// Replaces the selection with `text`, whose "${value}" parts become tab stops with that value.
static void insert_snippet(const char *text) {
    char plain[MAX_SNIPPET];
    size_t length = 0, count = 0, starts[MAX_SIGNATURE_INPUTS], ends[MAX_SIGNATURE_INPUTS];
    for (const char *c = text; *c && length + 1 < sizeof plain; c++) {
        if (c[0] == '$' && c[1] == '{' && count < MAX_SIGNATURE_INPUTS) {
            starts[count] = length;
            for (c += 2; *c && *c != '}' && length + 1 < sizeof plain; c++) plain[length++] = *c;
            ends[count++] = length;
            if (!*c) break;
        } else {
            plain[length++] = *c;
        }
    }
    size_t start, end;
    editor_selection(&editor, &start, &end);
    editor_insert(&editor, plain, length, EDIT_OTHER);
    snippet.generation++;
    snippet.active = count > 0;
    snippet.count = count;
    snippet.exit = start + length;
    for (size_t i = 0; i < count; i++) {
        snippet.starts[i] = start + starts[i];
        snippet.ends[i] = start + ends[i];
    }
    if (count) select_tab_stop(0);
}

// After an edit inside the current tab stop, the stop grows or shrinks and the later ones move. The snippet ends
// when the cursor leaves the current stop.
static void follow_snippet(size_t old_length) {
    if (!snippet.active) return;
    size_t grown = editor.length - old_length, *current_end = &snippet.ends[snippet.current]; // wraps when it shrinks
    *current_end += grown;
    for (size_t i = snippet.current + 1; i < snippet.count; i++) {
        snippet.starts[i] += grown;
        snippet.ends[i] += grown;
    }
    snippet.exit += grown;
    if (editor_selection_count(&editor) > 1 || editor.cursor < snippet.starts[snippet.current] ||
        editor.cursor > *current_end || snippet.exit > editor.length)
        snippet.active = false;
}

static void next_tab_stop(int direction) {
    if (direction > 0 && snippet.current + 1 >= snippet.count) {
        snippet.active = false;
        editor_set_cursor(&editor, snippet.exit, false);
    } else if (direction > 0) {
        select_tab_stop(snippet.current + 1);
    } else if (snippet.current > 0) {
        select_tab_stop(snippet.current - 1);
    }
}

static void close_completion(void) {
    for (size_t i = 0; i < completion.count; i++) {
        free(completion.items[i]);
        free(completion.inserts[i]);
    }
    completion.count = 0;
    completion.open = false;
}

static bool add_completion(const char *word, size_t length, const char *insert) {
    for (size_t i = 0; i < completion.count; i++)
        if (strlen(completion.items[i]) == length && !strncmp(completion.items[i], word, length)) return true;
    if (completion.count == completion.limit) return false;
    completion.inserts[completion.count] = insert ? strdup(insert) : NULL;
    completion.items[completion.count++] = strndup(word, length);
    return true;
}

static bool starts_with(const char *text, const char *prefix, size_t length) {
    return strlen(text) >= length && !strncmp(text, prefix, length);
}

// A known function becomes its call with the defaults as tab stops, unless a "(" follows already.
static void add_function_completion(const char *name, size_t length) {
    char insert[MAX_SNIPPET];
    const struct signature *signature = signature_find(signatures, signature_count, name, length);
    bool call_follows = editor.cursor < editor.length && editor.text[editor.cursor] == '(';
    if (signature && !call_follows) signature_snippet(signature, insert, sizeof insert);
    add_completion(name, length, signature && !call_follows ? insert : NULL);
}

static uint32_t completion_color(uint32_t rgb, uint32_t alpha) {
    return rgb | (alpha * (uint32_t)settings.completion_opacity + 50) / 100;
}

static const char *const OUTPUT_NAMES[] = { "o0", "o1", "o2", "o3" };
static const char *const SOURCE_NAMES[] = { "s0", "s1", "s2", "s3" };

static bool is_hydra_name(const char *word) {
    for (size_t i = 0; i < 4; i++)
        if (!strcmp(word, OUTPUT_NAMES[i]) || !strcmp(word, SOURCE_NAMES[i])) return true;
    return signature_find(signatures, signature_count, word, strlen(word)) != NULL;
}

// The identifiers of the file that start with `prefix`, without the hydra functions and outputs, whose places the
// typed completions and __completionNames know.
static void add_file_words(const char *prefix, size_t length) {
    char **words;
    size_t count = editor_words(&editor, prefix, length, &words);
    for (size_t i = 0; i < count; i++) {
        if (!is_hydra_name(words[i])) add_completion(words[i], strlen(words[i]), NULL);
        free(words[i]);
    }
    free(words);
}

static void add_names(const char *const *names, size_t count, const char *prefix, size_t length) {
    for (size_t i = 0; i < count; i++)
        if (starts_with(names[i], prefix, length)) add_completion(names[i], strlen(names[i]), NULL);
}

static void add_functions_of_kind(const char *kind, const char *prefix, size_t length) {
    for (size_t i = 0; i < signature_count; i++)
        if (!strcmp(signatures[i].kind, kind) && starts_with(signatures[i].name, prefix, length))
            add_function_completion(signatures[i].name, strlen(signatures[i].name));
}

// What stands before the "." at `dot`: a hydra chain (a call of a source or a transform), a source s0 to s3, the
// audio object a, or something else, such as Math.
enum receiver { RECEIVER_OTHER, RECEIVER_CHAIN, RECEIVER_SOURCE, RECEIVER_AUDIO };

static enum receiver receiver_before(size_t dot) {
    const char *text = editor.text;
    size_t end = dot;
    while (end > 0 && isspace((unsigned char)text[end - 1])) end--;
    if (end > 0 && text[end - 1] == ')') {
        size_t depth = 0, open = end;
        while (open > 0) {
            char c = text[--open];
            if (c == ')') depth++;
            else if (c == '(' && --depth == 0) break;
        }
        if (depth != 0) return RECEIVER_OTHER;
        size_t name_end = open;
        while (name_end > 0 && isspace((unsigned char)text[name_end - 1])) name_end--;
        size_t name_start = name_end;
        while (name_start > 0 && is_identifier_char((unsigned char)text[name_start - 1])) name_start--;
        const struct signature *called =
            signature_find(signatures, signature_count, text + name_start, name_end - name_start);
        return called &&
            (signature_is_source(called) || signature_is_chained(called)) ? RECEIVER_CHAIN : RECEIVER_OTHER;
    }
    size_t start = end;
    while (start > 0 && is_identifier_char((unsigned char)text[start - 1])) start--;
    if (end - start == 2 && text[start] == 's' && text[start + 1] >= '0' && text[start + 1] <= '3')
        return RECEIVER_SOURCE;
    if (end - start == 1 && text[start] == 'a') return RECEIVER_AUDIO;
    return RECEIVER_OTHER;
}

// After "Name." where Name is a dotted path to an object of the running JavaScript (Math, JSON, a user object such
// as midi): its properties from __propertyNames in js/main.js; a function goes in as a call with the cursor inside
// the parentheses. False when the runtime knows no such object.
static bool add_property_names(size_t dot, const char *prefix, size_t length) {
    size_t start = dot;
    while (start > 0 && (is_identifier_char((unsigned char)editor.text[start - 1]) || editor.text[start - 1] == '.'))
        start--;
    char *path = strndup(editor.text + start, dot - start);
    char *names = host.call("__propertyNames", path);
    free(path);
    bool found = names && names[0];
    bool call_follows = editor.cursor < editor.length && editor.text[editor.cursor] == '(';
    for (char *name = found ? strtok(names, "\n") : NULL; name; name = strtok(NULL, "\n")) {
        size_t name_length = strcspn(name, "(");
        if (name_length < length || strncmp(name, prefix, length) != 0) continue;
        char insert[MAX_SNIPPET];
        snprintf(insert, sizeof insert, "%.*s(${})", (int)name_length, name);
        add_completion(name, name_length, name[name_length] && !call_follows ? insert : NULL);
    }
    free(names);
    return found;
}

// Completions by type at the word that starts at `start`: after a ".", the methods of what stands before it; in
// an argument, what its type allows. False when the place has no type.
static bool open_typed_completion(const char *prefix, size_t length, size_t start) {
    if (start > 0 && editor.text[start - 1] == '.') {
        switch (receiver_before(start - 1)) {
        case RECEIVER_CHAIN:
            for (size_t i = 0; i < signature_count; i++)
                if (signature_is_chained(&signatures[i]) && starts_with(signatures[i].name, prefix, length))
                    add_function_completion(signatures[i].name, strlen(signatures[i].name));
            if (starts_with("out", prefix, length)) add_completion("out", 3, "out(${o0})");
            return true;
        case RECEIVER_SOURCE: add_functions_of_kind("source", prefix, length); return true;
        case RECEIVER_AUDIO: add_functions_of_kind("audio", prefix, length); return true;
        case RECEIVER_OTHER: return add_property_names(start - 1, prefix, length);
        }
    }
    struct call_context call;
    const struct signature *signature = call_at(start, &call);
    if (!signature || !call.direct || call.argument >= signature->input_count) return false;
    const struct signature_input *input = &signature->inputs[call.argument];
    bool glsl = signature_is_source(signature) || signature_is_chained(signature);
    if (!glsl) { // the functions of js/api-signatures.js, typed in TypeScript notation
        if (!strcmp(input->type, "HydraOutput") || !strcmp(input->type, "HydraSource")) {
            add_names(OUTPUT_NAMES, 4, prefix, length); // out(o1), render(o2)
            return true;
        }
        if (strcmp(input->type, "number") != 0) return true; // a string or an object: nothing to offer
    } else if (signature_input_is_texture(input)) {
        add_names(OUTPUT_NAMES, 4, prefix, length);
        add_names(SOURCE_NAMES, 4, prefix, length);
        if (!strcmp(input->type, "sampler2D")) return true; // src() takes an output or a source, not a chain
        if (starts_with("src", prefix, length)) add_completion("src", 3, "src(${o0})");
        for (size_t i = 0; i < signature_count; i++)
            if (signature_is_source(&signatures[i]) && starts_with(signatures[i].name, prefix, length))
                add_function_completion(signatures[i].name, strlen(signatures[i].name));
        return true;
    }
    if (input->default_value[0] && starts_with(input->default_value, prefix, length))
        add_completion(input->default_value, strlen(input->default_value), NULL);
    if (!length) {
        add_completion("() => ", 6, NULL);
        add_completion("[ ]", 3, "[${}]");
    }
    // A helper changes over time, so it goes into an arrow function unless one is open already.
    size_t before = start;
    while (before > 0 && isspace((unsigned char)editor.text[before - 1])) before--;
    bool in_arrow = before >= 2 && !strncmp(editor.text + before - 2, "=>", 2);
    char *helpers = host.call("__numberHelpers", "");
    for (char *helper = helpers ? strtok(helpers, "\n") : NULL; helper; helper = strtok(NULL, "\n")) {
        if (!starts_with(helper, prefix, length)) continue;
        char insert[MAX_SNIPPET];
        size_t helper_length = strlen(helper);
        bool indexed = helper_length >= 2 &&
            (!strcmp(helper + helper_length - 2, "[]") || !strcmp(helper + helper_length - 2, "()"));
        snprintf(insert, sizeof insert, "%s%.*s%s%s", in_arrow ? "" : "() => ",
                 (int)(indexed ? helper_length - 1 : helper_length),
                 helper, indexed ? "${0}" : "", indexed ? helper + helper_length - 1 : "");
        add_completion(helper, helper_length, insert);
    }
    free(helpers);
    add_file_words(prefix, length); // variables of the sketch, such as a speed or a MIDI value
    return true;
}

// Typed completions where the place has a type (a chosen function comes with its arguments); else plain hydra
// names first (from __completionNames in js/main.js), then the identifiers of the file.
static void open_completion(bool forced) {
    close_completion();
    size_t start, length = editor_word_before_cursor(&editor, &start);
    if (length < (forced ? 0 : MIN_COMPLETION_PREFIX) || editor_has_selection(&editor)) return;
    const char *prefix = editor.text + start;
    load_signatures();
    completion.limit = MAX_COMPLETIONS;
    if (!open_typed_completion(prefix, length, start)) {
        completion.limit = MAX_NAME_COMPLETIONS;
        // After an unknown object and its "." hydra names do not fit; at the start of a statement the transforms of
        // a chain do not either, because they need a chain before them.
        bool after_dot = start > 0 && editor.text[start - 1] == '.';
        size_t before = start;
        while (before > 0 && (editor.text[before - 1] == ' ' || editor.text[before - 1] == '\t')) before--;
        bool statement_start = before == 0 || strchr("\n;{}", editor.text[before - 1]) != NULL;
        char *names = after_dot ? NULL : host.call("__completionNames", "");
        for (char *name = names ? strtok(names, "\n") : NULL; name &&
             completion.count < completion.limit; name = strtok(NULL, "\n")) {
            const struct signature *named = signature_find(signatures, signature_count, name, strlen(name));
            if (statement_start && named && signature_is_chained(named)) continue;
            if (strlen(name) > length && !strncmp(name, prefix, length)) add_completion(name, strlen(name), NULL);
        }
        free(names);
        add_file_words(prefix, length);
    }
    completion.prefix_length = length;
    completion.selected = 0;
    completion.open = completion.count > 0;
}

static void accept_completion(void) {
    size_t start;
    editor_word_before_cursor(&editor, &start);
    editor_set_cursor(&editor, start, true);
    const char *insert = completion.inserts[completion.selected];
    if (insert) {
        insert_snippet(insert);
    } else {
        const char *item = completion.items[completion.selected];
        editor_insert(&editor, item, strlen(item), EDIT_OTHER);
    }
    close_completion();
}

// ---------------------------------------------------------------- prompts: find, replace, go to line, palette

static bool palette_matches(enum command command) {
    return command != COMMAND_NONE && strcasestr(command_description(command), prompt.query) != NULL;
}

static enum command palette_command(size_t index) {
    for (int command = 0; command < COMMAND_COUNT; command++)
        if (palette_matches((enum command)command) && index-- == 0) return (enum command)command;
    return COMMAND_NONE;
}

static void open_prompt(enum prompt_kind kind) {
    close_completion();
    if (kind == PROMPT_FIND || kind == PROMPT_REPLACE) {
        // Start with the selected text, else with the last query, selected so that typing replaces it.
        size_t start, end;
        editor_selection(&editor, &start, &end);
        snprintf(prompt.query, sizeof prompt.query, "%s", last_find_query);
        if (end > start && end - start < MAX_PROMPT && !memchr(editor.text + start, '\n', end - start))
            snprintf(prompt.query, sizeof prompt.query, "%.*s", (int)(end - start), editor.text + start);
        prompt.query_selected = prompt.query[0] != '\0';
        prompt.field = 0;
    } else {
        prompt.query[0] = '\0';
    }
    prompt.palette_index = 0;
    prompt.kind = kind;
}

static void set_server(const char *url);

static char *prompt_field(void) {
    return prompt.kind == PROMPT_REPLACE && prompt.field == 1 ? prompt.replacement : prompt.query;
}

static bool find_open(void) { return prompt.kind == PROMPT_FIND || prompt.kind == PROMPT_REPLACE; }

static void find(int direction) {
    if (!prompt.query[0]) return;
    snprintf(last_find_query, sizeof last_find_query, "%s", prompt.query);
    editor_find(&editor, prompt.query, strlen(prompt.query), true, direction);
}

// F3 and Shift+F3 open the find bar with the last query, as in the browser.
static void find_again(int direction) {
    if (!find_open()) {
        prompt.kind = PROMPT_FIND;
        prompt.field = 0;
        prompt.query_selected = false;
        snprintf(prompt.query, sizeof prompt.query, "%s", last_find_query);
    }
    find(direction);
}

static void update_matches(void) {
    free(matches);
    matches = NULL;
    match_count = 0;
    if (find_open() && prompt.query[0]) // ponytail: searches the whole text each frame, fine for sketch-sized code
        match_count = editor_find_all(&editor, prompt.query, strlen(prompt.query), true, &matches);
    size_t start, end;
    editor_selection(&editor, &start, &end);
    for (active_match = 0; active_match < match_count; active_match++)
        if (matches[active_match].start == start && matches[active_match].end == end) break;
}

// Searches again from the start of the selection, so that typing extends the current match.
static void find_incremental(void) {
    size_t start, end;
    editor_selection(&editor, &start, &end);
    editor_set_cursor(&editor, start, false);
    find(1);
}

static void prompt_changed(void) {
    if (prompt.kind == PROMPT_FIND || (prompt.kind == PROMPT_REPLACE && prompt.field == 0)) find_incremental();
    if (prompt.kind == PROMPT_PALETTE) prompt.palette_index = 0;
}

static void run_command(enum command command, bool extend);

static void save_settings(void);
static void set_font(const char *path);
static void load_settings_lists(void);

static void accept_prompt(int direction) {
    switch (prompt.kind) {
    case PROMPT_FIND: find(direction); break;
    case PROMPT_REPLACE:
        if (prompt.field == 0) find(direction);
        else
            editor_replace_next(&editor, prompt.query, strlen(prompt.query), prompt.replacement,
                                strlen(prompt.replacement), true);
        break;
    case PROMPT_GO_TO_LINE: {
        long line = atol(prompt.query);
        if (line > 0) editor_go_to_line(&editor, (size_t)line - 1);
        prompt.kind = PROMPT_NONE;
        break;
    }
    case PROMPT_SERVER:
        prompt.kind = PROMPT_NONE;
        set_server(prompt.query);
        break;
    case PROMPT_FONT_PATH:
        prompt.kind = PROMPT_NONE;
        set_font(prompt.query);
        break;
    case PROMPT_L10N_URL:
        prompt.kind = PROMPT_NONE;
        snprintf(settings.l10n_url, sizeof settings.l10n_url, "%s", prompt.query);
        save_settings();
        ui_show_message("translation URL: applies at the next start", false);
        break;
    case PROMPT_SHARE:
        prompt.kind = PROMPT_NONE;
        if (!host.share(editor.text, prompt.query))
            ui_show_message("no gallery server: choose one in the settings (Ctrl+,)", true);
        else ui_show_message("sharing with the gallery...", false);
        break;
    case PROMPT_UNSAVED: break; // it takes s, d and Escape only
    case PROMPT_PALETTE: {
        enum command command = palette_command(prompt.palette_index);
        prompt.kind = PROMPT_NONE;
        if (command != COMMAND_NONE) run_command(command, false);
        break;
    }
    case PROMPT_NONE: break;
    }
}

// ---------------------------------------------------------------- commands

// The gallery and patch bay server ("" is off): js/main.js starts or stops the patch bay at once, and "share"
// shows only with a server, as in the web editor.
static void set_server(const char *url) {
    snprintf(settings.gallery_server, sizeof settings.gallery_server, "%s", url);
    save_settings();
    host.set_server(url);
    free(host.call("__setPatchBayServer", ""));
    gallery_enabled = url[0] != '\0';
    char text[SETTINGS_TEXT + 64];
    if (url[0]) snprintf(text, sizeof text, "gallery and patch bay: %s", url);
    else snprintf(text, sizeof text, "gallery and patch bay: off");
    ui_show_message(text, false);
}

// The stream id of this instance in the patch bay room ("" without a connection): the socket id of the
// signalling, which other peers pass to s0.initStream and which pb.list() shows.
static char *patch_bay_stream_id(void) {
    char *id = host.call("__patchBayStreamId", "");
    if (!id) id = strdup("");
    return id;
}

static void copy_patch_bay_id(void) {
    char *stream_id = patch_bay_stream_id(), text[192];
    if (stream_id[0]) {
        SDL_SetClipboardText(stream_id);
        snprintf(text, sizeof text, "copied the stream id %s: s0.initStream('%s') elsewhere", stream_id, stream_id);
    } else {
        snprintf(text, sizeof text, "the patch bay is not connected (settings: server)");
    }
    ui_show_message(text, !stream_id[0]);
    free(stream_id);
}

static void save_settings(void) {
    char error[SETTINGS_TEXT];
    if (settings_file && !settings_save(&settings, settings_file, error, sizeof error))
        ui_show_message(error, true); // a file that does not parse stays as it is
}

static void change_font_size(float step) {
    if (overlay_set_font_size(overlay_font_size() + step)) {
        char text[64];
        snprintf(text, sizeof text, "font size %.0f px", overlay_font_size());
        ui_show_message(text, false);
        layout.valid = false;
        settings.font_size = overlay_font_size();
        save_settings();
    }
}

// The opacity of the code boxes in percent, from the setting or from the alpha of the theme.
static int code_background_percent(void) {
    int opacity = settings.code_background_opacity;
    return opacity == SETTINGS_THEME_OPACITY ? (int)((theme->line_background & 0xFFu) * 100 + 127) / 255 : opacity;
}

// The box behind a code line: the color of the theme, or white for blend mode difference (which inverts what
// is below), with the alpha of the theme or of "codeBackgroundOpacity".
static uint32_t code_background_color(void) {
    int opacity = settings.code_background_opacity;
    uint32_t alpha = opacity == SETTINGS_THEME_OPACITY ? theme->line_background & 0xFFu
        : (uint32_t)(opacity * 255 + 50) / 100;
    uint32_t rgb = settings.code_background_blend == CODE_BLEND_DIFFERENCE ? 0xFFFFFF00u
        : theme->line_background & 0xFFFFFF00u;
    return rgb | alpha;
}

// ---------------------------------------------------------------- number fields of the settings menu

// A click on a number of the settings menu turns it into a text field: Enter applies the typed value (clamped,
// with an optional unit), Escape cancels, Up and Down (and the wheel) step it, Shift by ten steps.
enum number_kind { NUMBER_OPACITY, NUMBER_FONT_SIZE, NUMBER_AUDIO_GAIN, NUMBER_COMPLETION_OPACITY };
static const struct {
    const char *unit; // accepted after the number, and shown after it
    double minimum, maximum;
    int button_step; // of the - and + buttons
} number_specs[] = {
    [NUMBER_OPACITY] = { "%", 0, 100, OPACITY_STEP },
    [NUMBER_FONT_SIZE] = { "px", OVERLAY_MIN_FONT_SIZE, OVERLAY_MAX_FONT_SIZE, (int)FONT_SIZE_STEP },
    [NUMBER_AUDIO_GAIN] = { "%", 0, SETTINGS_MAX_AUDIO_GAIN, GAIN_STEP },
    [NUMBER_COMPLETION_OPACITY] = { "%", 0, 100, OPACITY_STEP },
};
#define MAX_NUMBER_TEXT 16
#define LARGE_STEPS 10

static struct {
    bool open;
    enum number_kind kind;
    char text[MAX_NUMBER_TEXT];
    bool selected; // the whole text, so that typing replaces it
    bool invalid; // the last Enter found no number; it shows in the error color
} number_field;

static double number_value(enum number_kind kind) {
    switch (kind) {
    case NUMBER_OPACITY: return code_background_percent();
    case NUMBER_FONT_SIZE: return overlay_font_size();
    case NUMBER_AUDIO_GAIN: return settings.audio_gain;
    case NUMBER_COMPLETION_OPACITY: return settings.completion_opacity;
    }
    return 0;
}

static void format_number(enum number_kind kind, double value, char *text, size_t size) {
    snprintf(text, size, "%.0f %s", value, number_specs[kind].unit);
}

static void set_number(enum number_kind kind, double value) {
    if (value < number_specs[kind].minimum) value = number_specs[kind].minimum;
    if (value > number_specs[kind].maximum) value = number_specs[kind].maximum;
    switch (kind) {
    case NUMBER_FONT_SIZE: change_font_size((float)value - overlay_font_size()); return;
    case NUMBER_OPACITY: settings.code_background_opacity = (int)lround(value); break;
    case NUMBER_AUDIO_GAIN:
        settings.audio_gain = (int)lround(value);
        host.set_audio_gain(settings.audio_gain);
        break;
    case NUMBER_COMPLETION_OPACITY:
        settings.completion_opacity = (int)lround(value);
        break;
    }
    save_settings();
}

// A number with an optional unit ("45", "45 %", "20px"); false for anything else.
static bool parse_number(enum number_kind kind, const char *text, double *value) {
    char *end;
    *value = strtod(text, &end);
    if (end == text || !isfinite(*value)) return false;
    while (*end == ' ') end++;
    size_t unit = strlen(number_specs[kind].unit);
    if (!strncasecmp(end, number_specs[kind].unit, unit)) end += unit;
    while (*end == ' ') end++;
    return *end == '\0';
}

static void open_number_field(enum number_kind kind) {
    number_field.open = true;
    number_field.kind = kind;
    number_field.selected = true;
    number_field.invalid = false;
    format_number(kind, number_value(kind), number_field.text, sizeof number_field.text);
}

// The typed value when it is a number, else the current one, moved by `steps` steps and applied at once.
static void step_number(enum number_kind kind, int steps) {
    double value;
    if (!number_field.open || number_field.kind != kind || !parse_number(kind, number_field.text, &value))
        value = number_value(kind);
    set_number(kind, value + steps);
    if (number_field.open && number_field.kind == kind) open_number_field(kind);
}

static void accept_number_field(void) {
    double value;
    if (!parse_number(number_field.kind, number_field.text, &value)) {
        number_field.invalid = true;
        return;
    }
    set_number(number_field.kind, value);
    number_field.open = false;
}

static void type_number(const char *text) {
    if (number_field.selected) number_field.text[0] = '\0';
    number_field.selected = false;
    number_field.invalid = false;
    size_t length = strlen(number_field.text);
    snprintf(number_field.text + length, sizeof number_field.text - length, "%s", text);
}

static void number_backspace(void) {
    size_t length = number_field.selected ? 0 : strlen(number_field.text);
    while (length > 0 && (number_field.text[--length] & 0xC0) == 0x80) {}
    number_field.text[length] = '\0';
    number_field.selected = false;
    number_field.invalid = false;
}

static void set_theme(const struct theme *next) {
    theme = next;
    snprintf(settings.theme, sizeof settings.theme, "%s", next->name);
    save_settings();
}

static void set_autocomplete(enum autocomplete_style style, enum autocomplete_trigger trigger) {
    close_completion();
    settings.autocomplete_style = style;
    settings.autocomplete_trigger = trigger;
    save_settings();
    char text[128];
    snprintf(text, sizeof text, "autocomplete: %s, %s", style == AUTOCOMPLETE_GHOST ? "ghost text" : "list",
             trigger == AUTOCOMPLETE_TYPING ? "while typing" : "Ctrl+Space");
    ui_show_message(text, false);
}

static void set_signature_help(enum signature_help mode) {
    settings.signature_help = mode;
    save_settings();
    ui_show_message(mode == SIGNATURE_HELP_PAREN ? "signature help: Ctrl+Shift+Space and after \"(\""
                                                 : "signature help: Ctrl+Shift+Space", false);
}

static void set_font(const char *path) {
    if (!overlay_set_font(path)) return ui_show_message("cannot use this font", true);
    snprintf(settings.font, sizeof settings.font, "%s", path);
    layout.valid = false;
    save_settings();
}

// Replaces the code with the result of a JS function of js/editor-actions.js (examples, mutator).
static bool replace_with(const char *function, const char *failure) {
    char *code = host.call(function, editor.text);
    if (!code) {
        ui_show_message(failure, true);
        return false;
    }
    editor_replace_text(&editor, code, strlen(code));
    free(code);
    return true;
}

// The web editor formats the code after the mutator (editor-store.js); astring gives it unindented.
static bool mutate(const char *function) {
    if (!replace_with(function, "mutator: the code does not parse")) return false;
    editor_format(&editor);
    return true;
}

// Code that replaces the whole sketch (an example, a URL) does not belong to the open file, so that Ctrl+S cannot
// overwrite the file with it; Ctrl+S then asks for a file.
static void close_file(void) {
    free(file_path);
    file_path = NULL;
}

// Clear all; with an open file, reset: back to the saved state of the file, which runs again.
static void clear_all(void) {
    free(evaluate_code("hush(); speed = 1", strlen("hush(); speed = 1"), "<clear>"));
    if (file_path) {
        char *path = strdup(file_path); // open_file replaces file_path
        open_file(path);
        free(path);
        return;
    }
    editor_replace_text(&editor, CLEARED_CODE, strlen(CLEARED_CODE));
    char *url = host.call("__webEditorUrl", ""); // 'gallery: clear' removes the code from the URL
    if (url) show_title(url);
    free(url);
}

// Like "save to URL" of the web editor; without an address bar, the URL goes to the window title and the clipboard.
static void save_to_url(void) {
    char *url = host.call("__urlForCode", editor.text);
    if (!url) return;
    show_title(url);
    SDL_SetClipboardText(url);
    ui_show_message("copied the hydra.ojack.xyz URL of the code to the clipboard", false);
    free(url);
}

// Like pasting a sketch URL into the address bar: loads the code of the URL in the clipboard and runs it.
static void open_url_from_clipboard(void) {
    char *url = SDL_GetClipboardText();
    char *code = url && (strstr(url, "code=") || strstr(url, "sketch_id=")) ? host.call("__codeFromUrl", url) : NULL;
    if (!code) {
        ui_show_message("the clipboard has no hydra.ojack.xyz URL with code= or sketch_id=", true);
    } else {
        close_file();
        editor_replace_text(&editor, code, strlen(code));
        editor_set_cursor(&editor, 0, false);
        editor.modified = false; // no recovery for code that nobody edited
        evaluate_range(0, editor.length);
        show_title(url);
    }
    free(code);
    SDL_free(url);
}

// An action that replaces the code or closes the file. With unsaved changes of the open file, it waits for the
// answer to the question: s saves, then it runs; d discards the changes, then it runs; Escape cancels it.
enum pending_action {
    PENDING_NONE,
    PENDING_RESET,
    PENDING_CLEAR,
    PENDING_EXAMPLE,
    PENDING_OPEN_FILE, // the path is in pending.path
    PENDING_OPEN_URL,
    PENDING_QUIT,
};
static struct {
    enum pending_action action;
    char *path;
} pending;

static void run_pending(void) {
    enum pending_action action = pending.action;
    pending.action = PENDING_NONE;
    switch (action) {
    case PENDING_NONE: break;
    case PENDING_RESET: clear_all(); break;
    case PENDING_CLEAR:
        close_file();
        clear_all();
        break;
    case PENDING_EXAMPLE:
        close_file();
        clear_all();
        if (replace_with("__randomExample", "no example")) {
            evaluate_range(0, editor.length);
            update_url();
        }
        break;
    case PENDING_OPEN_FILE: open_file(pending.path); break;
    case PENDING_OPEN_URL: open_url_from_clipboard(); break;
    case PENDING_QUIT:
        remove_recovery();
        host.quit();
        break;
    }
}

static void confirm_then(enum pending_action action, const char *path) {
    free(pending.path);
    pending.path = path ? strdup(path) : NULL;
    pending.action = action;
    if (!file_path || !editor.modified) return run_pending();
    visible = true; // also for Ctrl+Q while the code is hidden
    help_visible = false;
    number_field.open = false;
    open_prompt(PROMPT_UNSAVED);
}

static void answer_unsaved(char answer) {
    answer = (char)tolower((unsigned char)answer);
    if (answer != 's' && answer != 'd') return;
    prompt.kind = PROMPT_NONE;
    if (answer == 's') save();
    if (answer == 's' && editor.modified) return; // the bar shows why the save failed, and the action waits
    if (answer == 'd') remove_recovery();
    run_pending();
}

// The answer of the open dialog of the "file" row.
void ui_open_file(const char *path) { confirm_then(PENDING_OPEN_FILE, path); }

void ui_quit(void) { confirm_then(PENDING_QUIT, NULL); }

const struct settings *ui_settings(void) { return &settings; }

static void escape(void) {
    if (reset_menu_open) reset_menu_open = false;
    else if (completion.open) close_completion();
    else if (signature_help_open) signature_help_open = false;
    else if (prompt.kind != PROMPT_NONE) prompt.kind = PROMPT_NONE;
    else if (help_visible) help_visible = false;
    else if (info_visible) info_visible = false;
    else if (extensions_visible) extensions_visible = false;
    else if (settings_visible) settings_visible = false;
    else if (visible && editor_selection_count(&editor) > 1) editor_collapse_to_first(&editor);
    else if (visible && editor_has_selection(&editor)) editor_set_cursor(&editor, editor.cursor, false);
    else if (host.is_fullscreen()) host.toggle_fullscreen();
}

static void prompt_backspace(void) {
    char *field = prompt_field();
    if (prompt.query_selected && field == prompt.query) field[0] = '\0';
    prompt.query_selected = false;
    size_t length = strlen(field);
    while (length > 0 && (field[--length] & 0xC0) == 0x80) {}
    field[length] = '\0';
    prompt_changed();
}

static void run_command_body(enum command command, bool extend);

static bool is_completion_command(enum command command) {
    return command == COMMAND_TRIGGER_COMPLETION || command == COMMAND_COMPLETION_NEXT ||
        command == COMMAND_COMPLETION_PREVIOUS ||
           command == COMMAND_COMPLETION_ACCEPT;
}

// The command palette reaches every command, and the keys of the prompt context apply to all prompts. A command
// of a context does nothing where it has no meaning (the list is closed, the prompt is not "replace").
static bool command_applies(enum command command) {
    switch (command) {
    case COMMAND_COMPLETION_NEXT:
    case COMMAND_COMPLETION_PREVIOUS:
    case COMMAND_COMPLETION_ACCEPT: return completion.open;
    case COMMAND_SNIPPET_NEXT:
    case COMMAND_SNIPPET_PREVIOUS:
    case COMMAND_SNIPPET_END: return snippet.active;
    case COMMAND_NUMBER_ACCEPT:
    case COMMAND_NUMBER_CANCEL:
    case COMMAND_NUMBER_UP:
    case COMMAND_NUMBER_DOWN:
    case COMMAND_NUMBER_UP_LARGE:
    case COMMAND_NUMBER_DOWN_LARGE:
    case COMMAND_NUMBER_BACKSPACE: return number_field.open;
    case COMMAND_REPLACE_ALL: return prompt.kind == PROMPT_REPLACE;
    case COMMAND_MENU_NEXT:
    case COMMAND_MENU_PREVIOUS:
    case COMMAND_MENU_ACCEPT: return reset_menu_open;
    default: return true;
    }
}

// Open suggestions follow Backspace and close for any other command, as in VS Code.
static void run_command(enum command command, bool extend) {
    if (!command_applies(command)) return;
    bool was_open = completion.open;
    size_t old_length = editor.length;
    unsigned generation = snippet.generation;
    run_command_body(command, extend);
    if (generation == snippet.generation) follow_snippet(old_length);
    if (was_open && command == COMMAND_BACKSPACE) open_completion(true);
}

static void run_command_body(enum command command, bool extend) {
    size_t start, end;
    long page = (long)visible_rows() - 1;
    if (!is_completion_command(command)) close_completion();
    if (command != COMMAND_MENU_NEXT && command != COMMAND_MENU_PREVIOUS && command != COMMAND_ESCAPE)
        reset_menu_open = false;
    switch (command) {
    case COMMAND_NONE:
    case COMMAND_COUNT: break;
    case COMMAND_TOGGLE_CODE: visible = !visible; return;
    case COMMAND_EVAL_LINE: editor_line_range(&editor, &start, &end); evaluate_range(start, end); break;
    case COMMAND_EVAL_BLOCK: editor_block_range(&editor, &start, &end); evaluate_range(start, end); break;
    case COMMAND_EVAL_ALL: evaluate_all(); break;
    case COMMAND_SAVE: save(); break;
    case COMMAND_FULLSCREEN: host.toggle_fullscreen(); break;
    case COMMAND_QUIT: ui_quit(); break;
    case COMMAND_ESCAPE: escape(); break;
    case COMMAND_UNDO: editor_undo(&editor); break;
    case COMMAND_REDO: editor_redo(&editor); break;
    case COMMAND_COPY: copy_selection(false); break;
    case COMMAND_CUT: copy_selection(true); break;
    case COMMAND_PASTE: paste(); break;
    case COMMAND_SELECT_ALL: editor_select_all(&editor); break;
    case COMMAND_TOGGLE_COMMENT: editor_toggle_comment(&editor); break;
    case COMMAND_MOVE_LINE_UP: editor_move_lines(&editor, -1); break;
    case COMMAND_MOVE_LINE_DOWN: editor_move_lines(&editor, 1); break;
    case COMMAND_DUPLICATE_LINE: editor_duplicate_lines(&editor); break;
    case COMMAND_FORMAT: editor_format(&editor); break;
    case COMMAND_SCREENCAP: host.screencap(); break;
    case COMMAND_SAVE_TO_URL: save_to_url(); break;
    case COMMAND_OPEN_URL: confirm_then(PENDING_OPEN_URL, NULL); break;
    case COMMAND_GALLERY_SHARE: {
        // Like 'gallery:shareSketch': evaluate first, and ask only when the code runs.
        char *error = evaluate_code(editor.text, editor.length, "<editor>");
        show_result("", error);
        if (!error) open_prompt(PROMPT_SHARE);
        free(error);
        return;
    }
    case COMMAND_CLEAR_ALL: confirm_then(PENDING_RESET, NULL); break;
    case COMMAND_CLEAR_AND_CLOSE: confirm_then(PENDING_CLEAR, NULL); break;
    case COMMAND_SHOW_EXAMPLE: confirm_then(PENDING_EXAMPLE, NULL); break;
    // The mutator evaluates the code itself and retries on errors, like in the web editor.
    case COMMAND_MUTATE:
        if (mutate("__mutate")) update_url();
        break;
    case COMMAND_MUTATE_TRANSFORM:
        if (mutate("__mutateTransform")) update_url();
        break;
    case COMMAND_MUTATE_UNDO: replace_with("__mutateUndo", "mutator: nothing to undo"); break;
    case COMMAND_TOGGLE_HELP: help_visible = !help_visible; return;
    case COMMAND_TOGGLE_EXTENSIONS:
        extensions_visible = !extensions_visible;
        info_visible = settings_visible = false;
        info_scroll = 0;
        return;
    case COMMAND_TOGGLE_INFO:
        info_visible = !info_visible;
        extensions_visible = settings_visible = false;
        info_scroll = 0;
        return;
    case COMMAND_TOGGLE_SETTINGS:
        settings_visible = !settings_visible;
        if (settings_visible) load_settings_lists();
        info_visible = extensions_visible = false;
        info_scroll = 0;
        return;
    case COMMAND_COPY_PATCH_BAY_ID: copy_patch_bay_id(); return;
    case COMMAND_SET_SERVER:
        open_prompt(PROMPT_SERVER);
        snprintf(prompt.query, sizeof prompt.query, "%.*s", (int)sizeof prompt.query - 1, settings.gallery_server);
        prompt.query_selected = prompt.query[0] != '\0'; // typing replaces the current URL
        return;
    case COMMAND_NEXT_THEME: {
        set_theme(&themes[(size_t)(theme - themes + 1) % theme_count]);
        char text[96];
        snprintf(text, sizeof text, "theme %s", theme->name);
        ui_show_message(text, false);
        return;
    }
    case COMMAND_TOGGLE_AUTOCOMPLETE_STYLE:
        set_autocomplete(settings.autocomplete_style == AUTOCOMPLETE_LIST ? AUTOCOMPLETE_GHOST : AUTOCOMPLETE_LIST,
                         settings.autocomplete_trigger);
        return;
    case COMMAND_TOGGLE_AUTOCOMPLETE_TRIGGER:
        set_autocomplete(settings.autocomplete_style,
                         settings.autocomplete_trigger == AUTOCOMPLETE_MANUAL ? AUTOCOMPLETE_TYPING
                         : AUTOCOMPLETE_MANUAL);
        return;
    case COMMAND_SELECT_NEXT_OCCURRENCE: editor_select_next_occurrence(&editor); break;
    case COMMAND_FIND: open_prompt(PROMPT_FIND); break;
    case COMMAND_REPLACE: open_prompt(PROMPT_REPLACE); break;
    case COMMAND_GO_TO_LINE: open_prompt(PROMPT_GO_TO_LINE); break;
    case COMMAND_COMMAND_PALETTE: open_prompt(PROMPT_PALETTE); return;
    case COMMAND_FIND_NEXT: find_again(1); break;
    case COMMAND_FIND_PREVIOUS: find_again(-1); break;
    case COMMAND_FONT_BIGGER: change_font_size(FONT_SIZE_STEP); break;
    case COMMAND_FONT_SMALLER: change_font_size(-FONT_SIZE_STEP); break;
    case COMMAND_NEWLINE: editor_insert_newline(&editor); break;
    case COMMAND_BACKSPACE: editor_backspace(&editor); break;
    case COMMAND_DELETE: editor_delete_forward(&editor); break;
    case COMMAND_DELETE_WORD_LEFT: editor_delete_word(&editor, -1); break;
    case COMMAND_DELETE_WORD_RIGHT: editor_delete_word(&editor, 1); break;
    case COMMAND_INDENT: editor_indent(&editor, false); break;
    case COMMAND_OUTDENT: editor_indent(&editor, true); break;
    case COMMAND_INDENT_MORE: editor_indent_lines(&editor, false); break;
    case COMMAND_INDENT_LESS: editor_indent_lines(&editor, true); break;
    case COMMAND_SELECT_LINE: editor_select_lines(&editor); break;
    case COMMAND_DELETE_LINE: editor_delete_lines(&editor); break;
    case COMMAND_JOIN_LINES: editor_join_lines(&editor); break;
    case COMMAND_SORT_LINES: editor_sort_lines(&editor, false, false); break;
    case COMMAND_SORT_LINES_INSENSITIVE: editor_sort_lines(&editor, false, true); break;
    case COMMAND_SORT_LINES_REVERSE: editor_sort_lines(&editor, true, false); break;
    case COMMAND_SORT_LINES_REVERSE_INSENSITIVE: editor_sort_lines(&editor, true, true); break;
    case COMMAND_CHORD:
        chord_prefix = SDLK_K;
        ui_show_message("Ctrl+K: press the second key", false);
        return;
    case COMMAND_SUBWORD_LEFT: editor_move(&editor, MOTION_SUBWORD_LEFT, extend); break;
    case COMMAND_SUBWORD_RIGHT: editor_move(&editor, MOTION_SUBWORD_RIGHT, extend); break;
    case COMMAND_TOGGLE_BOOKMARK: editor_toggle_bookmark(&editor); break;
    case COMMAND_NEXT_BOOKMARK: editor_next_bookmark(&editor, 1); break;
    case COMMAND_PREVIOUS_BOOKMARK: editor_next_bookmark(&editor, -1); break;
    case COMMAND_CLEAR_BOOKMARKS: editor_clear_bookmarks(&editor); break;
    case COMMAND_SELECT_BOOKMARKS: editor_select_bookmarks(&editor); break;
    case COMMAND_FIND_UNDER: editor_find_under(&editor, 1); break;
    case COMMAND_FIND_UNDER_PREVIOUS: editor_find_under(&editor, -1); break;
    case COMMAND_FIND_ALL_UNDER: editor_select_all_under(&editor); break;
    case COMMAND_SELECT_SCOPE: editor_select_scope(&editor); break;
    case COMMAND_UPPER_CASE: editor_change_case(&editor, true); break;
    case COMMAND_LOWER_CASE: editor_change_case(&editor, false); break;
    case COMMAND_DELETE_LINE_RIGHT: editor_delete_line_part(&editor, 1); break;
    case COMMAND_DELETE_LINE_LEFT: editor_delete_line_part(&editor, -1); break;
    case COMMAND_SHOW_IN_CENTER: {
        update_layout();
        size_t x, row = global_row(editor.cursor, &x);
        scroll_row = row > visible_rows() / 2 ? row - visible_rows() / 2 : 0;
        return;
    }
    case COMMAND_SET_MARK: editor_set_mark(&editor); break;
    case COMMAND_SELECT_TO_MARK: editor_select_to_mark(&editor); break;
    case COMMAND_DELETE_TO_MARK: editor_delete_to_mark(&editor); break;
    case COMMAND_SWAP_WITH_MARK: editor_swap_with_mark(&editor); break;
    case COMMAND_YANK: editor_yank(&editor); break;
    case COMMAND_SKIP_AND_SELECT_NEXT: editor_skip_and_select_next(&editor); break;
    case COMMAND_HARD_WRAP: editor_hard_wrap(&editor, EDITOR_HARD_WRAP_COLUMNS); break;
    case COMMAND_TRANSPOSE: editor_transpose(&editor); break;
    case COMMAND_GO_TO_BRACKET: editor_go_to_bracket(&editor); break;
    case COMMAND_SELECT_BETWEEN_BRACKETS: editor_select_between_brackets(&editor); break;
    case COMMAND_ADD_CURSOR_ABOVE: editor_add_cursor_vertical(&editor, -1); break;
    case COMMAND_ADD_CURSOR_BELOW: editor_add_cursor_vertical(&editor, 1); break;
    case COMMAND_SPLIT_SELECTION: editor_split_selection_by_line(&editor); break;
    case COMMAND_TOGGLE_WRAP:
        wrap = !wrap;
        layout.valid = false;
        break;
    case COMMAND_TRIGGER_COMPLETION: open_completion(true); break;
    case COMMAND_COMPLETION_NEXT: completion.selected = (completion.selected + 1) % completion.count; return;
    case COMMAND_COMPLETION_PREVIOUS:
        completion.selected = (completion.selected + completion.count - 1) % completion.count;
        return;
    case COMMAND_COMPLETION_ACCEPT: accept_completion(); break;
    case COMMAND_SIGNATURE_HELP: {
        struct call_context call;
        signature_help_open = call_at(editor.cursor, &call) != NULL;
        if (!signature_help_open) ui_show_message("no hydra function around the cursor", false);
        break;
    }
    case COMMAND_SNIPPET_NEXT: next_tab_stop(1); break;
    case COMMAND_SNIPPET_PREVIOUS: next_tab_stop(-1); break;
    case COMMAND_SNIPPET_END: snippet.active = false; break;
    case COMMAND_NUMBER_ACCEPT: accept_number_field(); return;
    case COMMAND_NUMBER_CANCEL: number_field.open = false; return;
    case COMMAND_NUMBER_UP: step_number(number_field.kind, 1); return;
    case COMMAND_NUMBER_DOWN: step_number(number_field.kind, -1); return;
    case COMMAND_NUMBER_UP_LARGE: step_number(number_field.kind, LARGE_STEPS); return;
    case COMMAND_NUMBER_DOWN_LARGE: step_number(number_field.kind, -LARGE_STEPS); return;
    case COMMAND_NUMBER_BACKSPACE: number_backspace(); return;
    case COMMAND_PROMPT_ACCEPT: accept_prompt(1); break;
    case COMMAND_PROMPT_ACCEPT_BACKWARD: accept_prompt(-1); break;
    case COMMAND_PROMPT_NEXT_FIELD: prompt.field = prompt.kind == PROMPT_REPLACE ? !prompt.field : 0; return;
    // The palette list grows upwards from the prompt line, so Up goes to the next entry.
    case COMMAND_PROMPT_UP:
        if (palette_command(prompt.palette_index + 1) != COMMAND_NONE) prompt.palette_index++;
        return;
    case COMMAND_PROMPT_DOWN: if (prompt.palette_index > 0) prompt.palette_index--; return;
    case COMMAND_PROMPT_BACKSPACE: prompt_backspace(); break;
    case COMMAND_MENU_NEXT: reset_menu_selected = (reset_menu_selected + 1) % RESET_MENU_COUNT; return;
    case COMMAND_MENU_PREVIOUS:
        reset_menu_selected = (reset_menu_selected + RESET_MENU_COUNT - 1) % RESET_MENU_COUNT;
        return;
    case COMMAND_MENU_ACCEPT: run_command(reset_menu[reset_menu_selected].command, false); return;
    case COMMAND_REPLACE_ALL: {
        size_t count =
            editor_replace_all(&editor, prompt.query, strlen(prompt.query), prompt.replacement,
                               strlen(prompt.replacement), true);
        char text[64];
        snprintf(text, sizeof text, "%zu replaced", count);
        ui_show_message(text, false);
        break;
    }
    case COMMAND_LEFT: editor_move(&editor, MOTION_LEFT, extend); break;
    case COMMAND_RIGHT: editor_move(&editor, MOTION_RIGHT, extend); break;
    case COMMAND_UP: editor_move(&editor, MOTION_UP, extend); break;
    case COMMAND_DOWN: editor_move(&editor, MOTION_DOWN, extend); break;
    case COMMAND_WORD_LEFT: editor_move(&editor, MOTION_GROUP_LEFT, extend); break;
    case COMMAND_WORD_RIGHT: editor_move(&editor, MOTION_GROUP_RIGHT, extend); break;
    case COMMAND_LINE_START: editor_move(&editor, MOTION_LINE_START, extend); break;
    case COMMAND_LINE_END: editor_move(&editor, MOTION_LINE_END, extend); break;
    case COMMAND_DOCUMENT_START: editor_move(&editor, MOTION_DOCUMENT_START, extend); break;
    case COMMAND_DOCUMENT_END: editor_move(&editor, MOTION_DOCUMENT_END, extend); break;
    case COMMAND_PAGE_UP:
        editor_move_vertical(&editor, -page, extend);
        scroll_row = scroll_row > (size_t)page ? scroll_row - (size_t)page : 0;
        break;
    case COMMAND_PAGE_DOWN:
        editor_move_vertical(&editor, page, extend);
        scroll_row += (size_t)page;
        break;
    }
    if (visible) reveal_cursor();
}

static enum key_context key_context(void) {
    if (!settings_visible) number_field.open = false;
    if (number_field.open) return CONTEXT_NUMBER;
    if (reset_menu_open) return CONTEXT_MENU;
    if (completion.open) return settings.autocomplete_style == AUTOCOMPLETE_GHOST ? CONTEXT_GHOST : CONTEXT_COMPLETION;
    if (prompt.kind != PROMPT_NONE) return CONTEXT_PROMPT;
    follow_snippet(editor.length); // the snippet ends when a mouse click moved the cursor out of it
    if (snippet.active) return CONTEXT_SNIPPET;
    return CONTEXT_ANY;
}

bool ui_key(const SDL_KeyboardEvent *event) {
    suppress_text = false;
    bool extend = false;
    SDL_Keycode key = event->key;
    if (key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LSHIFT || key == SDLK_RSHIFT || key == SDLK_LALT ||
        key == SDLK_RALT)
        return false; // a modifier alone keeps a pending two-key command
    SDL_Keycode prefix = chord_prefix;
    chord_prefix = 0;
    const struct key_binding *binding =
        keymap_lookup(key, (SDL_Keymod)event->mod, visible ? key_context() : CONTEXT_ANY, prefix, &extend);
    // The key of a command that does not apply here keeps its meaning outside of the context (Ctrl+Alt+Enter
    // evaluates all in the find prompt).
    if (binding && !command_applies(binding->command))
        binding = keymap_lookup(key, (SDL_Keymod)event->mod, CONTEXT_ANY, prefix, &extend);
    if (prefix && !binding) {
        ui_show_message("no command for this key after Ctrl+K", false);
        suppress_text = true;
        return true;
    }
    if (!binding || (!visible && !binding->global)) return false;
    // A held key repeats editing commands, but it does not evaluate or toggle again and again.
    if (event->repeat && binding->global) return true;
    suppress_text = true;
    run_command(binding->command, extend);
    return true;
}

void ui_text_editing(const char *text) { snprintf(composition, sizeof composition, "%s", text); }

void ui_text_input(const char *text) {
    composition[0] = '\0';
    if (suppress_text || !visible || (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_LALT))) return;
    reset_menu_open = false;
    if (prompt.kind == PROMPT_UNSAVED) return answer_unsaved(text[0]);
    if (help_visible) return;
    if (number_field.open && settings_visible) {
        type_number(text);
        return;
    }
    if (prompt.kind != PROMPT_NONE) {
        char *field = prompt_field();
        if (prompt.query_selected && field == prompt.query) field[0] = '\0';
        prompt.query_selected = false;
        size_t length = strlen(field);
        snprintf(field + length, MAX_PROMPT - length, "%s", text);
        prompt_changed();
        reveal_cursor();
        return;
    }
    size_t old_length = editor.length;
    editor_type(&editor, text, strlen(text));
    follow_snippet(old_length);
    struct call_context call;
    if (!strcmp(text, "(") && settings.signature_help == SIGNATURE_HELP_PAREN && call_at(editor.cursor, &call))
        signature_help_open = true;
    bool word = isalnum((unsigned char)text[0]) || text[0] == '_' || text[0] == '$';
    // Open suggestions follow the typed word; new ones open only with the trigger "while typing" (the default is
    // Ctrl+Space alone).
    if (completion.open) open_completion(true);
    else if (word && settings.autocomplete_trigger == AUTOCOMPLETE_TYPING) open_completion(false);
    reveal_cursor();
}

// ---------------------------------------------------------------- toolbar

struct toolbar_item {
    const char *label;
    const char *title; // the tooltip in the texts of the web editor (toolbar.run, ...)
    enum command command;
    enum command with_shift; // like Shift+click on the dice of the web editor
    enum command with_ctrl;
};

static const struct toolbar_item toolbar[] = {
    { "run", "toolbar.run", COMMAND_EVAL_ALL, COMMAND_EVAL_ALL, COMMAND_EVAL_ALL },
    { "clear", "toolbar.clear", COMMAND_CLEAR_ALL, COMMAND_CLEAR_ALL, COMMAND_CLEAR_ALL },
    { "extensions", "toolbar.load-extension", COMMAND_TOGGLE_EXTENSIONS, COMMAND_TOGGLE_EXTENSIONS,
      COMMAND_TOGGLE_EXTENSIONS },
    { "example", "toolbar.shuffle", COMMAND_SHOW_EXAMPLE, COMMAND_SHOW_EXAMPLE, COMMAND_SHOW_EXAMPLE },
    { "mutate", "toolbar.random", COMMAND_MUTATE, COMMAND_MUTATE_UNDO, COMMAND_MUTATE_TRANSFORM },
    { "share", "toolbar.upload", COMMAND_GALLERY_SHARE, COMMAND_GALLERY_SHARE, COMMAND_GALLERY_SHARE },
    // not in the web editor
    { "settings", NULL, COMMAND_TOGGLE_SETTINGS, COMMAND_TOGGLE_SETTINGS, COMMAND_TOGGLE_SETTINGS },
    { "?", "toolbar.show-info", COMMAND_TOGGLE_INFO, COMMAND_TOGGLE_HELP, COMMAND_TOGGLE_HELP },
};
#define TOOLBAR_COUNT (sizeof toolbar / sizeof toolbar[0])

static bool toolbar_shown(size_t index) { return gallery_enabled || toolbar[index].command != COMMAND_GALLERY_SHARE; }

static const char *toolbar_label(size_t index) {
    return toolbar[index].command == COMMAND_CLEAR_ALL && file_path ? "reset" : toolbar[index].label;
}

// The right-aligned boxes of the toolbar; returns the x of item `index`, and its width in `width`.
static float toolbar_x(size_t index, float *width) {
    float x = (float)last_width - MARGIN;
    for (size_t i = TOOLBAR_COUNT; i-- > index;) {
        if (!toolbar_shown(i)) continue;
        *width = ((float)strlen(toolbar_label(i)) + 1) * overlay_char_width();
        x -= *width + TEXT_PADDING;
    }
    return x;
}

// The drop-down list of "reset" lies below the button and is as wide as it; returns its left edge.
static float reset_menu_left(float *width) {
    size_t button = 0;
    while (toolbar[button].command != COMMAND_CLEAR_ALL) button++;
    return toolbar_x(button, width);
}

static float reset_menu_top(void) { return MARGIN + overlay_line_height() + TEXT_PADDING; }

// The entry of the open list at (x, y), or RESET_MENU_COUNT.
static size_t reset_menu_entry_at(float x, float y) {
    float width, left = reset_menu_left(&width), top = reset_menu_top();
    if (!reset_menu_open || x < left || x > left + width || y < top) return RESET_MENU_COUNT;
    size_t row = (size_t)((y - top) / overlay_line_height());
    return row < RESET_MENU_COUNT ? row : RESET_MENU_COUNT;
}

static bool toolbar_click(float x, float y, SDL_Keymod modifiers) {
    if (y < MARGIN || y > MARGIN + overlay_line_height()) return false;
    for (size_t i = 0; i < TOOLBAR_COUNT; i++) {
        if (!toolbar_shown(i)) continue;
        float width, left = toolbar_x(i, &width);
        if (x < left || x > left + width) continue;
        const struct toolbar_item *item = &toolbar[i];
        if (item->command == COMMAND_CLEAR_ALL && file_path) {
            reset_menu_open = !reset_menu_open;
            reset_menu_selected = 0;
            return true;
        }
        enum command command = item->command;
        if (modifiers & SDL_KMOD_CTRL) command = item->with_ctrl;
        if (modifiers & SDL_KMOD_SHIFT) command = item->with_shift;
        run_command(command, false);
        return true;
    }
    return false;
}

// A tooltip with its right edge at `right`.
static void draw_tooltip(float right, float top, const char *text) {
    float width = overlay_text_width(text, strlen(text)), left = right - width;
    if (left < MARGIN) left = MARGIN;
    overlay_rect(left - TEXT_PADDING, top, width + 2 * TEXT_PADDING, overlay_line_height(), theme->popup_background);
    overlay_text_run(left, top, text, strlen(text), theme->text);
}

static void draw_reset_menu(void) {
    float width, left = reset_menu_left(&width), top = reset_menu_top(), line_height = overlay_line_height();
    for (size_t entry = 0; entry < RESET_MENU_COUNT; entry++) {
        float y = top + (float)entry * line_height;
        overlay_rect(left, y, width, line_height,
                     entry == reset_menu_selected ? theme->popup_selected : theme->popup_background);
        overlay_text(left + overlay_char_width() / 2, y, reset_menu[entry].label, strlen(reset_menu[entry].label), 0,
                     64, NULL, theme->text);
    }
    size_t hovered = reset_menu_entry_at(pointer_x, pointer_y);
    if (hovered < RESET_MENU_COUNT)
        draw_tooltip(left - 2 * TEXT_PADDING, top + (float)hovered * line_height, reset_menu[hovered].title);
}

static void draw_toolbar(void) {
    for (size_t i = 0; i < TOOLBAR_COUNT; i++) {
        if (!toolbar_shown(i)) continue;
        float width, x = toolbar_x(i, &width);
        // The buttons look like the code lines: the box and the text of "code display" in the code layers.
        overlay_set_layer(OVERLAY_CODE_BACKGROUND);
        overlay_rect(x, MARGIN, width, overlay_line_height(), code_background_color());
        overlay_set_layer(OVERLAY_CODE_TEXT);
        overlay_text(x + overlay_char_width() / 2, MARGIN, toolbar_label(i), strlen(toolbar_label(i)), 0, 64, NULL,
                     theme->info);
        overlay_set_layer(OVERLAY_TOP);
        bool hover = pointer_y >= MARGIN && pointer_y <= MARGIN + overlay_line_height() && pointer_x >= x &&
                     pointer_x <= x + width;
        if (!hover || reset_menu_open) continue; // the open list takes the place of the tooltip
        const char *title = toolbar[i].command == COMMAND_TOGGLE_INFO &&
            info_visible ? "toolbar.hide-info" : toolbar[i].title;
        char *text = toolbar[i].command == COMMAND_CLEAR_ALL && file_path ? strdup(RESET_BUTTON_TITLE)
            : title ? host.call("__translate", title) : strdup(command_description(toolbar[i].command));
        if (!text) continue;
        draw_tooltip(x + width, MARGIN + overlay_line_height() + TEXT_PADDING, text);
        free(text);
    }
    if (reset_menu_open) draw_reset_menu();
}

// ---------------------------------------------------------------- info panel

// The info window of the web editor (src/views/info.js) with its language buttons; the texts come from
// js/l10n.js in the selected language.
static void info_panel(float *left, float *top, float *width, float *height) {
    size_t columns = visible_columns() < INFO_COLUMNS + 4 ? visible_columns() : INFO_COLUMNS + 4;
    *width = (float)columns * overlay_char_width();
    *left = ((float)last_width - *width) / 2;
    *top = MARGIN + 2 * overlay_line_height();
    *height = (float)last_height - *top - MARGIN - bar_height();
}

static bool in_info_panel(float x, float y) {
    float left, top, width, height;
    info_panel(&left, &top, &width, &height);
    return x >= left && x <= left + width && y >= top && y <= top + height;
}

static bool starts_wide(const char *text) {
    size_t consumed;
    return unicode_columns(unicode_decode((const unsigned char *)text, strlen(text), &consumed)) == 2;
}

// Breaks a paragraph into lines of at most `width` pixels: at spaces, and between characters of scripts without
// spaces (Japanese, Chinese). Calls `line` for each line.
static void wrap_text(const char *text, float width, void (*line)(const char *text, size_t length, void *data),
                      void *data) {
    size_t length = strlen(text), start = 0, end = 0;
    while (start < length) {
        size_t candidate = end;
        // The next break: after a space, or after a wide character.
        if (candidate < length && starts_wide(text + candidate)) {
            size_t consumed;
            unicode_decode((const unsigned char *)text + candidate, length - candidate, &consumed);
            candidate += consumed;
        } else {
            while (candidate < length && text[candidate] != ' ' && !starts_wide(text + candidate)) candidate++;
            while (candidate < length && text[candidate] == ' ') candidate++;
        }
        bool fits = overlay_text_width(text + start, candidate - start) <= width;
        if (!fits && end > start) {
            size_t trimmed = end;
            while (trimmed > start && text[trimmed - 1] == ' ') trimmed--;
            line(text + start, trimmed - start, data);
            start = end;
        } else {
            end = candidate;
            if (end >= length) {
                line(text + start, length - start, data);
                return;
            }
        }
    }
}

struct info_layout {
    float left, top;
    size_t row, rows, skipped;
};

static void draw_info_line(const char *text, size_t length, void *data) {
    struct info_layout *layout_state = data;
    if (layout_state->skipped++ < info_scroll || layout_state->row >= layout_state->rows) return;
    overlay_text_run(layout_state->left, layout_state->top + overlay_line_height() * (float)layout_state->row++,
                     text, length, theme->text);
}

static void draw_info(void) {
    float left, top, width, height, line_height = overlay_line_height(), char_width = overlay_char_width();
    info_panel(&left, &top, &width, &height);
    overlay_rect(left, top, width, height, theme->help_background);
    char *languages = host.call("__languages", "");
    float x = left + char_width;
    language_button_count = 0;
    for (char *line = languages ? strtok(languages, "\n") : NULL; line && language_button_count < MAX_LANGUAGES;
         line = strtok(NULL, "\n")) {
        char *name = strchr(line, '\t');
        if (!name) continue;
        *name++ = '\0';
        char *selected = strchr(name, '\t');
        if (selected) *selected++ = '\0';
        float name_width = overlay_text_width(name, strlen(name)) + char_width;
        if (x + name_width > left + width) break;
        snprintf(language_buttons[language_button_count].code, sizeof language_buttons[0].code, "%s", line);
        language_buttons[language_button_count].x = x;
        language_buttons[language_button_count++].width = name_width;
        overlay_rect(x, top + line_height / 2, name_width, line_height,
                     selected && *selected == '1' ? theme->popup_selected : theme->toolbar);
        overlay_text_run(x + char_width / 2, top + line_height / 2, name, strlen(name), theme->text);
        x += name_width + char_width;
    }
    free(languages);
    char *text = host.call("__infoText", "");
    if (!text) return;
    struct info_layout layout_state = { left + 2 * char_width, top + 2 * line_height, 0,
                                        (size_t)((height - 3 * line_height) / line_height), 0 };
    char *saveptr;
    for (char *paragraph = strtok_r(text, "\n", &saveptr); paragraph; paragraph = strtok_r(NULL, "\n", &saveptr)) {
        wrap_text(paragraph, width - 4 * char_width, draw_info_line, &layout_state);
        draw_info_line("", 0, &layout_state); // a blank line between paragraphs
    }
    free(text);
}

static void add_panel_button(float x, float y, float width, float height, enum panel_action action, int argument,
                             int second) {
    if (panel_button_count == MAX_PANEL_BUTTONS) return;
    panel_buttons[panel_button_count++] = (struct panel_button){ x, y, width, height, action, argument, second };
}

// A button of the extensions panel; returns the x of the next one.
static float panel_label(float x, float y, const char *text, bool selected, enum panel_action action, int argument,
                         int second) {
    size_t columns = overlay_text_columns(text, strlen(text)) + 1;
    float width = (float)columns * overlay_char_width();
    overlay_rect(x, y, width, overlay_line_height(), selected ? theme->popup_selected : theme->toolbar);
    overlay_text(x + overlay_char_width() / 2, y, text, strlen(text), 0, columns, NULL, theme->text);
    add_panel_button(x, y, width, overlay_line_height(), action, argument, second);
    return x + width + overlay_char_width();
}

// The extensions window of the web editor (src/views/extension-info.js): categories, then per entry a thumbnail,
// "name - author", the description, and buttons to add its load code, open its documentation and run its examples.
static void draw_extensions(void) {
    float left, top, width, height, line_height = overlay_line_height(), char_width = overlay_char_width();
    info_panel(&left, &top, &width, &height);
    overlay_rect(left, top, width, height, theme->help_background);
    panel_button_count = 0;
    char *panel = host.call("__extensionsPanel", "");
    if (!panel) return;
    float x = left + char_width, y = top + line_height / 2;
    size_t entry = 0, row = 0, rows = (size_t)((height - 2 * line_height) / line_height);
    float thumbnail_size = (EXTENSION_ROWS - 0.5f) * line_height,
        text_left = left + char_width + thumbnail_size + char_width;
    size_t text_columns = (size_t)((left + width - text_left) / char_width) - 1;
    char *saveptr;
    for (char *line = strtok_r(panel, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        char *fields[6] = { 0 };
        size_t count = 0;
        for (char *field = line; field && count < 6; count++) {
            fields[count] = field;
            field = strchr(field, '\t');
            if (field) *field++ = '\0';
        }
        if (!strcmp(fields[0], "category") && count >= 3) {
            x = panel_label(x, y, fields[1], fields[2][0] == '1', ACTION_EXTENSION_CATEGORY, (int)entry++, 0);
        } else if (!strcmp(fields[0], "state") && count >= 2) {
            entry = 0;
            const char *state = "";
            if (!strcmp(fields[1], "loading")) state = "loading the list from github...";
            if (!strcmp(fields[1], "failed")) state = "cannot load the list: hydra-native needs the network for it";
            overlay_text(left + char_width, y + 1.5f * line_height, state, strlen(state), 0, text_columns, NULL,
                         theme->info);
            row = 1;
        } else if (!strcmp(fields[0], "entry") && count >= 5) {
            int index = (int)entry++;
            if ((size_t)index < info_scroll) continue;
            if (row + EXTENSION_ROWS > rows) continue;
            float entry_top = y + line_height * (float)(row + 1);
            unsigned texture = count >= 6 && fields[5][0] ? thumbnail_texture(fields[5]) : 0;
            if (texture) overlay_image(left + char_width, entry_top, thumbnail_size, thumbnail_size, texture);
            char title[512];
            snprintf(title, sizeof title, "%s - %s", fields[1], fields[2]);
            overlay_text(text_left, entry_top, title, strlen(title), 0, text_columns, NULL, theme->help_keys);
            overlay_text(text_left, entry_top + line_height, fields[3], strlen(fields[3]), 0, text_columns, NULL,
                         theme->text);
            float button_y = entry_top + 2 * line_height;
            float button_x = panel_label(text_left, button_y, "+", false, ACTION_EXTENSION_ADD, index, 0);
            button_x = panel_label(button_x, button_y, "docs", false, ACTION_EXTENSION_DOCS, index, 0);
            for (int example = 0; example < atoi(fields[4]); example++) {
                char label[24];
                snprintf(label, sizeof label, "%d", example + 1);
                button_x = panel_label(button_x, button_y, label, false, ACTION_EXTENSION_EXAMPLE, index, example);
            }
            row += EXTENSION_ROWS + 1;
        }
    }
    free(panel);
}


// The actions of the extensions panel; js/extensions.js takes the indexes as text.
static void run_extension_action(const struct panel_button *button) {
    char index[32];
    char *result = NULL;
    if (button->action == ACTION_EXTENSION_EXAMPLE)
        snprintf(index, sizeof index, "%d %d", button->argument, button->second);
    else snprintf(index, sizeof index, "%d", button->argument);
    if (button->action == ACTION_EXTENSION_CATEGORY) {
        info_scroll = 0;
        free(host.call("__extensionsPanel", index));
    } else if (button->action == ACTION_EXTENSION_ADD && (result = host.call("__extensionLoadCode", index))) {
        // 'editor: add code to top' of the web editor (CodeMirror 5 version).
        size_t length = strlen(result) + editor.length + 4;
        char *code = malloc(length + 1);
        snprintf(code, length + 1, "\n%s\n\n%s\n", result, editor.text);
        editor_replace_text(&editor, code, strlen(code));
        free(code);
        extensions_visible = false;
    } else if (button->action == ACTION_EXTENSION_DOCS && (result = host.call("__extensionDocumentation", index))) {
        SDL_OpenURL(result);
    } else if (button->action == ACTION_EXTENSION_EXAMPLE && (result = host.call("__extensionExample", index))) {
        editor_replace_text(&editor, result, strlen(result));
        evaluate_range(0, editor.length);
        extensions_visible = false;
    }
    free(result);
}

static bool extensions_click(float x, float y) {
    if (!in_info_panel(x, y)) {
        extensions_visible = false;
        return false;
    }
    size_t button = panel_button_at(x, y);
    if (button < panel_button_count) run_extension_action(&panel_buttons[button]);
    return true;
}

static bool info_click(float x, float y) {
    float left, top, width, height;
    info_panel(&left, &top, &width, &height);
    if (x < left || x > left + width || y < top || y > top + height) {
        info_visible = false;
        return false;
    }
    float line_height = overlay_line_height();
    if (y >= top + line_height / 2 && y <= top + 1.5f * line_height)
        for (size_t i = 0; i < language_button_count; i++)
            if (x >= language_buttons[i].x && x <= language_buttons[i].x + language_buttons[i].width)
                free(host.call("__setLanguage", language_buttons[i].code));
    return true;
}

// ---------------------------------------------------------------- settings panel

#define MAX_FONT_CHOICES 256

// The monospace fonts of fontconfig, one file per family (a regular style when there is one).
static struct {
    char family[128];
    char file[512];
    bool regular;
} font_choices[MAX_FONT_CHOICES];
static size_t font_choice_count;
static bool font_choices_loaded;

static void load_font_choices(void) {
    font_choices_loaded = true;
    FILE *pipe = popen("fc-list -f '%{family[0]}\t%{style[0]}\t%{file}\n' :spacing=mono", "r");
    if (!pipe) return;
    char line[1024];
    while (fgets(line, sizeof line, pipe)) {
        line[strcspn(line, "\n")] = '\0';
        char *style = strchr(line, '\t'), *file = style ? strchr(style + 1, '\t') : NULL;
        if (!file) continue;
        *style++ = '\0';
        *file++ = '\0';
        const char *extension = strrchr(file, '.');
        if (strstr(line, "Emoji")) continue; // color glyphs only, no code font
        if (!extension ||
            (strcasecmp(extension, ".ttf") && strcasecmp(extension, ".otf") && strcasecmp(extension, ".ttc"))) continue;
        bool regular = !strcmp(style, "Regular") || !strcmp(style, "Book") || !strcmp(style, "Roman");
        size_t i = 0;
        while (i < font_choice_count && strcmp(font_choices[i].family, line)) i++;
        if (i == MAX_FONT_CHOICES || (i < font_choice_count && (font_choices[i].regular || !regular))) continue;
        snprintf(font_choices[i].family, sizeof font_choices[i].family, "%.127s", line);
        snprintf(font_choices[i].file, sizeof font_choices[i].file, "%.511s", file);
        font_choices[i].regular = regular;
        if (i == font_choice_count) font_choice_count++;
    }
    pclose(pipe);
    for (size_t i = 1; i < font_choice_count; i++)
        for (size_t k = i; k > 0 && strcasecmp(font_choices[k - 1].family, font_choices[k].family) > 0; k--) {
            typeof(font_choices[0]) swap = font_choices[k];
            font_choices[k] = font_choices[k - 1];
            font_choices[k - 1] = swap;
        }
}

// The audio inputs and the languages of the settings menu, read when it opens.
#define MAX_AUDIO_DEVICES 32
static struct audio_device audio_choices[MAX_AUDIO_DEVICES];
static int audio_choice_count;
static struct {
    char code[16], name[64];
} language_choices[MAX_LANGUAGES];
static size_t language_choice_count;
static char selected_language[16];

static void load_settings_lists(void) {
    audio_choice_count = host.audio_devices ? host.audio_devices(audio_choices, MAX_AUDIO_DEVICES) : 0;
    language_choice_count = 0;
    selected_language[0] = '\0';
    char *languages = host.call("__languages", "");
    for (char *line = languages ? strtok(languages, "\n") : NULL; line && language_choice_count < MAX_LANGUAGES;
         line = strtok(NULL, "\n")) {
        char *name = strchr(line, '\t'), *selected = name ? strchr(name + 1, '\t') : NULL;
        if (!selected) continue;
        *name++ = '\0';
        *selected++ = '\0';
        snprintf(language_choices[language_choice_count].code, sizeof language_choices[0].code, "%s", line);
        snprintf(language_choices[language_choice_count].name, sizeof language_choices[0].name, "%s", name);
        if (*selected == '1') snprintf(selected_language, sizeof selected_language, "%s", line);
        language_choice_count++;
    }
    free(languages);
}

// The items of a settings row flow from the value column and wrap at the right edge of the panel, so that long
// texts (a custom server URL, the patch bay ids, font names) stay inside it; a text longer than a row is cut.
#define LABEL_COLUMNS 22 // the names of the settings rows; a sub-row starts with a tree mark

// The heights of the rows of the settings menu, in font sizes, as in a dropdown: a top-level row, and a sub-row of
// a group or a wrapped line of items. The box of an item, and its hit area, fill the height of its row.
#define TOP_ROW_HEIGHT 1.2f
#define SUB_ROW_HEIGHT 1.0f

struct panel_flow {
    float x, y, start, right;
    float height;
    float top, bottom; // the panel shows only the rows between these; the wheel scrolls the others in
};

static bool flow_visible(const struct panel_flow *flow) {
    return flow->y >= flow->top && flow->y + flow->height <= flow->bottom;
}

// The text line centered in the row; it may reach a little beyond a row that is lower than a line.
static float flow_text_y(const struct panel_flow *flow) { return flow->y + (flow->height - overlay_line_height()) / 2; }

static void next_line(struct panel_flow *flow, float font_sizes) {
    flow->y += flow->height;
    flow->height = font_sizes * overlay_font_size();
}

// Text in `columns` columns; a longer text shows its end after "...", which tells apart long names that start
// alike (audio devices, server URLs).
static void flow_text_in(const struct panel_flow *flow, float x, const char *text, size_t columns, uint32_t color) {
    size_t length = strlen(text), text_columns = overlay_text_columns(text, length);
    if (text_columns <= columns || columns <= 3) {
        overlay_text(x, flow_text_y(flow), text, length, 0, columns, NULL, color);
        return;
    }
    overlay_text(x, flow_text_y(flow), "...", 3, 0, 3, NULL, color);
    overlay_text(x + 3 * overlay_char_width(), flow_text_y(flow), text, length, text_columns - (columns - 3),
                 columns - 3,
                 NULL, color);
}

static float flow_place(struct panel_flow *flow, const char *text, size_t *columns) {
    float char_width = overlay_char_width();
    size_t fit = (size_t)((flow->right - flow->start) / char_width);
    *columns = overlay_text_columns(text, strlen(text)) + 1;
    if (*columns > fit) *columns = fit;
    float width = (float)*columns * char_width;
    if (flow->x > flow->start && flow->x + width > flow->right) {
        flow->x = flow->start;
        next_line(flow, SUB_ROW_HEIGHT);
    }
    float x = flow->x;
    flow->x += width + char_width;
    return x;
}

static void flow_button(struct panel_flow *flow, const char *text, uint32_t color, bool selected,
                        enum panel_action action, int argument, int second) {
    size_t columns;
    float x = flow_place(flow, text, &columns), width = (float)columns * overlay_char_width();
    if (!flow_visible(flow)) return;
    overlay_rect(x, flow->y, width, flow->height, selected ? theme->popup_selected : theme->toolbar);
    flow_text_in(flow, x + overlay_char_width() / 2, text, columns - 1, color);
    add_panel_button(x, flow->y, width, flow->height, action, argument, second);
}

static void flow_label(struct panel_flow *flow, const char *text, bool selected, enum panel_action action,
                       int argument) {
    flow_button(flow, text, theme->text, selected, action, argument, 0);
}

// The two choices of a switch.
static void flow_switch(struct panel_flow *flow, const char *on, const char *off, bool value,
                        enum panel_action action) {
    flow_label(flow, on, value, action, 1);
    flow_label(flow, off, !value, action, 0);
}

// A number of the settings menu between its - and + buttons: a button that opens its text field, or the open field
// with the typed text.
static void flow_number(struct panel_flow *flow, enum number_kind kind) {
    flow_button(flow, "-", theme->text, false, ACTION_STEP_NUMBER, (int)kind, -number_specs[kind].button_step);
    char text[MAX_NUMBER_TEXT + 2];
    bool editing = number_field.open && number_field.kind == kind;
    if (editing) snprintf(text, sizeof text, "%s%s", number_field.text, number_field.selected ? "" : "|");
    else format_number(kind, number_value(kind), text, sizeof text);
    size_t columns;
    float x = flow_place(flow, text, &columns), width = (float)columns * overlay_char_width();
    float text_width = (float)overlay_text_columns(text, strlen(text)) * overlay_char_width();
    if (flow_visible(flow)) {
        overlay_rect(x, flow->y, width, flow->height, editing ? theme->popup_background : theme->toolbar);
        if (editing && number_field.selected)
            overlay_rect(x + overlay_char_width() / 2, flow->y, text_width, flow->height, theme->selection);
        overlay_text(x + overlay_char_width() / 2, flow_text_y(flow), text, strlen(text), 0, columns - 1, NULL,
                     editing && number_field.invalid ? theme->error : theme->text);
        add_panel_button(x, flow->y, width, flow->height, ACTION_EDIT_NUMBER, (int)kind, 0);
    }
    flow_button(flow, "+", theme->text, false, ACTION_STEP_NUMBER, (int)kind, number_specs[kind].button_step);
}

static void flow_text(struct panel_flow *flow, const char *text, uint32_t color) {
    size_t columns;
    float x = flow_place(flow, text, &columns);
    if (flow_visible(flow)) flow_text_in(flow, x, text, columns, color);
}

// Starts the next row with its name in the label column. A sub-row of a group has a name that starts with a space
// and a tree mark.
static void flow_row(struct panel_flow *flow, float label_x, const char *label) {
    flow->x = flow->start;
    next_line(flow, label[0] == ' ' ? SUB_ROW_HEIGHT : TOP_ROW_HEIGHT);
    if (flow_visible(flow))
        overlay_text(label_x, flow_text_y(flow), label, strlen(label), 0, LABEL_COLUMNS, NULL, theme->help_keys);
}

// A choice that cannot change now (such as "reload on file change" without an open file): no button, dim text.
static void flow_label_disabled(struct panel_flow *flow, const char *text, bool selected) {
    size_t columns;
    float x = flow_place(flow, text, &columns), width = (float)columns * overlay_char_width();
    if (!flow_visible(flow)) return;
    if (selected) overlay_rect(x, flow->y, width, flow->height, theme->toolbar);
    flow_text_in(flow, x + overlay_char_width() / 2, text, columns - 1, theme->ghost);
}

#define NO_FILE_PLACEHOLDER "choose a file...              " // as wide as a short path, so the row is easy to hit

static size_t settings_scroll_limit = (size_t)-1; // the rows below the panel in the last frame

// The settings menu; each choice applies at once and goes into the settings file.
static void draw_settings(void) {
    float left, top, width, height, line_height = overlay_line_height(), char_width = overlay_char_width();
    info_panel(&left, &top, &width, &height);
    overlay_rect(left, top, width, height, theme->help_background);
    panel_button_count = 0;
    if (!font_choices_loaded) load_font_choices();
    float label_x = left + char_width;
    if (info_scroll > settings_scroll_limit) info_scroll = settings_scroll_limit;
    float first_y = top + line_height / 2 - (float)info_scroll * line_height;
    struct panel_flow flow = { 0, first_y, left + (LABEL_COLUMNS + 1) * char_width, left + width - char_width, 0, top,
                               top + height };
    // The open sketch file, or a dim placeholder; a click chooses another one. Its reload is dim while no file is
    // open.
    flow_row(&flow, label_x, "file");
    if (file_path) flow_label(&flow, file_path, false, ACTION_OPEN_FILE, 0);
    else flow_button(&flow, NO_FILE_PLACEHOLDER, theme->ghost, false, ACTION_OPEN_FILE, 0, 0);
    flow_row(&flow, label_x, " \xE2\x94\x94 reload on change");
    if (file_path) {
        flow_switch(&flow, "on", "off", settings.watch, ACTION_WATCH);
    } else {
        flow_label_disabled(&flow, "on", settings.watch);
        flow_label_disabled(&flow, "off", !settings.watch);
    }
    flow_row(&flow, label_x, "theme");
    for (size_t i = 0; i < theme_count; i++)
        flow_label(&flow, themes[i].name, theme == &themes[i], ACTION_THEME, (int)i);
    flow_row(&flow, label_x, "autocomplete");
    flow_label(&flow, "list", settings.autocomplete_style == AUTOCOMPLETE_LIST, ACTION_AUTOCOMPLETE_STYLE,
               AUTOCOMPLETE_LIST);
    flow_label(&flow, "ghost text", settings.autocomplete_style == AUTOCOMPLETE_GHOST, ACTION_AUTOCOMPLETE_STYLE,
               AUTOCOMPLETE_GHOST);
    flow_row(&flow, label_x, " \xE2\x94\x9C list opacity");
    flow_number(&flow, NUMBER_COMPLETION_OPACITY);
    flow_row(&flow, label_x, " \xE2\x94\x9C trigger");
    flow_label(&flow, "Ctrl+Space", settings.autocomplete_trigger == AUTOCOMPLETE_MANUAL, ACTION_AUTOCOMPLETE_TRIGGER,
               AUTOCOMPLETE_MANUAL);
    flow_label(&flow, "while typing", settings.autocomplete_trigger == AUTOCOMPLETE_TYPING, ACTION_AUTOCOMPLETE_TRIGGER,
               AUTOCOMPLETE_TYPING);
    flow_row(&flow, label_x, " \xE2\x94\x94 signatures");
    flow_label(&flow, "Ctrl+Shift+Space", settings.signature_help == SIGNATURE_HELP_MANUAL, ACTION_SIGNATURE_HELP,
               SIGNATURE_HELP_MANUAL);
    flow_label(&flow, "on (", settings.signature_help == SIGNATURE_HELP_PAREN, ACTION_SIGNATURE_HELP,
               SIGNATURE_HELP_PAREN);
    flow_row(&flow, label_x, "code display");
    flow_row(&flow, label_x, " \xE2\x94\x9C background opacity");
    flow_number(&flow, NUMBER_OPACITY);
    flow_row(&flow, label_x, " \xE2\x94\x9C background blend");
    flow_label(&flow, "normal", settings.code_background_blend == CODE_BLEND_NORMAL, ACTION_BACKGROUND_BLEND,
               CODE_BLEND_NORMAL);
    flow_label(&flow, "difference", settings.code_background_blend == CODE_BLEND_DIFFERENCE, ACTION_BACKGROUND_BLEND,
               CODE_BLEND_DIFFERENCE);
    flow_row(&flow, label_x, " \xE2\x94\x94 text blend");
    flow_label(&flow, "normal", settings.code_text_blend == CODE_BLEND_NORMAL, ACTION_TEXT_BLEND, CODE_BLEND_NORMAL);
    flow_label(&flow, "difference", settings.code_text_blend == CODE_BLEND_DIFFERENCE, ACTION_TEXT_BLEND,
               CODE_BLEND_DIFFERENCE);
    flow_row(&flow, label_x, "code at start");
    flow_switch(&flow, "shown", "hidden", settings.code_visible, ACTION_CODE_AT_START);
    flow_text(&flow, "(next start)", theme->help_keys);
    flow_row(&flow, label_x, "whitespace");
    for (int i = WHITESPACE_NONE; i <= WHITESPACE_ALL; i++)
        flow_label(&flow, settings_whitespace_name((enum render_whitespace)i),
                   settings.render_whitespace == (enum render_whitespace)i, ACTION_WHITESPACE, i);
    flow_row(&flow, label_x, "line wrap");
    flow_switch(&flow, "on", "off", wrap, ACTION_WRAP);
    flow_row(&flow, label_x, "server");
    const char *server = settings.gallery_server;
    flow_label(&flow, "off", !server[0], ACTION_SERVER_OFF, 0);
    flow_label(&flow, "hydra.ojack.xyz", !strcmp(server, WEB_EDITOR_SERVER), ACTION_SERVER_WEB_EDITOR, 0);
    bool custom = server[0] && strcmp(server, WEB_EDITOR_SERVER);
    flow_label(&flow, custom ? server : "custom", custom, ACTION_SERVER_CUSTOM, 0);
    char *stream_id = patch_bay_stream_id();
    if (stream_id[0]) { // while the patch bay is connected: the id for s0.initStream, with a button that copies it
        char label[160];
        snprintf(label, sizeof label, "stream id %s", stream_id);
        flow_text(&flow, label, theme->help_keys);
        flow_label(&flow, "copy", false, ACTION_COPY_STREAM_ID, 0);
    }
    free(stream_id);
    flow_row(&flow, label_x, "window");
    bool fullscreen = host.is_fullscreen();
    flow_label(&flow, "fullscreen", fullscreen, ACTION_FULLSCREEN, !fullscreen);
    flow_switch(&flow, "vsync on", "vsync off", settings.vsync, ACTION_VSYNC);
    flow_row(&flow, label_x, "audio input");
    const char *audio = settings.audio_source;
    flow_label(&flow, "default", !audio[0], ACTION_AUDIO_DEFAULT, 0);
    for (int i = 0; i < audio_choice_count; i++)
        flow_label(&flow, audio_choices[i].label, !strcmp(audio, audio_choices[i].name), ACTION_AUDIO, i);
    flow_label(&flow, "none", !strcmp(audio, AUDIO_NONE), ACTION_AUDIO_NONE, 0);
    flow_row(&flow, label_x, " \xE2\x94\x94 input gain");
    flow_number(&flow, NUMBER_AUDIO_GAIN);
    flow_row(&flow, label_x, "language");
    for (size_t i = 0; i < language_choice_count; i++)
        flow_label(&flow, language_choices[i].name, !strcmp(selected_language, language_choices[i].code),
                   ACTION_LANGUAGE,
                   (int)i);
    flow_label(&flow, settings.l10n_url[0] ? settings.l10n_url : "translation URL...", false, ACTION_L10N_URL, 0);
    flow_row(&flow, label_x, "font size");
    flow_number(&flow, NUMBER_FONT_SIZE);
    flow_row(&flow, label_x, "font");
    flow_label(&flow, "file...", false, ACTION_FONT_FILE, 0);
    for (size_t i = 0; i < font_choice_count; i++) {
        flow.x = flow.start;
        next_line(&flow, SUB_ROW_HEIGHT);
        flow_label(&flow, font_choices[i].family, !strcmp(font_choices[i].file, settings.font), ACTION_FONT, (int)i);
    }
    // The wheel scrolls until the last row is in the panel.
    float below = flow.y + flow.height + line_height / 2 - (top + height);
    settings_scroll_limit = info_scroll + (below > 0 ? (size_t)ceilf(below / line_height) : 0);
}

// Opens `kind` with `text` selected, so that typing replaces it.
static void open_prompt_with(enum prompt_kind kind, const char *text) {
    settings_visible = false;
    open_prompt(kind);
    snprintf(prompt.query, sizeof prompt.query, "%.*s", (int)sizeof prompt.query - 1, text);
    prompt.query_selected = prompt.query[0] != '\0';
}

static void set_audio_source(const char *name) {
    snprintf(settings.audio_source, sizeof settings.audio_source, "%s", name);
    host.set_audio(name);
    save_settings();
}

static void run_settings_action(const struct panel_button *button) {
    int argument = button->argument;
    size_t index = (size_t)argument;
    if (button->action == ACTION_EDIT_NUMBER) {
        if (!number_field.open || number_field.kind != (enum number_kind)argument)
            open_number_field((enum number_kind)argument);
        return;
    }
    number_field.open = false; // a click on another button leaves the field without a change
    switch (button->action) {
    case ACTION_EXTENSION_CATEGORY:
    case ACTION_EXTENSION_ADD:
    case ACTION_EXTENSION_DOCS:
    case ACTION_EXTENSION_EXAMPLE:
    case ACTION_EDIT_NUMBER: return;
    case ACTION_OPEN_FILE:
        settings_visible = false;
        host.choose_file(false);
        return;
    case ACTION_THEME: set_theme(&themes[index]); return;
    case ACTION_AUTOCOMPLETE_STYLE: set_autocomplete((enum autocomplete_style)argument,
                                                     settings.autocomplete_trigger); return;
    case ACTION_AUTOCOMPLETE_TRIGGER: set_autocomplete(settings.autocomplete_style,
                                                       (enum autocomplete_trigger)argument); return;
    case ACTION_SIGNATURE_HELP: set_signature_help((enum signature_help)argument); return;
    case ACTION_STEP_NUMBER: set_number((enum number_kind)argument,
                                        number_value((enum number_kind)argument) + button->second); return;
    case ACTION_SERVER_OFF: set_server(""); return;
    case ACTION_SERVER_WEB_EDITOR: set_server(WEB_EDITOR_SERVER); return;
    case ACTION_SERVER_CUSTOM:
        settings_visible = false;
        run_command(COMMAND_SET_SERVER, false);
        return;
    case ACTION_COPY_STREAM_ID: copy_patch_bay_id(); return;
    case ACTION_AUDIO_DEFAULT: set_audio_source(""); return;
    case ACTION_AUDIO_NONE: set_audio_source(AUDIO_NONE); return;
    case ACTION_AUDIO: set_audio_source(audio_choices[index].name); return;
    case ACTION_L10N_URL: open_prompt_with(PROMPT_L10N_URL, settings.l10n_url); return;
    case ACTION_FONT_FILE: open_prompt_with(PROMPT_FONT_PATH, settings.font); return;
    case ACTION_FONT: set_font(font_choices[index].file); return;
    // The other actions change one setting, which is saved below.
    case ACTION_WATCH: settings.watch = argument; break;
    case ACTION_BACKGROUND_BLEND: settings.code_background_blend = (enum code_blend)argument; break;
    case ACTION_TEXT_BLEND: settings.code_text_blend = (enum code_blend)argument; break;
    case ACTION_CODE_AT_START:
        settings.code_visible = argument;
        ui_show_message("code at start: applies at the next start", false);
        break;
    case ACTION_WHITESPACE: settings.render_whitespace = (enum render_whitespace)argument; break;
    case ACTION_WRAP:
        wrap = settings.word_wrap = argument;
        layout.valid = false;
        break;
    case ACTION_FULLSCREEN:
        if (host.is_fullscreen() != (bool)argument) host.toggle_fullscreen();
        settings.fullscreen = argument;
        break;
    case ACTION_VSYNC:
        settings.vsync = argument;
        host.set_vsync(settings.vsync);
        break;
    case ACTION_LANGUAGE:
        free(host.call("__setLanguage", language_choices[index].code));
        snprintf(selected_language, sizeof selected_language, "%s", language_choices[index].code);
        snprintf(settings.language, sizeof settings.language, "%s", selected_language);
        break;
    }
    save_settings();
}

// The buttons take a click first; a click outside the panel and its buttons closes it.
static bool settings_click(float x, float y) {
    size_t button = panel_button_at(x, y);
    if (button < panel_button_count) {
        struct panel_button clicked = panel_buttons[button]; // the action may draw the panel again
        run_settings_action(&clicked);
        return true;
    }
    if (!in_info_panel(x, y)) settings_visible = false;
    return settings_visible;
}

// ---------------------------------------------------------------- help

static void draw_help(int width, int height) {
    overlay_rect(0, 0, (float)width, (float)height, theme->help_background);
    float line_height = overlay_line_height(), char_width = overlay_char_width();
    const char *title =
        "hydra-native: live coding with hydra-synth. F1 or Escape closes this help. [name]: for \"keybindings\" in "
            "settings.json";
    overlay_text(MARGIN, MARGIN, title, strlen(title), 0, visible_columns(), NULL, theme->text);
    size_t rows = (size_t)(((float)height - 3 * MARGIN - line_height) / line_height);
    if (rows == 0) rows = 1;
    size_t entries = COMMAND_COUNT - 1, columns = (entries + rows - 1) / rows;
    size_t column_width = visible_columns() / columns, keys_width = column_width * 2 / 5;
    for (size_t entry = 0; entry < entries; entry++) {
        enum command command = (enum command)(entry + 1);
        char keys[256];
        command_keys(command, keys, sizeof keys);
        if (!keys[0]) snprintf(keys, sizeof keys, "(palette)");
        float x = MARGIN + (float)((entry / rows) * column_width) * char_width;
        float y = MARGIN + line_height * (1.5f + (float)(entry % rows));
        overlay_text(x, y, keys, strlen(keys), 0, keys_width - 1, NULL, theme->help_keys);
        // The description with the command name for the "keybindings" of the settings file.
        char description[160];
        snprintf(description, sizeof description, "%s [%s]", command_description(command), command_name(command));
        overlay_text(x + (float)keys_width * char_width, y, description, strlen(description), 0,
                     column_width - keys_width - 1, NULL, theme->text);
    }
}

// ---------------------------------------------------------------- mouse

void ui_mouse_button(float x, float y, bool down, int clicks, SDL_Keymod modifiers) {
    if (!visible) return;
    if (!down) {
        mouse.dragging = false;
        return;
    }
    if (help_visible) {
        help_visible = false;
        return;
    }
    size_t entry = reset_menu_entry_at(x, y);
    if (entry < RESET_MENU_COUNT) {
        run_command(reset_menu[entry].command, false);
        return;
    }
    if (toolbar_click(x, y, modifiers)) return;
    reset_menu_open = false;
    if (info_visible && info_click(x, y)) return;
    if (extensions_visible && extensions_click(x, y)) return;
    if (settings_visible && settings_click(x, y)) return;
    close_completion();
    size_t offset = offset_at_point(x, y);
    mouse.dragging = true;
    mouse.column = (modifiers & SDL_KMOD_ALT) && (modifiers & SDL_KMOD_SHIFT);
    if (mouse.column) {
        size_t line, row, column;
        point_to_position(x, y, &line, &row, &column);
        mouse.anchor_line = line;
        mouse.anchor_column = editor_column_of(&editor, offset);
    } else if (modifiers & SDL_KMOD_ALT) {
        editor_add_cursor(&editor, offset); // Alt+click adds a cursor, as in VS Code
        mouse.dragging = false;
    } else if (clicks == 2) {
        editor_collapse(&editor);
        editor_select_word_at(&editor, offset);
    } else if (clicks >= 3) {
        editor_collapse(&editor);
        editor_set_cursor(&editor, offset, false);
        editor_select_lines(&editor);
    } else {
        if (!(modifiers & SDL_KMOD_SHIFT)) editor_collapse(&editor);
        editor_set_cursor(&editor, offset, modifiers & SDL_KMOD_SHIFT);
    }
    last_activity = now;
}

void ui_mouse_motion(float x, float y) {
    pointer_x = x;
    pointer_y = y;
    if (!visible || !mouse.dragging) return;
    size_t offset = offset_at_point(x, y);
    if (mouse.column) {
        editor_column_select(&editor, mouse.anchor_line, mouse.anchor_column, editor_line_of(&editor, offset),
                             editor_column_of(&editor, offset));
    } else {
        editor_set_cursor(&editor, offset, true);
    }
    reveal_cursor();
}

// The wheel over a number of the settings menu (or in its open field) steps it; Shift steps by ten.
static bool wheel_number(int lines) {
    int steps = lines * ((SDL_GetModState() & SDL_KMOD_SHIFT) ? LARGE_STEPS : 1);
    if (number_field.open) {
        step_number(number_field.kind, steps);
        return true;
    }
    size_t button = panel_button_at(pointer_x, pointer_y);
    if (button == panel_button_count || panel_buttons[button].action != ACTION_EDIT_NUMBER) return false;
    step_number((enum number_kind)panel_buttons[button].argument, steps);
    return true;
}

void ui_wheel(int lines) {
    if (!visible) return;
    if (settings_visible && wheel_number(lines)) return;
    if (info_visible || extensions_visible || settings_visible) {
        long target = (long)info_scroll - (long)lines * WHEEL_ROWS;
        info_scroll = target < 0 ? 0 : (size_t)target;
        return;
    }
    update_layout();
    long target = (long)scroll_row - (long)lines * WHEEL_ROWS;
    long last = (long)layout.total_rows - 1;
    scroll_row = (size_t)(target < 0 ? 0 : target > last ? last : target);
}

// ---------------------------------------------------------------- highlighting

struct highlight_state {
    bool block_comment;
    bool template_string;
    int bracket_depth;
};

static bool is_identifier_start(unsigned char c) { return isalpha(c) || c == '_' || c == '$'; }

static bool is_keyword(const char *word, size_t length) {
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if (strlen(keywords[i]) == length && memcmp(keywords[i], word, length) == 0) return true;
    return false;
}

// Colors one line (one color per byte, when `out` is not NULL) and carries the state across lines.
static void highlight_line(struct highlight_state *state, const char *text, size_t length, uint32_t *out) {
    size_t i = 0;
    while (i < length) {
        size_t start = i;
        uint32_t color = theme->text;
        unsigned char c = (unsigned char)text[i];
        char next = i + 1 < length ? text[i + 1] : '\0';
        if (state->block_comment) {
            color = theme->comment;
            while (i < length && !(text[i] == '*' && i + 1 < length && text[i + 1] == '/')) i++;
            if (i < length) { i += 2; state->block_comment = false; }
        } else if (state->template_string || c == '\'' || c == '"' || c == '`') {
            char quote = state->template_string ? '`' : (char)c;
            color = theme->string;
            if (!state->template_string) i++;
            state->template_string = false;
            while (i < length && text[i] != quote) i += text[i] == '\\' ? 2 : 1;
            if (i < length) i++;
            else if (quote == '`') state->template_string = true;
            if (i > length) i = length;
        } else if (c == '/' && next == '/') {
            color = theme->comment;
            i = length;
        } else if (c == '/' && next == '*') {
            state->block_comment = true;
            i += 2;
            if (out) out[start] = out[start + 1] = theme->comment;
            continue;
        } else if (isdigit(c) || (c == '.' && isdigit((unsigned char)next))) {
            color = theme->number;
            while (i < length && (isalnum((unsigned char)text[i]) || text[i] == '.')) i++;
        } else if (is_identifier_start(c)) {
            while (i < length && is_identifier_char((unsigned char)text[i])) i++;
            size_t after = i;
            while (after < length && text[after] == ' ') after++;
            if (is_keyword(text + start, i - start)) color = theme->keyword;
            else if (after < length && text[after] == '(') color = theme->function;
        } else if (strchr("([{", c)) {
            color = theme->brackets[state->bracket_depth++ % 3];
            i++;
        } else if (strchr(")]}", c)) {
            if (state->bracket_depth > 0) state->bracket_depth--;
            color = theme->brackets[state->bracket_depth % 3];
            i++;
        } else {
            i++;
        }
        if (out)
            for (size_t k = start; k < i && k < length; k++) out[k] = color;
    }
}

// ---------------------------------------------------------------- drawing

_Static_assert(OVERLAY_TAB_WIDTH == EDITOR_TAB_WIDTH, "the overlay and the editor must agree on tabs");

static size_t columns_between(size_t start, size_t end) { return editor_columns_between(&editor, start, end); }

// A rectangle over the columns [from, to) of a screen row, clipped to the visible columns.
static void column_rect(float y, size_t from, size_t to, uint32_t color) {
    size_t limit = scroll_column + visible_columns();
    if (to <= scroll_column || from >= limit) return;
    if (from < scroll_column) from = scroll_column;
    if (to > limit) to = limit;
    float x = MARGIN + (float)(from - scroll_column) * overlay_char_width();
    overlay_rect(x, y, (float)(to - from) * overlay_char_width(), overlay_line_height(), color);
}

static size_t text_columns(const char *text) { return overlay_text_columns(text, strlen(text)); }

// The bar at the bottom; the columns [selected_from, selected_to) of the text get the selection color.
static void draw_bar(int width, int height, const char *text, uint32_t color, size_t selected_from,
                     size_t selected_to) {
    float y = (float)height - bar_height();
    overlay_rect(0, y, (float)width, bar_height(), theme->bar_background);
    if (selected_to > selected_from)
        overlay_rect(MARGIN + (float)selected_from * overlay_char_width(), y + BAR_PADDING,
                     (float)(selected_to - selected_from) * overlay_char_width(), overlay_line_height(),
                     theme->selection);
    overlay_text(MARGIN, y + BAR_PADDING, text, strlen(text), 0, visible_columns(), NULL, color);
}

// editor.renderWhitespace of VS Code: whether the space or tab at `offset` shows.
static bool whitespace_shown(size_t line_start, size_t line_end, size_t offset, const struct selection *selections,
                             size_t selection_count) {
    switch (settings.render_whitespace) {
    case WHITESPACE_NONE: return false;
    case WHITESPACE_ALL: return true;
    case WHITESPACE_TRAILING: {
        size_t end = offset;
        while (end < line_end && (editor.text[end] == ' ' || editor.text[end] == '\t')) end++;
        return end == line_end;
    }
    case WHITESPACE_SELECTION:
        for (size_t i = 0; i < selection_count; i++) {
            if (offset >= selection_start(&selections[i]) && offset < selection_end(&selections[i])) return true;
        }
        return false;
    case WHITESPACE_BOUNDARY: // all but a single space between two words
        if (editor.text[offset] == '\t') return true;
        return (offset > line_start && (editor.text[offset - 1] == ' ' || editor.text[offset - 1] == '\t')) ||
               (offset + 1 < line_end && (editor.text[offset + 1] == ' ' || editor.text[offset + 1] == '\t')) ||
               offset == line_start || offset + 1 >= line_end;
    }
    return false;
}

static void draw_whitespace(size_t column, float y, bool tab) {
    if (column < scroll_column || column >= scroll_column + visible_columns()) return;
    float char_width = overlay_char_width(), x = MARGIN + (float)(column - scroll_column) * char_width;
    if (tab) {
        overlay_text(x, y, "\xE2\x86\x92", 3, 0, 1, NULL, theme->whitespace); // U+2192
        return;
    }
    float size = char_width / 6 < 2 ? 2 : char_width / 6;
    overlay_rect(x + (char_width - size) / 2, y + (overlay_line_height() - size) / 2, size, size, theme->whitespace);
}

// Draws one visual row: the bytes [start, end) of `line`, shown from column `indent` on.
static void draw_row(size_t line, size_t row_start, size_t row_end, size_t indent, float y,
                     const struct selection *selections, size_t selection_count, long bracket, long bracket_self) {
    size_t line_start = editor_line_start(&editor, line);
    size_t row_columns = columns_between(row_start, row_end);
    bool last_row = row_end == editor_line_end(&editor, line);
    // The box covers the characters from the first to the last one that is not a space or a tab.
    size_t text_start = row_start, text_end = row_end;
    while (text_start < text_end && (editor.text[text_start] == ' ' || editor.text[text_start] == '\t')) text_start++;
    while (text_end > text_start && (editor.text[text_end - 1] == ' ' || editor.text[text_end - 1] == '\t')) text_end--;
    size_t box_start = indent + columns_between(row_start, text_start), box_end = indent +
        columns_between(row_start, text_end);
    size_t limit = scroll_column + visible_columns();
    size_t from = box_start > scroll_column ? box_start : scroll_column, to = box_end < limit ? box_end : limit;
    overlay_set_layer(OVERLAY_CODE_BACKGROUND);
    if (to > from) {
        float x = MARGIN + (float)(from - scroll_column) * overlay_char_width();
        overlay_rect(x - TEXT_PADDING, y, (float)(to - from) * overlay_char_width() + 2 * TEXT_PADDING,
                     overlay_line_height(),
                     code_background_color());
    }
    overlay_set_layer(OVERLAY_CODE_MARKS);
    if (now < flash_until && flash_start != flash_end && flash_start <= row_end && flash_end >= row_start)
        column_rect(y, indent, indent + (row_columns > 0 ? row_columns : 1), theme->flash);
    for (size_t i = 0; i < selection_count; i++) {
        size_t start = selection_start(&selections[i]), end = selection_end(&selections[i]);
        if (start == end || start > row_end || end <= row_start || (start == row_end && !last_row)) continue;
        size_t from = indent + (start > row_start ? columns_between(row_start, start) : 0);
        size_t to = indent + (end > row_end ? row_columns + (last_row ? 1 : 0) : columns_between(row_start, end));
        column_rect(y, from, to, theme->selection);
    }
    for (size_t i = 0; i < match_count; i++) {
        size_t start = matches[i].start, end = matches[i].end;
        if (start >= row_end || end <= row_start) continue;
        size_t from = indent + (start > row_start ? columns_between(row_start, start) : 0);
        size_t to = indent + (end > row_end ? row_columns : columns_between(row_start, end));
        column_rect(y, from, to, i == active_match ? theme->match_active : theme->match);
    }
    long positions[2] = { bracket, bracket_self };
    for (int k = 0; k < 2; k++) {
        if (bracket < 0 || positions[k] < (long)row_start || positions[k] >= (long)row_end) continue;
        size_t column = indent + columns_between(row_start, (size_t)positions[k]);
        column_rect(y, column, column + 1, theme->bracket);
    }
    if (settings.render_whitespace != WHITESPACE_NONE) {
        size_t line_end = editor_line_end(&editor, line), column = indent;
        for (size_t i = row_start; i < row_end;) {
            size_t next = editor_cluster_end(&editor, i);
            char c = editor.text[i];
            if ((c == ' ' || c == '\t') && whitespace_shown(line_start, line_end, i, selections, selection_count))
                draw_whitespace(column, y, c == '\t');
            column += c == '\t' ? EDITOR_TAB_WIDTH - (column - indent) % EDITOR_TAB_WIDTH
                                 : (size_t)unicode_cluster_columns((const unsigned char *)editor.text, i, next);
            i = next;
        }
    }
    size_t skip = scroll_column > indent ? scroll_column - indent : 0;
    size_t shift = indent > scroll_column ? indent - scroll_column : 0;
    size_t columns = visible_columns() > shift ? visible_columns() - shift : 0;
    overlay_set_layer(OVERLAY_CODE_TEXT);
    overlay_text(MARGIN + (float)shift * overlay_char_width(), y, editor.text + row_start, row_end - row_start, skip,
                 columns, colors + (row_start - line_start), theme->text);
    overlay_set_layer(OVERLAY_TOP);
}

static void draw_code(void) {
    update_layout();
    float line_height = overlay_line_height();
    size_t rows = visible_rows();
    if (scroll_row >= layout.total_rows) scroll_row = layout.total_rows - 1;
    size_t first_line = line_of_row(scroll_row);
    struct highlight_state state = { 0 };
    for (size_t line = 0; line < first_line; line++) {
        size_t start = editor_line_start(&editor, line);
        highlight_line(&state, editor.text + start, editor_line_end(&editor, line) - start, NULL);
    }
    struct selection *selections;
    size_t selection_count = editor_selections(&editor, &selections);
    long bracket = editor_has_selection(&editor) ? -1 : editor_matching_bracket(&editor, editor.cursor);
    long bracket_self = -1;
    if (bracket >= 0) {
        bool at_cursor = editor.cursor < editor.length && strchr("()[]{}", editor.text[editor.cursor]);
        bracket_self = at_cursor ? (long)editor.cursor : (long)editor.cursor - 1;
    }
    size_t starts[MAX_ROWS_PER_LINE];
    size_t screen_row = 0;
    for (size_t line = first_line; line < editor.line_count && screen_row < rows; line++) {
        size_t line_start = editor_line_start(&editor, line), line_end = editor_line_end(&editor, line);
        if (line_end - line_start > colors_capacity) {
            colors_capacity = (line_end - line_start) * 2;
            colors = realloc(colors, colors_capacity * sizeof *colors);
        }
        highlight_line(&state, editor.text + line_start, line_end - line_start, colors);
        for (size_t i = 0; i < match_count; i++)
            for (size_t k = matches[i].start; k < matches[i].end; k++)
                if (k >= line_start && k < line_end) colors[k - line_start] = theme->match_text;
        size_t row_count = editor_wrap_rows(&editor, line, starts, MAX_ROWS_PER_LINE);
        if (row_count > MAX_ROWS_PER_LINE) row_count = MAX_ROWS_PER_LINE;
        size_t indent = editor_row_indent(&editor, line);
        size_t first = line == first_line ? scroll_row - layout.first_row[line] : 0;
        for (size_t row = first; row < row_count && screen_row < rows; row++, screen_row++) {
            size_t row_end = row + 1 < row_count ? starts[row + 1] : line_end;
            draw_row(line, starts[row], row_end, row > 0 ? indent : 0, MARGIN + (float)screen_row * line_height,
                     selections, selection_count, bracket, bracket_self);
        }
    }

    bool blink_on = fmod(now - last_activity, 2 * CURSOR_BLINK_MS) < CURSOR_BLINK_MS;
    for (size_t i = 0; blink_on && i < selection_count; i++) {
        size_t x, row = global_row(selections[i].cursor, &x);
        if (row < scroll_row || row >= scroll_row + rows || x < scroll_column || x > scroll_column + visible_columns())
            continue;
        overlay_rect(MARGIN + (float)(x - scroll_column) * overlay_char_width() - CURSOR_WIDTH / 2,
                     MARGIN + (float)(row - scroll_row) * line_height, CURSOR_WIDTH, line_height, theme->cursor);
    }
    // Bookmarks: a mark in the left margin of the first row of the line.
    for (size_t i = 0; i < editor.bookmark_count; i++) {
        size_t line = editor.bookmarks[i];
        if (line >= layout.line_count || layout.first_row[line] < scroll_row ||
            layout.first_row[line] >= scroll_row + rows)
            continue;
        overlay_rect(MARGIN / 4, MARGIN + (float)(layout.first_row[line] - scroll_row) * line_height + line_height / 4,
                     MARGIN / 2, line_height / 2, theme->bookmark);
    }
    // The IME composition, underlined at the cursor, until the input method commits it.
    if (composition[0]) {
        size_t x, row = global_row(editor.cursor, &x);
        if (row >= scroll_row && row < scroll_row + rows) {
            float left = MARGIN + (float)(x - scroll_column) * overlay_char_width();
            float top = MARGIN + (float)(row - scroll_row) * line_height;
            size_t length = strlen(composition), columns = text_columns(composition);
            overlay_rect(left, top, (float)columns * overlay_char_width(), line_height, theme->popup_background);
            overlay_text(left, top, composition, length, 0, columns, NULL, theme->text);
            overlay_rect(left, top + line_height - 2, (float)columns * overlay_char_width(), 2, theme->text);
        }
    }
    free(selections);
}

// The list below the word that is being typed, left-aligned with its start (above the cursor line when there is
// no room below, as in VS Code). It hides the code below it; see COMPLETION_BOX_RGB.
static void draw_completion_list(void) {
    size_t x, row = global_row(editor.cursor, &x);
    if (row < scroll_row || row >= scroll_row + visible_rows()) return;
    float char_width = overlay_char_width(), line_height = overlay_line_height();
    size_t width = 0;
    for (size_t i = 0; i < completion.count; i++)
        if (strlen(completion.items[i]) > width) width = strlen(completion.items[i]);
    float box_width = (float)(width + 2) * char_width, box_height = (float)completion.count * line_height;
    size_t word_column = x - scroll_column > completion.prefix_length ? x - scroll_column - completion.prefix_length
        : 0;
    float left = MARGIN + (float)word_column * char_width - char_width; // the text starts one column into the box
    if (left < 0) left = 0;
    float top = MARGIN + (float)(row - scroll_row + 1) * line_height;
    if (top + box_height > (float)last_height - bar_height()) top -= box_height + line_height;
    overlay_set_layer(OVERLAY_POPUP);
    overlay_rect(left, top, box_width, box_height, completion_color(COMPLETION_BOX_RGB, 0xFF));
    overlay_set_layer(OVERLAY_POPUP_TEXT);
    overlay_rect(left, top, box_width, box_height, completion_color(0, COMPLETION_SCRIM_ALPHA));
    overlay_rect(left, top + (float)completion.selected * line_height, box_width, line_height,
                 COMPLETION_SELECTED_COLOR);
    for (size_t i = 0; i < completion.count; i++)
        overlay_text(left + char_width, top + (float)i * line_height, completion.items[i], strlen(completion.items[i]),
                     0,
                     width, NULL, COMPLETION_TEXT_COLOR);
    overlay_set_layer(OVERLAY_TOP);
}

static void draw_completion_ghost(void) {
    size_t x, row = global_row(editor.cursor, &x);
    if (row < scroll_row || row >= scroll_row + visible_rows() || x < scroll_column) return;
    const char *rest = completion.items[completion.selected] + completion.prefix_length;
    float left = MARGIN + (float)(x - scroll_column) * overlay_char_width(),
        top = MARGIN + (float)(row - scroll_row) * overlay_line_height();
    size_t columns = overlay_text_columns(rest, strlen(rest));
    overlay_rect(left, top, (float)columns * overlay_char_width() + TEXT_PADDING, overlay_line_height(),
                 theme->line_background);
    overlay_text(left, top, rest, strlen(rest), 0, visible_columns() - (x - scroll_column), NULL, theme->ghost);
}

// Signature help above the cursor line (below it on the first visible row): the call with its parameters and
// defaults, the current parameter marked, and that parameter with its type. The list style draws it like the
// autocomplete list; the ghost style in the ghost color with the parameter in the text color.
static void draw_signature_help(void) {
    if (!signature_help_open) return;
    struct call_context call;
    const struct signature *signature = call_at(editor.cursor, &call);
    if (!signature) {
        signature_help_open = false;
        return;
    }
    char label[MAX_SNIPPET], detail[MAX_SNIPPET];
    size_t active_start, active_end;
    signature_label(signature, call.argument, label, sizeof label, &active_start, &active_end);
    if (call.argument < signature->input_count) {
        const struct signature_input *input = &signature->inputs[call.argument];
        snprintf(detail, sizeof detail, "%s: %s%s%s%s", input->name, input->type,
                 signature_input_is_texture(input) ? " (texture)" : "",
                 input->default_value[0] ? ", default " : "", input->default_value);
    } else {
        snprintf(detail, sizeof detail, "%s takes %zu arguments", signature->name, signature->input_count);
    }
    size_t x, row = global_row(editor.cursor, &x);
    if (row < scroll_row || row >= scroll_row + visible_rows()) return;
    size_t name_x;
    global_row(call.name_start, &name_x);
    if (name_x <= x) x = name_x;
    float char_width = overlay_char_width(), line_height = overlay_line_height();
    size_t columns = strlen(label) > strlen(detail) ? strlen(label) : strlen(detail);
    float box_width = (float)(columns + 2) * char_width, box_height = 2 * line_height;
    float left = MARGIN + (float)(x > scroll_column ? x - scroll_column : 0) * char_width - char_width;
    if (left < 0) left = 0;
    size_t screen_row = row - scroll_row;
    float top = MARGIN + (screen_row >= 2 ? (float)(screen_row - 2) : (float)(screen_row + 1)) * line_height;
    float text_left = left + char_width, active_left = text_left + (float)active_start * char_width;
    float active_width = (float)(active_end - active_start) * char_width;
    if (settings.autocomplete_style == AUTOCOMPLETE_GHOST) {
        overlay_rect(left, top, box_width, box_height, theme->line_background);
        overlay_text(text_left, top, label, strlen(label), 0, columns, NULL, theme->ghost);
        if (active_end > active_start)
            overlay_text(active_left, top, label + active_start, active_end - active_start, 0, columns, NULL,
                         theme->text);
        overlay_text(text_left, top + line_height, detail, strlen(detail), 0, columns, NULL, theme->ghost);
        return;
    }
    overlay_set_layer(OVERLAY_POPUP);
    overlay_rect(left, top, box_width, box_height, completion_color(COMPLETION_BOX_RGB, 0xFF));
    overlay_set_layer(OVERLAY_POPUP_TEXT);
    overlay_rect(left, top, box_width, box_height, completion_color(0, COMPLETION_SCRIM_ALPHA));
    if (active_end > active_start) overlay_rect(active_left, top, active_width, line_height, COMPLETION_SELECTED_COLOR);
    overlay_text(text_left, top, label, strlen(label), 0, columns, NULL, COMPLETION_TEXT_COLOR);
    overlay_text(text_left, top + line_height, detail, strlen(detail), 0, columns, NULL, COMPLETION_TEXT_COLOR);
    overlay_set_layer(OVERLAY_TOP);
}

static void draw_completion(void) {
    if (!completion.open) return;
    if (settings.autocomplete_style == AUTOCOMPLETE_GHOST) draw_completion_ghost();
    else draw_completion_list();
}

static void draw_prompt(int width, int height) {
    char text[2 * MAX_PROMPT + 128], count[48] = "";
    uint32_t color = theme->info;
    if (find_open() && prompt.query[0]) {
        // The match count of the find bar: "0/0" in red when nothing matches.
        snprintf(count, sizeof count, "   %zu/%zu", active_match < match_count ? active_match + 1 : 0, match_count);
        if (!match_count) color = theme->error;
    }
    switch (prompt.kind) {
    case PROMPT_FIND: snprintf(text, sizeof text, "find: %s%s   (Enter: next, Shift+Enter: previous)", prompt.query,
                               count); break;
    case PROMPT_REPLACE:
        snprintf(text, sizeof text, "find: %s%s%s   replace: %s%s   (Tab: field, Enter, Ctrl+Alt+Enter: all)",
                 prompt.query,
                 prompt.field == 0 ? "_" : "", count, prompt.replacement, prompt.field == 1 ? "_" : "");
        break;
    case PROMPT_GO_TO_LINE: snprintf(text, sizeof text, "go to line: %s", prompt.query); break;
    case PROMPT_UNSAVED:
        snprintf(text, sizeof text, "unsaved changes in %s: s save, d discard, Escape cancel", base_name(file_path));
        color = theme->error;
        break;
    case PROMPT_SERVER: snprintf(text, sizeof text, "gallery and patch bay server URL: %s   (Enter: OK, empty: off)",
                                 prompt.query); break;
    case PROMPT_FONT_PATH: snprintf(text, sizeof text, "font file (.ttf, .otf, .ttc): %s   (Enter: OK)",
                                    prompt.query); break;
    case PROMPT_L10N_URL:
        snprintf(text, sizeof text, "editor.json URL of the language: %s   (Enter: OK, empty: the l10n project)",
                 prompt.query);
        break;
    case PROMPT_SHARE: {
        float line_height = overlay_line_height(), y = (float)height - bar_height();
        size_t lines = sizeof share_confirmation / sizeof share_confirmation[0];
        float top = y - (float)(lines + 1) * line_height;
        overlay_rect(0, top, (float)width, (float)(lines + 1) * line_height, theme->popup_background);
        for (size_t i = 0; i < lines; i++)
            overlay_text_run(MARGIN, top + (float)i * line_height + line_height / 2, share_confirmation[i],
                             strlen(share_confirmation[i]), theme->text);
        snprintf(text, sizeof text, "name: %s   (Enter: OK, Escape: cancel)", prompt.query);
        break;
    }
    case PROMPT_PALETTE: {
        snprintf(text, sizeof text, "> %s", prompt.query);
        float line_height = overlay_line_height(), y = (float)height - bar_height();
        size_t shown = 0;
        for (int command = 0; command < COMMAND_COUNT && shown < MAX_PALETTE_ROWS; command++) {
            if (!palette_matches((enum command)command)) continue;
            char row[256], keys[128];
            command_keys((enum command)command, keys, sizeof keys);
            snprintf(row, sizeof row, "%-48s %s", command_description((enum command)command), keys);
            float row_y = y - (float)(shown + 1) * line_height;
            overlay_rect(0, row_y, (float)width, line_height,
                         shown == prompt.palette_index ? theme->popup_selected : theme->popup_background);
            overlay_text(MARGIN, row_y, row, strlen(row), 0, visible_columns(), NULL, theme->text);
            shown++;
        }
        break;
    }
    case PROMPT_NONE: return;
    }
    size_t selected_from = text_columns("find: "), selected_to = selected_from + text_columns(prompt.query);
    if (!prompt.query_selected) selected_to = selected_from;
    draw_bar(width, height, text, color, selected_from, selected_to);
}

void ui_draw(int width, int height, double now_ms) {
    now = now_ms;
    update_recovery();
    if (width != last_width) layout.valid = false;
    last_width = width;
    last_height = height;
    if (!visible) return;
    overlay_begin();
    if (help_visible) {
        draw_help(width, height);
        overlay_end(width, height);
        return;
    }
    update_matches();
    overlay_set_difference(OVERLAY_CODE_BACKGROUND, settings.code_background_blend == CODE_BLEND_DIFFERENCE);
    overlay_set_difference(OVERLAY_CODE_TEXT, settings.code_text_blend == CODE_BLEND_DIFFERENCE);
    draw_code();
    if (info_visible) draw_info();
    if (extensions_visible) draw_extensions();
    if (settings_visible) draw_settings();
    draw_toolbar();
    draw_signature_help();
    draw_completion();
    if (prompt.kind != PROMPT_NONE) draw_prompt(width, height);
    else if (message[0] && (message_is_error || now < message_until))
        draw_bar(width, height, message, message_is_error ? theme->error : theme->info, 0, 0);
    overlay_end(width, height);
}

// ---------------------------------------------------------------- setup

bool ui_text_input_rect(int *x, int *y, int *width, int *height) {
    if (!visible || help_visible) return false;
    float line_height = overlay_line_height();
    *width = (int)overlay_char_width();
    *height = (int)line_height;
    if (prompt.kind != PROMPT_NONE) { // the end of the text in the bar
        *x = (int)(MARGIN + (float)text_columns(prompt_field()) * overlay_char_width());
        *y = (int)((float)last_height - bar_height() + BAR_PADDING);
        return true;
    }
    size_t column, row = global_row(editor.cursor, &column);
    if (row < scroll_row || row >= scroll_row + visible_rows() || column < scroll_column) return false;
    *x = (int)(MARGIN + (float)(column - scroll_column) * overlay_char_width());
    *y = (int)(MARGIN + (float)(row - scroll_row) * line_height);
    return true;
}

void ui_set_visible(bool show) { visible = show; }

void ui_show_info(void) { info_visible = true; }

void ui_enable_gallery(void) { gallery_enabled = true; }

void ui_share_done(bool ok, const char *output) {
    char text[MAX_MESSAGE];
    if (ok) {
        size_t length = strcspn(output, "\n");
        char *url = strndup(output, length);
        SDL_SetClipboardText(url);
        snprintf(text, sizeof text, "shared: %s (copied)", url);
        free(url);
    } else {
        snprintf(text, sizeof text, "sharing failed: %s", output[0] ? output : "no answer from the gallery server");
    }
    ui_show_message(text, !ok);
}

void ui_show_random_example(void) {
    if (replace_with("__randomExample", "no example")) {
        editor.modified = false;
        evaluate_range(0, editor.length);
    }
}

void ui_set_code(const char *code) {
    if (recovery.restored) { // a sketch URL at the start: the recovery file keeps the restored changes
        recovery.restored = false;
        keep_recovery(file_path ? file_path : "");
        close_file();
    }
    editor_set_text(&editor, code, strlen(code));
}

const char *ui_code(void) { return editor.text; }

bool ui_modified(void) { return editor.modified; }

bool ui_init(const struct settings *start_settings, const char *settings_path,
             const char *path, const struct ui_host *ui_host) {
    // A second call (ui_test starts again without a file) replaces the state of the first.
    free(settings_file);
    free(file_path);
    file_path = NULL;
    if (editor.text) editor_free(&editor);
    host = *ui_host;
    settings = *start_settings;
    settings_file = settings_path ? strdup(settings_path) : NULL;
    char error[SETTINGS_TEXT] = "";
    load_themes(&settings, error, sizeof error);
    theme = find_theme(settings.theme);
    if (!theme) {
        if (!error[0]) snprintf(error, sizeof error, "no theme %s, using %s", settings.theme, themes[0].name);
        theme = &themes[0];
    }
    // The "keybindings" of the settings file: command name to key, over the table of src/keymap.c.
    for (size_t i = 0; i < settings.keybinding_count; i++) {
        char problem[SETTINGS_TEXT];
        bool bound = keymap_bind(settings.keybindings[i].key, settings.keybindings[i].value, problem, sizeof problem);
        if (!bound && !error[0]) snprintf(error, sizeof error, "%s", problem);
    }
    if (error[0]) ui_show_message(error, true);
    wrap = settings.word_wrap;
    editor_init(&editor);
    if (path) {
        file_path = strdup(path);
        size_t length;
        char *text = read_whole_file(path, &length);
        if (text) editor_set_text(&editor, text, length);
        free(text);
        show_title(NULL);
    }
    memset(&recovery, 0, sizeof recovery);
    if (recovery_path(recovery.path, sizeof recovery.path)) restore_recovery(path);
    recovery.revision = recovery.seen_revision = editor.revision;
    return overlay_init(settings.font, settings.font_size);
}
