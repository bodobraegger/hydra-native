#define _GNU_SOURCE
#include "ui.h"

#include <ctype.h>
#include <errno.h>
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
#define CURSOR_BLINK_MS 530.0
#define WHEEL_ROWS 3
#define FONT_SIZE_STEP 2.0f
#define MAX_MESSAGE 512
#define MAX_PROMPT 256
#define MAX_COMPLETIONS 24
#define MAX_NAME_COMPLETIONS 12 // the list of names and words; typed lists (textures) may be longer
#define MAX_SIGNATURES 512
#define MAX_SNIPPET 512
#define MIN_COMPLETION_PREFIX 2
#define MAX_PALETTE_ROWS 12
#define MAX_ROWS_PER_LINE 1024
// The autocomplete list draws with blend mode difference, so these are its colors over black in every theme.
#define COMPLETION_BOX_COLOR 0x0000FFFFu
#define COMPLETION_SELECTED_COLOR 0x6060FFFFu
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
    { "contrast", 0x000000FFu, 0x0050FFFFu, 0xFFFFFFA0u, 0xFFFFFF90u, 0xFFFF00FFu, 0xFFFFFFFFu, 0xFF5FFFFFu, 0xFFAF00FFu,
      0x5FFF5FFFu, 0xBCBCBCFFu, 0x5FFFFFFFu, 0x000000FFu, 0xFF3030FFu, 0xFFFFFFFFu, 0x000000FFu, 0x000000FFu,
      0x0050FFFFu, 0x000000F0u, 0x5FFFFFFFu, 0x5FFFFFFFu, 0xFFFF00FFu, 0xFF9632FFu, 0x000000FFu, 0xFFFFFF90u,
      { 0xFFFF00FFu, 0xFF5FFFFFu, 0x5FAFFFFFu }, 0xFFFFFF70u },
    { "solarized", 0x002B36D0u, 0x268BD2A0u, 0xEEE8D570u, 0x93A1A160u, 0xEEE8D5FFu, 0x93A1A1FFu, 0x859900FFu, 0xD33682FFu,
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
    THEME_COLOR("bracket", bracket), THEME_COLOR("cursor", cursor), THEME_COLOR("text", text), THEME_COLOR("keyword", keyword),
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
    if (text[0] != '#' || (length != 7 && length != 9) || strspn(text + 1, "0123456789abcdefABCDEF") != length - 1) return false;
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
                if (!strcmp(theme_color_names[n].name, pair->key)) target = (uint32_t *)((char *)built + theme_color_names[n].offset);
            if (target && parse_color(pair->value, &color)) *target = color;
            else if (!error[0]) snprintf(error, size, "theme %s: %s \"%s\" is no known color", custom->name, pair->key, pair->value);
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

enum prompt_kind { PROMPT_NONE, PROMPT_FIND, PROMPT_REPLACE, PROMPT_GO_TO_LINE, PROMPT_PALETTE, PROMPT_SHARE, PROMPT_SERVER };

// The server of the deployed web editor (VITE_SERVER_URL of hydra.ojack.xyz) for gallery and patch bay.
#define WEB_EDITOR_SERVER "https://api.hydrasynth.xyz"

// The prompt() text of showConfirmation() in gallery-store.js of the web editor.
static const char *const share_confirmation[] = {
    "HYDRA SKETCH GALLERY",
    "///////////////////////////////////////",
    "\xF0\x9F\x8E\xA8 Add your sketch to the gallery of hydra sketches at https://botsin.space/@hydra.",
    "\xE2\x9C\x8D\xEF\xB8\x8F Type your name, mastodon handle, or a short description below, and then press 'OK' to share.",
    "\xE2\x80\xBC\xEF\xB8\x8F Make sure you are ready to share - there is no undo button!",
    "\xF0\x9F\x92\x96 Thank you for sharing! You are also warmly invited to join the the live coding server on the "
    "fediverse at https://social.toplap.org/.",
};

static struct ui_host host;
// The settings file as stored, and the settings of this run (command line options on top).
static struct settings stored_settings, active_settings;
static char *settings_file;
static struct editor editor;
static char *file_path;
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
#define MAX_PANEL_BUTTONS 256
#define EXTENSION_ROWS 3
// Clickable areas of the extensions panel, with the action that src/ui.c runs on a click.
static struct {
    float x, y, width, height;
    char action[32]; // "category N", "add N", "docs N", "example N M"
} panel_buttons[MAX_PANEL_BUTTONS];
static size_t panel_button_count;
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

static size_t global_caret_row(size_t offset, enum sticky sticky, size_t *x) {
    size_t row;
    editor_caret_position(&editor, offset, sticky, &row, x);
    return layout.first_row[editor_line_of(&editor, offset)] + row;
}

static size_t global_row(size_t offset, size_t *x) { return global_caret_row(offset, STICKY_NONE, x); }

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

// true when the code ran without an error
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
    if (url) host.set_title(url);
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
        used += count;
        if (used == capacity) text = realloc(text, capacity *= 2);
    }
    fclose(file);
    *length = used;
    return text;
}

static void save(void) {
    if (!file_path) return ui_show_message("no file to save: start hydra-native with a file argument", true);
    char *temporary;
    if (asprintf(&temporary, "%s.hydra-native-save", file_path) < 0) return;
    FILE *file = fopen(temporary, "wb");
    bool ok = file && fwrite(editor.text, 1, editor.length, file) == editor.length;
    if (file && fclose(file) != 0) ok = false;
    if (ok && rename(temporary, file_path) != 0) ok = false;
    char text[MAX_MESSAGE];
    if (ok) {
        editor.modified = false;
        snprintf(text, sizeof text, "saved %s", file_path);
    } else {
        snprintf(text, sizeof text, "cannot save %s: %s", file_path, strerror(errno));
        unlink(temporary);
    }
    ui_show_message(text, !ok);
    free(temporary);
}

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

// The hydra function whose parentheses hold `offset`, or NULL.
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

// Completions by the type of the argument at the word that starts at `start`: transforms after a ".", textures
// for a texture input, values and helpers for a number input. False when the place has no type.
static bool open_typed_completion(const char *prefix, size_t length, size_t start) {
    if (start > 0 && editor.text[start - 1] == '.') {
        for (size_t i = 0; i < signature_count; i++)
            if (signature_is_chained(&signatures[i]) && starts_with(signatures[i].name, prefix, length))
                add_function_completion(signatures[i].name, strlen(signatures[i].name));
        if (starts_with("out", prefix, length)) add_completion("out", 3, "out(${o0})");
        return true;
    }
    struct call_context call;
    const struct signature *signature = call_at(start, &call);
    if (!signature || !call.direct || call.argument >= signature->input_count) return false;
    if (!signature_is_source(signature) && !signature_is_chained(signature)) return false; // not a glsl function
    const struct signature_input *input = &signature->inputs[call.argument];
    if (signature_input_is_texture(input)) {
        static const char *const textures[] = { "o0", "o1", "o2", "o3", "s0", "s1", "s2", "s3" };
        for (size_t i = 0; i < sizeof textures / sizeof textures[0]; i++)
            if (starts_with(textures[i], prefix, length)) add_completion(textures[i], strlen(textures[i]), NULL);
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
        bool indexed = helper_length >= 2 && (!strcmp(helper + helper_length - 2, "[]") || !strcmp(helper + helper_length - 2, "()"));
        snprintf(insert, sizeof insert, "%s%.*s%s%s", in_arrow ? "" : "() => ", (int)(indexed ? helper_length - 1 : helper_length),
                 helper, indexed ? "${0}" : "", indexed ? helper + helper_length - 1 : "");
        add_completion(helper, helper_length, insert);
    }
    free(helpers);
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
        char *names = host.call("__completionNames", "");
        for (char *name = names ? strtok(names, "\n") : NULL; name && completion.count < completion.limit; name = strtok(NULL, "\n"))
            if (strlen(name) > length && !strncmp(name, prefix, length)) add_completion(name, strlen(name), NULL);
        free(names);
        char **words;
        size_t count = editor_words(&editor, prefix, length, &words);
        for (size_t i = 0; i < count; i++) {
            add_completion(words[i], strlen(words[i]), NULL);
            free(words[i]);
        }
        free(words);
    }
    completion.prefix_length = length;
    completion.selected = 0;
    completion.open = completion.count > 0;
}

static void accept_completion(void) {
    size_t start;
    editor_word_before_cursor(&editor, &start);
    editor_set_cursor(&editor, start, true); // the typed word is replaced
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

static char *prompt_field(void) { return prompt.kind == PROMPT_REPLACE && prompt.field == 1 ? prompt.replacement : prompt.query; }

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

static void accept_prompt(int direction) {
    switch (prompt.kind) {
    case PROMPT_FIND: find(direction); break;
    case PROMPT_REPLACE:
        if (prompt.field == 0) find(direction);
        else editor_replace_next(&editor, prompt.query, strlen(prompt.query), prompt.replacement, strlen(prompt.replacement), true);
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
    case PROMPT_SHARE:
        prompt.kind = PROMPT_NONE;
        if (!host.share(editor.text, prompt.query)) ui_show_message("no gallery server: start with --gallery-server URL", true);
        else ui_show_message("sharing with the gallery...", false);
        break;
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

static void save_settings(void);

// The gallery and patch bay server ("" is off): js/main.js starts or stops the patch bay at once, and "share"
// shows only with a server, as in the web editor.
static void set_server(const char *url) {
    snprintf(stored_settings.gallery_server, sizeof stored_settings.gallery_server, "%s", url);
    snprintf(active_settings.gallery_server, sizeof active_settings.gallery_server, "%s", url);
    save_settings();
    free(host.call("__setPatchBayServer", url));
    gallery_enabled = url[0] != '\0';
    char text[SETTINGS_TEXT + 64];
    if (url[0]) snprintf(text, sizeof text, "gallery and patch bay: %s", url);
    else snprintf(text, sizeof text, "gallery and patch bay: off");
    ui_show_message(text, false);
}

// "pb.id stream-id" of this instance in the patch bay room ("" without a connection). The stream id (the socket
// id of the signalling) is what other peers pass to s0.initStream; pb.id is only the default nickname.
static char *patch_bay_id(void) {
    char *id = host.call("__patchBayId", "");
    if (!id) id = strdup("");
    return id;
}

static void copy_patch_bay_id(void) {
    char *id = patch_bay_id(), text[192];
    char *stream_id = strchr(id, ' ');
    if (stream_id) {
        SDL_SetClipboardText(stream_id + 1);
        snprintf(text, sizeof text, "copied the stream id %s: s0.initStream('%s') elsewhere", stream_id + 1, stream_id + 1);
    } else {
        snprintf(text, sizeof text, "the patch bay is not connected (settings: server)");
    }
    ui_show_message(text, !stream_id);
    free(id);
}

// Writes the settings file after a change of the settings menu or of the font size.
static void save_settings(void) {
    char error[SETTINGS_TEXT];
    if (settings_file && !settings_save(&stored_settings, settings_file, error, sizeof error))
        ui_show_message(error, true); // a file that does not parse stays as it is
}

static void change_font_size(float step) {
    if (overlay_set_font_size(overlay_font_size() + step)) {
        char text[64];
        snprintf(text, sizeof text, "font size %.0f px", overlay_font_size());
        ui_show_message(text, false);
        layout.valid = false;
        stored_settings.font_size = active_settings.font_size = overlay_font_size();
        save_settings();
    }
}

static void set_theme(const struct theme *next) {
    theme = next;
    snprintf(stored_settings.theme, sizeof stored_settings.theme, "%s", next->name);
    snprintf(active_settings.theme, sizeof active_settings.theme, "%s", next->name);
    save_settings();
}

static void set_autocomplete(enum autocomplete_style style, enum autocomplete_trigger trigger) {
    close_completion();
    stored_settings.autocomplete_style = active_settings.autocomplete_style = style;
    stored_settings.autocomplete_trigger = active_settings.autocomplete_trigger = trigger;
    save_settings();
    char text[128];
    snprintf(text, sizeof text, "autocomplete: %s, %s", style == AUTOCOMPLETE_GHOST ? "ghost text" : "list",
             trigger == AUTOCOMPLETE_TYPING ? "while typing" : "Ctrl+Space");
    ui_show_message(text, false);
}

static void set_signature_help(enum signature_help mode) {
    stored_settings.signature_help = active_settings.signature_help = mode;
    save_settings();
    ui_show_message(mode == SIGNATURE_HELP_PAREN ? "signature help: Ctrl+Shift+Space and after \"(\""
                                                 : "signature help: Ctrl+Shift+Space", false);
}

static void set_font(const char *path) {
    if (!overlay_set_font(path)) return ui_show_message("cannot use this font", true);
    snprintf(stored_settings.font, sizeof stored_settings.font, "%s", path);
    snprintf(active_settings.font, sizeof active_settings.font, "%s", path);
    layout.valid = false;
    save_settings();
}

// Formats with js-beautify, like the web editor (see __format in js/main.js).
static void format_code(void) {
    char *formatted = host.call("__format", editor.text);
    if (!formatted) return ui_show_message("format: the code has an error", true);
    editor_replace_text(&editor, formatted, strlen(formatted));
    editor_set_cursor(&editor, 0, false); // cm.setValue() puts the cursor at the start
    free(formatted);
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

static void clear_all(void) {
    free(evaluate_code("hush(); speed = 1", strlen("hush(); speed = 1"), "<clear>"));
    editor_replace_text(&editor, CLEARED_CODE, strlen(CLEARED_CODE));
    char *url = host.call("__webEditorUrl", ""); // 'gallery: clear' removes the code from the URL
    if (url) host.set_title(url);
    free(url);
}

// Like "save to URL" of the web editor; without an address bar, the URL goes to the window title and the clipboard.
static void save_to_url(void) {
    char *url = host.call("__urlForCode", editor.text);
    if (!url) return;
    host.set_title(url);
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
        editor_replace_text(&editor, code, strlen(code));
        editor_set_cursor(&editor, 0, false);
        evaluate_range(0, editor.length);
        host.set_title(url);
    }
    free(code);
    SDL_free(url);
}

static void escape(void) {
    if (completion.open) close_completion();
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
    return command == COMMAND_TRIGGER_COMPLETION || command == COMMAND_COMPLETION_NEXT || command == COMMAND_COMPLETION_PREVIOUS ||
           command == COMMAND_COMPLETION_ACCEPT;
}

// Open suggestions follow Backspace and close for any other command, as in VS Code.
static void run_command(enum command command, bool extend) {
    bool was_open = completion.open;
    size_t old_length = editor.length;
    unsigned generation = snippet.generation;
    if (was_open && !is_completion_command(command) && command != COMMAND_BACKSPACE) close_completion();
    run_command_body(command, extend);
    if (generation == snippet.generation) follow_snippet(old_length);
    if (was_open && command == COMMAND_BACKSPACE) open_completion(true);
}

static void run_command_body(enum command command, bool extend) {
    size_t start, end;
    long page = (long)visible_rows() - 1;
    if (command != COMMAND_COMPLETION_NEXT && command != COMMAND_COMPLETION_PREVIOUS &&
        command != COMMAND_COMPLETION_ACCEPT && command != COMMAND_TRIGGER_COMPLETION)
        close_completion();
    switch (command) {
    case COMMAND_NONE:
    case COMMAND_COUNT: break;
    case COMMAND_TOGGLE_CODE: visible = !visible; return;
    case COMMAND_EVAL_LINE: editor_line_range(&editor, &start, &end); evaluate_range(start, end); break;
    case COMMAND_EVAL_BLOCK: editor_block_range(&editor, &start, &end); evaluate_range(start, end); break;
    case COMMAND_EVAL_ALL: evaluate_all(); break;
    case COMMAND_SAVE: save(); break;
    case COMMAND_FULLSCREEN: host.toggle_fullscreen(); break;
    case COMMAND_QUIT: host.quit(); break;
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
    case COMMAND_FORMAT: format_code(); break;
    case COMMAND_SCREENCAP: host.screencap(); break;
    case COMMAND_SAVE_TO_URL: save_to_url(); break;
    case COMMAND_OPEN_URL: open_url_from_clipboard(); break;
    case COMMAND_GALLERY_SHARE: {
        // Like 'gallery:shareSketch': evaluate first, and ask only when the code runs.
        char *error = evaluate_code(editor.text, editor.length, "<editor>");
        show_result("", error);
        if (!error) open_prompt(PROMPT_SHARE);
        free(error);
        return;
    }
    case COMMAND_CLEAR_ALL: clear_all(); break;
    case COMMAND_SHOW_EXAMPLE:
        clear_all();
        if (replace_with("__randomExample", "no example")) {
            evaluate_range(0, editor.length);
            update_url();
        }
        break;
    // The mutator evaluates the code itself and retries on errors, like in the web editor.
    case COMMAND_MUTATE:
        if (replace_with("__mutate", "mutator: the code does not parse")) update_url();
        break;
    case COMMAND_MUTATE_TRANSFORM:
        if (replace_with("__mutateTransform", "mutator: the code does not parse")) update_url();
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
        info_visible = extensions_visible = false;
        info_scroll = 0;
        return;
    case COMMAND_COPY_PATCH_BAY_ID: copy_patch_bay_id(); return;
    case COMMAND_SET_SERVER:
        open_prompt(PROMPT_SERVER);
        snprintf(prompt.query, sizeof prompt.query, "%.*s", (int)sizeof prompt.query - 1, active_settings.gallery_server);
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
        set_autocomplete(active_settings.autocomplete_style == AUTOCOMPLETE_LIST ? AUTOCOMPLETE_GHOST : AUTOCOMPLETE_LIST,
                         active_settings.autocomplete_trigger);
        return;
    case COMMAND_TOGGLE_AUTOCOMPLETE_TRIGGER:
        set_autocomplete(active_settings.autocomplete_style,
                         active_settings.autocomplete_trigger == AUTOCOMPLETE_MANUAL ? AUTOCOMPLETE_TYPING : AUTOCOMPLETE_MANUAL);
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
    case COMMAND_COMPLETION_PREVIOUS: completion.selected = (completion.selected + completion.count - 1) % completion.count; return;
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
    case COMMAND_PROMPT_ACCEPT: accept_prompt(1); break;
    case COMMAND_PROMPT_ACCEPT_BACKWARD: accept_prompt(-1); break;
    case COMMAND_PROMPT_NEXT_FIELD: prompt.field = prompt.kind == PROMPT_REPLACE ? !prompt.field : 0; return;
    // The palette list grows upwards from the prompt line, so Up goes to the next entry.
    case COMMAND_PROMPT_UP:
        if (palette_command(prompt.palette_index + 1) != COMMAND_NONE) prompt.palette_index++;
        return;
    case COMMAND_PROMPT_DOWN: if (prompt.palette_index > 0) prompt.palette_index--; return;
    case COMMAND_PROMPT_BACKSPACE: prompt_backspace(); break;
    case COMMAND_REPLACE_ALL: {
        size_t count = editor_replace_all(&editor, prompt.query, strlen(prompt.query), prompt.replacement, strlen(prompt.replacement), true);
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
    if (completion.open) return active_settings.autocomplete_style == AUTOCOMPLETE_GHOST ? CONTEXT_GHOST : CONTEXT_COMPLETION;
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
    if (help_visible) return;
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
    if (!strcmp(text, "(") && active_settings.signature_help == SIGNATURE_HELP_PAREN && call_at(editor.cursor, &call))
        signature_help_open = true;
    bool word = isalnum((unsigned char)text[0]) || text[0] == '_' || text[0] == '$';
    // Open suggestions follow the typed word; new ones open only with the trigger "while typing" (the default is
    // Ctrl+Space alone).
    if (completion.open) open_completion(true);
    else if (word && active_settings.autocomplete_trigger == AUTOCOMPLETE_TYPING) open_completion(false);
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
    { "extensions", "toolbar.load-extension", COMMAND_TOGGLE_EXTENSIONS, COMMAND_TOGGLE_EXTENSIONS, COMMAND_TOGGLE_EXTENSIONS },
    { "example", "toolbar.shuffle", COMMAND_SHOW_EXAMPLE, COMMAND_SHOW_EXAMPLE, COMMAND_SHOW_EXAMPLE },
    { "mutate", "toolbar.random", COMMAND_MUTATE, COMMAND_MUTATE_UNDO, COMMAND_MUTATE_TRANSFORM },
    { "share", "toolbar.upload", COMMAND_GALLERY_SHARE, COMMAND_GALLERY_SHARE, COMMAND_GALLERY_SHARE },
    { "settings", NULL, COMMAND_TOGGLE_SETTINGS, COMMAND_TOGGLE_SETTINGS, COMMAND_TOGGLE_SETTINGS }, // not in the web editor
    { "?", "toolbar.show-info", COMMAND_TOGGLE_INFO, COMMAND_TOGGLE_HELP, COMMAND_TOGGLE_HELP },
};
#define TOOLBAR_COUNT (sizeof toolbar / sizeof toolbar[0])

static bool toolbar_shown(size_t index) { return gallery_enabled || toolbar[index].command != COMMAND_GALLERY_SHARE; }

// The right-aligned boxes of the toolbar; returns the x of item `index`, and its width in `width`.
static float toolbar_x(size_t index, float *width) {
    float x = (float)last_width - MARGIN;
    for (size_t i = TOOLBAR_COUNT; i-- > index;) {
        if (!toolbar_shown(i)) continue;
        *width = ((float)strlen(toolbar[i].label) + 1) * overlay_char_width();
        x -= *width + TEXT_PADDING;
    }
    return x;
}

static bool toolbar_click(float x, float y, SDL_Keymod modifiers) {
    if (y < MARGIN || y > MARGIN + overlay_line_height()) return false;
    for (size_t i = 0; i < TOOLBAR_COUNT; i++) {
        if (!toolbar_shown(i)) continue;
        float width, left = toolbar_x(i, &width);
        if (x < left || x > left + width) continue;
        const struct toolbar_item *item = &toolbar[i];
        run_command(modifiers & SDL_KMOD_SHIFT ? item->with_shift : modifiers & SDL_KMOD_CTRL ? item->with_ctrl : item->command,
                    false);
        return true;
    }
    return false;
}

static void draw_toolbar(void) {
    for (size_t i = 0; i < TOOLBAR_COUNT; i++) {
        if (!toolbar_shown(i)) continue;
        float width, x = toolbar_x(i, &width);
        overlay_rect(x, MARGIN, width, overlay_line_height(), theme->toolbar);
        overlay_text(x + overlay_char_width() / 2, MARGIN, toolbar[i].label, strlen(toolbar[i].label), 0, 64, NULL,
                     theme->info);
        bool hover = pointer_y >= MARGIN && pointer_y <= MARGIN + overlay_line_height() && pointer_x >= x &&
                     pointer_x <= x + width;
        if (!hover) continue;
        const char *title = toolbar[i].command == COMMAND_TOGGLE_INFO && info_visible ? "toolbar.hide-info" : toolbar[i].title;
        char *text = title ? host.call("__translate", title) : strdup(command_description(toolbar[i].command));
        if (!text) continue;
        float tooltip_width = overlay_text_shaped_width(text, strlen(text), false);
        float left = x + width - tooltip_width;
        if (left < MARGIN) left = MARGIN;
        float top = MARGIN + overlay_line_height() + TEXT_PADDING;
        overlay_rect(left - TEXT_PADDING, top, tooltip_width + 2 * TEXT_PADDING, overlay_line_height(), theme->popup_background);
        overlay_text_shaped(left, top, text, strlen(text), false, theme->text);
        free(text);
    }
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

static bool starts_wide(const char *text) {
    size_t consumed;
    return unicode_columns(unicode_decode((const unsigned char *)text, strlen(text), &consumed)) == 2;
}

// Breaks a paragraph into lines of at most `width` pixels of shaped text: at spaces, and between characters of
// scripts without spaces (Japanese, Chinese). Calls `line` for each line.
static void wrap_shaped(const char *text, float width, bool rtl, void (*line)(const char *text, size_t length, void *data),
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
        bool fits = overlay_text_shaped_width(text + start, candidate - start, rtl) <= width;
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
    float left, top, width;
    bool rtl;
    size_t row, rows, skipped;
};

static void draw_info_line(const char *text, size_t length, void *data) {
    struct info_layout *layout_state = data;
    if (layout_state->skipped++ < info_scroll || layout_state->row >= layout_state->rows) return;
    float line_width = overlay_text_shaped_width(text, length, layout_state->rtl);
    float x = layout_state->rtl ? layout_state->left + layout_state->width - line_width : layout_state->left;
    overlay_text_shaped(x, layout_state->top + overlay_line_height() * (float)layout_state->row++, text, length,
                        layout_state->rtl, theme->text);
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
        float name_width = overlay_text_shaped_width(name, strlen(name), false) + char_width;
        if (x + name_width > left + width) break;
        snprintf(language_buttons[language_button_count].code, sizeof language_buttons[0].code, "%s", line);
        language_buttons[language_button_count].x = x;
        language_buttons[language_button_count++].width = name_width;
        overlay_rect(x, top + line_height / 2, name_width, line_height,
                     selected && *selected == '1' ? theme->popup_selected : theme->toolbar);
        overlay_text_shaped(x + char_width / 2, top + line_height / 2, name, strlen(name), false, theme->text);
        x += name_width + char_width;
    }
    free(languages);
    char *text = host.call("__infoText", "");
    if (!text) return;
    char *saveptr;
    char *direction = strtok_r(text, "\n", &saveptr);
    struct info_layout layout_state = { left + 2 * char_width, top + 2 * line_height, width - 4 * char_width,
                                        direction && !strcmp(direction, "rtl"), 0,
                                        (size_t)((height - 3 * line_height) / line_height), 0 };
    for (char *paragraph = strtok_r(NULL, "\n", &saveptr); paragraph; paragraph = strtok_r(NULL, "\n", &saveptr)) {
        wrap_shaped(paragraph, layout_state.width, layout_state.rtl, draw_info_line, &layout_state);
        draw_info_line("", 0, &layout_state); // a blank line between paragraphs
    }
    free(text);
}

static void add_panel_button(float x, float y, float width, float height, const char *action) {
    if (panel_button_count == MAX_PANEL_BUTTONS) return;
    panel_buttons[panel_button_count].x = x;
    panel_buttons[panel_button_count].y = y;
    panel_buttons[panel_button_count].width = width;
    panel_buttons[panel_button_count].height = height;
    snprintf(panel_buttons[panel_button_count++].action, sizeof panel_buttons[0].action, "%s", action);
}

static float panel_label(float x, float y, const char *text, bool selected, const char *action) {
    size_t columns = overlay_text_columns(text, strlen(text)) + 1;
    float width = (float)columns * overlay_char_width();
    overlay_rect(x, y, width, overlay_line_height(), selected ? theme->popup_selected : theme->toolbar);
    overlay_text(x + overlay_char_width() / 2, y, text, strlen(text), 0, columns, NULL, theme->text);
    add_panel_button(x, y, width, overlay_line_height(), action);
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
    float thumbnail_size = (EXTENSION_ROWS - 0.5f) * line_height, text_left = left + char_width + thumbnail_size + char_width;
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
        char action[32];
        if (!strcmp(fields[0], "category") && count >= 3) {
            snprintf(action, sizeof action, "category %zu", entry++);
            x = panel_label(x, y, fields[1], fields[2][0] == '1', action);
        } else if (!strcmp(fields[0], "state") && count >= 2) {
            entry = 0;
            const char *state = !strcmp(fields[1], "loading") ? "loading the list from github..."
                                : !strcmp(fields[1], "failed") ? "cannot load the list: hydra-native needs the network for it"
                                                                : "";
            overlay_text(left + char_width, y + 1.5f * line_height, state, strlen(state), 0, text_columns, NULL, theme->info);
            row = 1;
        } else if (!strcmp(fields[0], "entry") && count >= 5) {
            size_t index = entry++;
            if (index < info_scroll) continue;
            if (row + EXTENSION_ROWS > rows) continue;
            float entry_top = y + line_height * (float)(row + 1);
            unsigned texture = count >= 6 && fields[5][0] ? thumbnail_texture(fields[5]) : 0;
            if (texture) overlay_image(left + char_width, entry_top, thumbnail_size, thumbnail_size, texture);
            char title[512];
            snprintf(title, sizeof title, "%s - %s", fields[1], fields[2]);
            overlay_text(text_left, entry_top, title, strlen(title), 0, text_columns, NULL, theme->help_keys);
            overlay_text(text_left, entry_top + line_height, fields[3], strlen(fields[3]), 0, text_columns, NULL, theme->text);
            float button_x = text_left;
            snprintf(action, sizeof action, "add %zu", index);
            button_x = panel_label(button_x, entry_top + 2 * line_height, "+", false, action);
            snprintf(action, sizeof action, "docs %zu", index);
            button_x = panel_label(button_x, entry_top + 2 * line_height, "docs", false, action);
            for (long example = 0; example < atol(fields[4]); example++) {
                char label[24];
                snprintf(label, sizeof label, "%ld", example + 1);
                snprintf(action, sizeof action, "example %zu %ld", index, example);
                button_x = panel_label(button_x, entry_top + 2 * line_height, label, false, action);
            }
            row += EXTENSION_ROWS + 1;
        }
    }
    free(panel);
}


static void run_panel_action(const char *action) {
    char *result = NULL;
    if (!strncmp(action, "category ", 9)) {
        info_scroll = 0;
        free(host.call("__extensionsPanel", action + 9));
    } else if (!strncmp(action, "add ", 4) && (result = host.call("__extensionLoadCode", action + 4))) {
        // 'editor: add code to top' of the web editor (CodeMirror 5 version).
        size_t length = strlen(result) + editor.length + 4;
        char *code = malloc(length + 1);
        snprintf(code, length + 1, "\n%s\n\n%s\n", result, editor.text);
        editor_replace_text(&editor, code, strlen(code));
        free(code);
        extensions_visible = false;
    } else if (!strncmp(action, "docs ", 5) && (result = host.call("__extensionDocumentation", action + 5))) {
        SDL_OpenURL(result);
    } else if (!strncmp(action, "example ", 8) && (result = host.call("__extensionExample", action + 8))) {
        // 'load and eval code'
        editor_replace_text(&editor, result, strlen(result));
        evaluate_range(0, editor.length);
        extensions_visible = false;
    }
    free(result);
}

static bool extensions_click(float x, float y) {
    float left, top, width, height;
    info_panel(&left, &top, &width, &height);
    if (x < left || x > left + width || y < top || y > top + height) {
        extensions_visible = false;
        return false;
    }
    for (size_t i = 0; i < panel_button_count; i++) {
        if (x >= panel_buttons[i].x && x <= panel_buttons[i].x + panel_buttons[i].width && y >= panel_buttons[i].y &&
            y <= panel_buttons[i].y + panel_buttons[i].height) {
            char action[32];
            snprintf(action, sizeof action, "%s", panel_buttons[i].action);
            run_panel_action(action);
            break;
        }
    }
    return true;
}

static bool info_click(float x, float y) {
    float left, top, width, height;
    info_panel(&left, &top, &width, &height);
    if (x < left || x > left + width || y < top || y > top + height) {
        info_visible = false; // a click outside closes the panel
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
        if (!extension || (strcasecmp(extension, ".ttf") && strcasecmp(extension, ".otf") && strcasecmp(extension, ".ttc"))) continue;
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
    // Sorted by family name.
    for (size_t i = 1; i < font_choice_count; i++)
        for (size_t k = i; k > 0 && strcasecmp(font_choices[k - 1].family, font_choices[k].family) > 0; k--) {
            typeof(font_choices[0]) swap = font_choices[k];
            font_choices[k] = font_choices[k - 1];
            font_choices[k - 1] = swap;
        }
}

// The settings menu: theme, autocomplete, font size and font, saved in the settings file at once.
static void draw_settings(void) {
    float left, top, width, height, line_height = overlay_line_height(), char_width = overlay_char_width();
    info_panel(&left, &top, &width, &height);
    overlay_rect(left, top, width, height, theme->help_background);
    panel_button_count = 0;
    if (!font_choices_loaded) load_font_choices();
    float label_x = left + char_width, value_x = left + 15 * char_width, y = top + line_height / 2;
    char action[32];
    const char *labels[] = { "theme", "autocomplete", "trigger", "signatures", "whitespace", "line wrap", "server", "font size",
                             "font" };
    for (size_t row = 0; row < sizeof labels / sizeof labels[0]; row++)
        overlay_text(label_x, y + (float)row * 1.5f * line_height, labels[row], strlen(labels[row]), 0, 14, NULL, theme->help_keys);
    float x = value_x;
    for (size_t i = 0; i < theme_count; i++) {
        snprintf(action, sizeof action, "theme %zu", i);
        x = panel_label(x, y, themes[i].name, theme == &themes[i], action);
    }
    y += 1.5f * line_height;
    x = panel_label(value_x, y, "list", active_settings.autocomplete_style == AUTOCOMPLETE_LIST, "style 0");
    panel_label(x, y, "ghost text", active_settings.autocomplete_style == AUTOCOMPLETE_GHOST, "style 1");
    y += 1.5f * line_height;
    x = panel_label(value_x, y, "Ctrl+Space", active_settings.autocomplete_trigger == AUTOCOMPLETE_MANUAL, "trigger 0");
    panel_label(x, y, "while typing", active_settings.autocomplete_trigger == AUTOCOMPLETE_TYPING, "trigger 1");
    y += 1.5f * line_height;
    x = panel_label(value_x, y, "Ctrl+Shift+Space", active_settings.signature_help == SIGNATURE_HELP_MANUAL, "signature 0");
    panel_label(x, y, "on (", active_settings.signature_help == SIGNATURE_HELP_PAREN, "signature 1");
    y += 1.5f * line_height;
    x = value_x;
    for (int i = WHITESPACE_NONE; i <= WHITESPACE_ALL; i++) {
        snprintf(action, sizeof action, "whitespace %d", i);
        x = panel_label(x, y, settings_whitespace_name((enum render_whitespace)i), active_settings.render_whitespace == (enum render_whitespace)i,
                        action);
    }
    y += 1.5f * line_height;
    x = panel_label(value_x, y, "on", wrap, "wrap 1");
    panel_label(x, y, "off", !wrap, "wrap 0");
    y += 1.5f * line_height;
    const char *server = active_settings.gallery_server;
    x = panel_label(value_x, y, "off", !server[0], "server off");
    x = panel_label(x, y, "hydra.ojack.xyz", !strcmp(server, WEB_EDITOR_SERVER), "server web");
    bool custom = server[0] && strcmp(server, WEB_EDITOR_SERVER);
    x = panel_label(x, y, custom ? server : "custom", custom, "server custom");
    char *id = patch_bay_id(), *stream_id = strchr(id, ' ');
    if (stream_id) { // the own ids while the patch bay is connected, with a button that copies the stream id
        char label[160];
        *stream_id++ = '\0';
        snprintf(label, sizeof label, "pb.id %s, stream id %s", id, stream_id);
        overlay_text(x, y, label, strlen(label), 0, 40, NULL, theme->help_keys);
        panel_label(x + (float)(strlen(label) + 1) * char_width, y, "copy", false, "copy id");
    }
    free(id);
    y += 1.5f * line_height;
    char size[16];
    snprintf(size, sizeof size, "%.0f px", overlay_font_size());
    x = panel_label(value_x, y, "-", false, "size -");
    overlay_text(x, y, size, strlen(size), 0, 8, NULL, theme->text);
    panel_label(x + 7 * char_width, y, "+", false, "size +");
    y += 1.5f * line_height;
    size_t rows = (size_t)((top + height - y - line_height / 2) / line_height);
    if (info_scroll > font_choice_count) info_scroll = font_choice_count;
    for (size_t i = info_scroll, row = 0; i < font_choice_count && row < rows; i++, row++) {
        snprintf(action, sizeof action, "font %zu", i);
        panel_label(value_x, y + (float)row * line_height, font_choices[i].family, !strcmp(font_choices[i].file, active_settings.font),
                    action);
    }
}

static void run_settings_action(const char *action) {
    size_t index = (size_t)atol(strchr(action, ' ') + 1);
    if (!strncmp(action, "theme ", 6) && index < theme_count) set_theme(&themes[index]);
    else if (!strncmp(action, "style ", 6)) set_autocomplete((enum autocomplete_style)index, active_settings.autocomplete_trigger);
    else if (!strncmp(action, "trigger ", 8)) set_autocomplete(active_settings.autocomplete_style, (enum autocomplete_trigger)index);
    else if (!strncmp(action, "signature ", 10) && index <= SIGNATURE_HELP_PAREN) set_signature_help((enum signature_help)index);
    else if (!strncmp(action, "whitespace ", 11) && index <= WHITESPACE_ALL) {
        stored_settings.render_whitespace = active_settings.render_whitespace = (enum render_whitespace)index;
        save_settings();
    } else if (!strcmp(action, "server off")) {
        set_server("");
    } else if (!strcmp(action, "server web")) {
        set_server(WEB_EDITOR_SERVER);
    } else if (!strcmp(action, "server custom")) {
        settings_visible = false;
        run_command(COMMAND_SET_SERVER, false);
    } else if (!strcmp(action, "copy id")) {
        copy_patch_bay_id();
    } else if (!strncmp(action, "wrap ", 5)) {
        wrap = stored_settings.word_wrap = active_settings.word_wrap = index != 0;
        layout.valid = false;
        save_settings();
    } else if (!strcmp(action, "size -")) change_font_size(-FONT_SIZE_STEP);
    else if (!strcmp(action, "size +")) change_font_size(FONT_SIZE_STEP);
    else if (!strncmp(action, "font ", 5) && index < font_choice_count) set_font(font_choices[index].file);
}

static bool settings_click(float x, float y) {
    float left, top, width, height;
    info_panel(&left, &top, &width, &height);
    if (x < left || x > left + width || y < top || y > top + height) {
        settings_visible = false;
        return false;
    }
    for (size_t i = 0; i < panel_button_count; i++) {
        if (x >= panel_buttons[i].x && x <= panel_buttons[i].x + panel_buttons[i].width && y >= panel_buttons[i].y &&
            y <= panel_buttons[i].y + panel_buttons[i].height) {
            char action[32];
            snprintf(action, sizeof action, "%s", panel_buttons[i].action);
            run_settings_action(action);
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------- help

static void draw_help(int width, int height) {
    overlay_rect(0, 0, (float)width, (float)height, theme->help_background);
    float line_height = overlay_line_height(), char_width = overlay_char_width();
    const char *title = "hydra-native: live coding with hydra-synth. F1 or Escape closes this help. [name]: for \"keybindings\" in settings.json";
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
    if (toolbar_click(x, y, modifiers)) return;
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

void ui_wheel(int lines) {
    if (!visible) return;
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
static bool is_identifier_char(unsigned char c) { return isalnum(c) || c == '_' || c == '$'; }

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
static void draw_bar(int width, int height, const char *text, uint32_t color, size_t selected_from, size_t selected_to) {
    float y = (float)height - bar_height();
    overlay_rect(0, y, (float)width, bar_height(), theme->bar_background);
    if (selected_to > selected_from)
        overlay_rect(MARGIN + (float)selected_from * overlay_char_width(), y + BAR_PADDING,
                     (float)(selected_to - selected_from) * overlay_char_width(), overlay_line_height(), theme->selection);
    overlay_text(MARGIN, y + BAR_PADDING, text, strlen(text), 0, visible_columns(), NULL, color);
}

// editor.renderWhitespace of VS Code: whether the space or tab at `offset` shows.
static bool whitespace_shown(size_t line_start, size_t line_end, size_t offset, const struct selection *selections, size_t selection_count) {
    switch (active_settings.render_whitespace) {
    case WHITESPACE_NONE: return false;
    case WHITESPACE_ALL: return true;
    case WHITESPACE_TRAILING: {
        size_t end = offset;
        while (end < line_end && (editor.text[end] == ' ' || editor.text[end] == '\t')) end++;
        return end == line_end;
    }
    case WHITESPACE_SELECTION:
        for (size_t i = 0; i < selection_count; i++) {
            size_t start = selections[i].cursor < selections[i].anchor ? selections[i].cursor : selections[i].anchor;
            size_t end = selections[i].cursor < selections[i].anchor ? selections[i].anchor : selections[i].cursor;
            if (offset >= start && offset < end) return true;
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

// A middle-height dot for a space, an arrow for a tab.
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

// Rectangles over the cells of a right-to-left row whose text starts in [start, end).
static void cell_rects(float y, const struct editor_cell *cells, size_t count, size_t start, size_t end, uint32_t color) {
    for (size_t i = 0; i < count; i++)
        if (cells[i].start >= start && cells[i].start < end) column_rect(y, cells[i].column, cells[i].column + cells[i].width, color);
}

// A row with right-to-left text, drawn in the visual order of the full bidi algorithm, as the browser draws the
// line of CodeMirror 5: left-to-right runs on the grid, right-to-left runs shaped (Arabic joins) in their cells.
static void draw_bidi_row(size_t line, size_t row_start, size_t row_end, size_t indent, float y, const struct selection *selections,
                          size_t selection_count, long bracket, long bracket_self) {
    size_t line_start = editor_line_start(&editor, line);
    bool last_row = row_end == editor_line_end(&editor, line);
    struct editor_cell *cells;
    size_t count = editor_row_cells(&editor, line, row_start, row_end, indent, &cells);
    size_t row_columns = count ? cells[count - 1].column + cells[count - 1].width - indent : 0;
    if (row_columns > 0) column_rect(y, indent, indent + row_columns, theme->line_background);
    if (now < flash_until && flash_start != flash_end && flash_start <= row_end && flash_end >= row_start)
        column_rect(y, indent, indent + (row_columns > 0 ? row_columns : 1), theme->flash);
    for (size_t i = 0; i < selection_count; i++) {
        size_t start = selections[i].cursor < selections[i].anchor ? selections[i].cursor : selections[i].anchor;
        size_t end = selections[i].cursor < selections[i].anchor ? selections[i].anchor : selections[i].cursor;
        if (start == end) continue;
        cell_rects(y, cells, count, start, end, theme->selection);
        if (last_row && end > row_end && start <= row_end) column_rect(y, indent + row_columns, indent + row_columns + 1, theme->selection);
    }
    for (size_t i = 0; i < match_count; i++)
        cell_rects(y, cells, count, matches[i].start, matches[i].end, i == active_match ? theme->match_active : theme->match);
    if (bracket >= 0)
        for (int k = 0; k < 2; k++) {
            size_t position = (size_t)(k ? bracket_self : bracket);
            cell_rects(y, cells, count, position, position + 1, theme->bracket);
        }
    for (size_t i = 0; i < count; i++) {
        char c = editor.text[cells[i].start];
        if ((c == ' ' || c == '\t') && whitespace_shown(line_start, editor_line_end(&editor, line), cells[i].start, selections, selection_count))
            draw_whitespace(cells[i].column, y, c == '\t');
    }
    for (size_t i = 0; i < count;) {
        size_t end = i + 1, first = cells[i].start, last = cells[i].end;
        while (end < count && cells[end].level == cells[i].level) {
            if (cells[end].start < first) first = cells[end].start;
            if (cells[end].end > last) last = cells[end].end;
            end++;
        }
        size_t column = cells[i].column, width = cells[end - 1].column + cells[end - 1].width - column;
        if (column >= scroll_column) {
            float x = MARGIN + (float)(column - scroll_column) * overlay_char_width();
            if (cells[i].level % 2)
                overlay_text_fitted(x, y, editor.text + first, last - first, (float)width * overlay_char_width(), colors[first - line_start]);
            else
                overlay_text(x, y, editor.text + first, last - first, 0, width, colors + (first - line_start), theme->text);
        }
        i = end;
    }
    free(cells);
}

// Draws one visual row: the bytes [start, end) of `line`, shown from column `indent` on.
static void draw_row(size_t line, size_t row_start, size_t row_end, size_t indent, float y,
                     const struct selection *selections, size_t selection_count, long bracket, long bracket_self) {
    if (editor_line_has_rtl(&editor, line))
        return draw_bidi_row(line, row_start, row_end, indent, y, selections, selection_count, bracket, bracket_self);
    size_t line_start = editor_line_start(&editor, line);
    size_t row_columns = columns_between(row_start, row_end);
    bool last_row = row_end == editor_line_end(&editor, line);
    size_t limit = scroll_column + visible_columns();
    size_t from = indent > scroll_column ? indent : scroll_column, to = indent + row_columns < limit ? indent + row_columns : limit;
    if (row_columns > 0 && to > from) {
        float x = MARGIN + (float)(from - scroll_column) * overlay_char_width();
        overlay_rect(x - TEXT_PADDING, y, (float)(to - from) * overlay_char_width() + 2 * TEXT_PADDING, overlay_line_height(),
                     theme->line_background);
    }
    if (now < flash_until && flash_start != flash_end && flash_start <= row_end && flash_end >= row_start)
        column_rect(y, indent, indent + (row_columns > 0 ? row_columns : 1), theme->flash);
    for (size_t i = 0; i < selection_count; i++) {
        size_t start = selections[i].cursor < selections[i].anchor ? selections[i].cursor : selections[i].anchor;
        size_t end = selections[i].cursor < selections[i].anchor ? selections[i].anchor : selections[i].cursor;
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
    if (active_settings.render_whitespace != WHITESPACE_NONE) {
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
    overlay_text(MARGIN + (float)shift * overlay_char_width(), y, editor.text + row_start, row_end - row_start, skip,
                 columns, colors + (row_start - line_start), theme->text);
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
        size_t x, row = global_caret_row(selections[i].cursor, selections[i].sticky, &x);
        if (row < scroll_row || row >= scroll_row + rows || x < scroll_column || x > scroll_column + visible_columns())
            continue;
        overlay_rect(MARGIN + (float)(x - scroll_column) * overlay_char_width() - CURSOR_WIDTH / 2,
                     MARGIN + (float)(row - scroll_row) * line_height, CURSOR_WIDTH, line_height, theme->cursor);
    }
    // Bookmarks: a mark in the left margin of the first row of the line.
    for (size_t i = 0; i < editor.bookmark_count; i++) {
        size_t line = editor.bookmarks[i];
        if (line >= layout.line_count || layout.first_row[line] < scroll_row || layout.first_row[line] >= scroll_row + rows)
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
// no room below, as in VS Code), composited with blend mode difference: over black it shows a blue box with white
// text, over anything else it inverts what is below.
static void draw_completion_list(void) {
    size_t x, row = global_caret_row(editor.cursor, editor.sticky, &x);
    if (row < scroll_row || row >= scroll_row + visible_rows()) return;
    float char_width = overlay_char_width(), line_height = overlay_line_height();
    size_t width = 0;
    for (size_t i = 0; i < completion.count; i++)
        if (strlen(completion.items[i]) > width) width = strlen(completion.items[i]);
    float box_width = (float)(width + 2) * char_width, box_height = (float)completion.count * line_height;
    size_t word_column = x - scroll_column > completion.prefix_length ? x - scroll_column - completion.prefix_length : 0;
    float left = MARGIN + (float)word_column * char_width - char_width; // the text starts one column into the box
    if (left < 0) left = 0;
    float top = MARGIN + (float)(row - scroll_row + 1) * line_height;
    if (top + box_height > (float)last_height - bar_height()) top -= box_height + line_height;
    overlay_difference_begin();
    for (size_t i = 0; i < completion.count; i++) {
        float y = top + (float)i * line_height;
        overlay_rect(left, y, box_width, line_height, i == completion.selected ? COMPLETION_SELECTED_COLOR : COMPLETION_BOX_COLOR);
        overlay_text(left + char_width, y, completion.items[i], strlen(completion.items[i]), 0, width, NULL, COMPLETION_TEXT_COLOR);
    }
    overlay_difference_end(left, top, box_width, box_height);
}

// Ghost text: the rest of the selected candidate after the cursor, in the dim ghost color of the theme.
static void draw_completion_ghost(void) {
    size_t x, row = global_caret_row(editor.cursor, editor.sticky, &x);
    if (row < scroll_row || row >= scroll_row + visible_rows() || x < scroll_column) return;
    const char *rest = completion.items[completion.selected] + completion.prefix_length;
    float left = MARGIN + (float)(x - scroll_column) * overlay_char_width(), top = MARGIN + (float)(row - scroll_row) * overlay_line_height();
    size_t columns = overlay_text_columns(rest, strlen(rest));
    overlay_rect(left, top, (float)columns * overlay_char_width() + TEXT_PADDING, overlay_line_height(), theme->line_background);
    overlay_text(left, top, rest, strlen(rest), 0, visible_columns() - (x - scroll_column), NULL, theme->ghost);
}

// Signature help above the cursor line (below it on the first visible row): the call with its parameters and
// defaults, the current parameter marked, and that parameter with its type. The list style draws it like the
// autocomplete list, with blend mode difference; the ghost style in the ghost color with the parameter in the text color.
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
        snprintf(detail, sizeof detail, "%s: %s%s%s%s", input->name, input->type, signature_input_is_texture(input) ? " (texture)" : "",
                 input->default_value[0] ? ", default " : "", input->default_value);
    } else {
        snprintf(detail, sizeof detail, "%s takes %zu arguments", signature->name, signature->input_count);
    }
    size_t x, row = global_caret_row(editor.cursor, editor.sticky, &x);
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
    if (active_settings.autocomplete_style == AUTOCOMPLETE_GHOST) {
        overlay_rect(left, top, box_width, box_height, theme->line_background);
        overlay_text(text_left, top, label, strlen(label), 0, columns, NULL, theme->ghost);
        if (active_end > active_start)
            overlay_text(active_left, top, label + active_start, active_end - active_start, 0, columns, NULL, theme->text);
        overlay_text(text_left, top + line_height, detail, strlen(detail), 0, columns, NULL, theme->ghost);
        return;
    }
    overlay_difference_begin();
    overlay_rect(left, top, box_width, box_height, COMPLETION_BOX_COLOR);
    if (active_end > active_start) overlay_rect(active_left, top, active_width, line_height, COMPLETION_SELECTED_COLOR);
    overlay_text(text_left, top, label, strlen(label), 0, columns, NULL, COMPLETION_TEXT_COLOR);
    overlay_text(text_left, top + line_height, detail, strlen(detail), 0, columns, NULL, COMPLETION_TEXT_COLOR);
    overlay_difference_end(left, top, box_width, box_height);
}

static void draw_completion(void) {
    if (!completion.open) return;
    if (active_settings.autocomplete_style == AUTOCOMPLETE_GHOST) draw_completion_ghost();
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
    case PROMPT_FIND: snprintf(text, sizeof text, "find: %s%s   (Enter: next, Shift+Enter: previous)", prompt.query, count); break;
    case PROMPT_REPLACE:
        snprintf(text, sizeof text, "find: %s%s%s   replace: %s%s   (Tab: field, Enter, Ctrl+Alt+Enter: all)", prompt.query,
                 prompt.field == 0 ? "_" : "", count, prompt.replacement, prompt.field == 1 ? "_" : "");
        break;
    case PROMPT_GO_TO_LINE: snprintf(text, sizeof text, "go to line: %s", prompt.query); break;
    case PROMPT_SERVER: snprintf(text, sizeof text, "gallery and patch bay server URL: %s   (Enter: OK, empty: off)", prompt.query); break;
    case PROMPT_SHARE: {
        float line_height = overlay_line_height(), y = (float)height - bar_height();
        size_t lines = sizeof share_confirmation / sizeof share_confirmation[0], columns = visible_columns();
        float top = y - (float)(lines + 1) * line_height;
        overlay_rect(0, top, (float)width, (float)(lines + 1) * line_height, theme->popup_background);
        (void)columns;
        for (size_t i = 0; i < lines; i++)
            overlay_text_shaped(MARGIN, top + (float)i * line_height + line_height / 2, share_confirmation[i],
                                strlen(share_confirmation[i]), false, theme->text);
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
            overlay_rect(0, row_y, (float)width, line_height, shown == prompt.palette_index ? theme->popup_selected : theme->popup_background);
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

void ui_set_server(const char *url) { set_server(url); }

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

void ui_set_code(const char *code) { editor_set_text(&editor, code, strlen(code)); }

const char *ui_code(void) { return editor.text; }

bool ui_init(const struct settings *stored, const struct settings *active, const char *settings_path,
             const char *emoji_font_path, const char *path, const struct ui_host *ui_host) {
    host = *ui_host;
    stored_settings = *stored;
    active_settings = *active;
    settings_file = settings_path ? strdup(settings_path) : NULL;
    char error[SETTINGS_TEXT] = "";
    load_themes(&active_settings, error, sizeof error);
    theme = find_theme(active->theme);
    if (!theme) {
        if (!error[0]) snprintf(error, sizeof error, "no theme %s, using %s", active->theme, themes[0].name);
        theme = &themes[0];
    }
    // The "keybindings" of the settings file: command name to key, over the table of src/keymap.c.
    for (size_t i = 0; i < active->keybinding_count; i++) {
        char problem[SETTINGS_TEXT];
        if (!keymap_bind(active->keybindings[i].key, active->keybindings[i].value, problem, sizeof problem) && !error[0])
            snprintf(error, sizeof error, "%s", problem);
    }
    if (error[0]) ui_show_message(error, true);
    wrap = active->word_wrap;
    editor_init(&editor);
    if (path) {
        file_path = strdup(path);
        size_t length;
        char *text = read_whole_file(path, &length);
        if (text) editor_set_text(&editor, text, length);
        free(text);
    }
    return overlay_init(active->font, emoji_font_path, active->font_size);
}
