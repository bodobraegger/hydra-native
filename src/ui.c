#define _GNU_SOURCE
#include "ui.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#include "editor.h"
#include "keymap.h"
#include "overlay.h"
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
#define MAX_COMPLETIONS 12
#define MIN_COMPLETION_PREFIX 2
#define MAX_PALETTE_ROWS 12
#define MAX_ROWS_PER_LINE 1024

#define COLOR_LINE_BACKGROUND 0x000000B8u
#define COLOR_SELECTION 0x3D6FD6C0u
#define COLOR_FLASH 0xFFFFFF70u
#define COLOR_BRACKET 0xFFFFFF55u
#define COLOR_CURSOR 0xFFFFFFFFu
#define COLOR_TEXT 0xF2F2F2FFu
#define COLOR_KEYWORD 0xFF7AC6FFu
#define COLOR_NUMBER 0xFFB86CFFu
#define COLOR_STRING 0xB5E655FFu
#define COLOR_COMMENT 0xA0A0A0FFu
#define COLOR_FUNCTION 0x6FDCF2FFu
#define COLOR_BAR_BACKGROUND 0x000000D8u
#define COLOR_ERROR 0xFF6B6BFFu
#define COLOR_INFO 0xE6E6E6FFu
#define COLOR_TOOLBAR 0x000000B8u
#define COLOR_POPUP_BACKGROUND 0x101010F0u
#define COLOR_POPUP_SELECTED 0x3D6FD6FFu
#define COLOR_HELP_BACKGROUND 0x000000E0u
#define COLOR_HELP_KEYS 0x6FDCF2FFu
#define COLOR_BOOKMARK 0x6FDCF2FFu
// Bracket pair colors by nesting depth, like VS Code.
static const uint32_t bracket_colors[] = { 0xFFD700FFu, 0xDA70D6FFu, 0x179FFFFFu };
// The text of clear() in the web editor.
#define CLEARED_CODE "\n \n // Type some code on a new line (such as \"osc().out()\"), and press CTRL+shift+enter"

static const char *const keywords[] = {
    "const", "let", "var", "function", "return", "if", "else", "for", "while", "do", "new", "true", "false",
    "null", "undefined", "this", "of", "in", "typeof", "async", "await", "class", "break", "continue", "switch",
    "case", "default", "try", "catch", "throw",
};

enum prompt_kind { PROMPT_NONE, PROMPT_FIND, PROMPT_REPLACE, PROMPT_GO_TO_LINE, PROMPT_PALETTE, PROMPT_SHARE };

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
    size_t palette_index;
} prompt;

static struct {
    bool open;
    char *items[MAX_COMPLETIONS];
    size_t count;
    size_t selected;
    size_t prefix_length;
} completion;

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
    editor_visual_position(&editor, offset, &row, x);
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

static void evaluate_range(size_t start, size_t end) {
    flash_start = start;
    flash_end = end;
    flash_until = now + FLASH_MS;
    if (start == end) return;
    char *code = strndup(editor.text + start, end - start);
    char *error = evaluate_code(code, end - start, "<editor>");
    show_result("", error);
    free(error);
    free(code);
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

// ---------------------------------------------------------------- autocomplete

static void close_completion(void) {
    for (size_t i = 0; i < completion.count; i++) free(completion.items[i]);
    completion.count = 0;
    completion.open = false;
}

static bool add_completion(const char *word, size_t length) {
    for (size_t i = 0; i < completion.count; i++)
        if (strlen(completion.items[i]) == length && !strncmp(completion.items[i], word, length)) return true;
    if (completion.count == MAX_COMPLETIONS) return false;
    completion.items[completion.count++] = strndup(word, length);
    return true;
}

// Hydra names first (from __completionNames in js/main.js), then the identifiers of the file.
static void open_completion(bool forced) {
    close_completion();
    size_t start, length = editor_word_before_cursor(&editor, &start);
    if (length < (forced ? 0 : MIN_COMPLETION_PREFIX) || editor_has_selection(&editor)) return;
    const char *prefix = editor.text + start;
    char *names = host.call("__completionNames", "");
    for (char *name = names ? strtok(names, "\n") : NULL; name; name = strtok(NULL, "\n"))
        if (strlen(name) > length && !strncmp(name, prefix, length) && !add_completion(name, strlen(name))) break;
    free(names);
    char **words;
    size_t count = editor_words(&editor, prefix, length, &words);
    for (size_t i = 0; i < count; i++) {
        add_completion(words[i], strlen(words[i]));
        free(words[i]);
    }
    free(words);
    completion.prefix_length = length;
    completion.selected = 0;
    completion.open = completion.count > 0;
}

static void accept_completion(void) {
    const char *item = completion.items[completion.selected];
    size_t start, length = editor_word_before_cursor(&editor, &start);
    if (strlen(item) > length) editor_insert(&editor, item + length, strlen(item) - length, EDIT_OTHER);
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
        // Start with the selected text, as editors do.
        size_t start, end;
        editor_selection(&editor, &start, &end);
        prompt.query[0] = '\0';
        if (end > start && end - start < MAX_PROMPT && !memchr(editor.text + start, '\n', end - start))
            snprintf(prompt.query, sizeof prompt.query, "%.*s", (int)(end - start), editor.text + start);
        prompt.field = 0;
    } else {
        prompt.query[0] = '\0';
    }
    prompt.palette_index = 0;
    prompt.kind = kind;
}

static char *prompt_field(void) { return prompt.kind == PROMPT_REPLACE && prompt.field == 1 ? prompt.replacement : prompt.query; }

static void find(int direction) {
    if (!prompt.query[0]) return;
    if (!editor_find(&editor, prompt.query, strlen(prompt.query), direction)) ui_show_message("not found", false);
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
        else editor_replace_next(&editor, prompt.query, strlen(prompt.query), prompt.replacement, strlen(prompt.replacement));
        break;
    case PROMPT_GO_TO_LINE: {
        long line = atol(prompt.query);
        if (line > 0) editor_go_to_line(&editor, (size_t)line - 1);
        prompt.kind = PROMPT_NONE;
        break;
    }
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

static void change_font_size(float step) {
    if (overlay_set_font_size(overlay_font_size() + step)) {
        char text[64];
        snprintf(text, sizeof text, "font size %.0f px", overlay_font_size());
        ui_show_message(text, false);
        layout.valid = false;
    }
}

// Formats with js-beautify, like the web editor (see __format in js/main.js).
static void format_code(void) {
    char *formatted = host.call("__format", editor.text);
    if (!formatted) return ui_show_message("format: the code has an error", true);
    editor_replace_text(&editor, formatted, strlen(formatted));
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
}

// Like "save to URL" of the web editor; without an address bar, the URL goes to the clipboard.
static void save_to_url(void) {
    char *url = host.call("__urlForCode", editor.text);
    if (!url) return;
    SDL_SetClipboardText(url);
    ui_show_message("copied the hydra.ojack.xyz URL of the code to the clipboard", false);
    free(url);
}

static void escape(void) {
    if (completion.open) close_completion();
    else if (prompt.kind != PROMPT_NONE) prompt.kind = PROMPT_NONE;
    else if (help_visible) help_visible = false;
    else if (info_visible) info_visible = false;
    else if (extensions_visible) extensions_visible = false;
    else if (visible && editor_selection_count(&editor) > 1) editor_collapse_to_first(&editor);
    else if (visible && editor_has_selection(&editor)) editor_set_cursor(&editor, editor.cursor, false);
    else if (host.is_fullscreen()) host.toggle_fullscreen();
}

static void prompt_backspace(void) {
    char *field = prompt_field();
    size_t length = strlen(field);
    while (length > 0 && (field[--length] & 0xC0) == 0x80) {}
    field[length] = '\0';
    prompt_changed();
}

static void run_command(enum command command, bool extend) {
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
    case COMMAND_EVAL_ALL: evaluate_range(0, editor.length); break;
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
        if (replace_with("__randomExample", "no example")) evaluate_range(0, editor.length);
        break;
    // The mutator evaluates the code itself and retries on errors, like in the web editor.
    case COMMAND_MUTATE: replace_with("__mutate", "mutator: the code does not parse"); break;
    case COMMAND_MUTATE_TRANSFORM: replace_with("__mutateTransform", "mutator: the code does not parse"); break;
    case COMMAND_MUTATE_UNDO: replace_with("__mutateUndo", "mutator: nothing to undo"); break;
    case COMMAND_TOGGLE_HELP: help_visible = !help_visible; return;
    case COMMAND_TOGGLE_EXTENSIONS:
        extensions_visible = !extensions_visible;
        info_visible = false;
        info_scroll = 0;
        return;
    case COMMAND_TOGGLE_INFO:
        info_visible = !info_visible;
        extensions_visible = false;
        info_scroll = 0;
        return;
    case COMMAND_SELECT_NEXT_OCCURRENCE: editor_select_next_occurrence(&editor); break;
    case COMMAND_FIND: open_prompt(PROMPT_FIND); break;
    case COMMAND_REPLACE: open_prompt(PROMPT_REPLACE); break;
    case COMMAND_GO_TO_LINE: open_prompt(PROMPT_GO_TO_LINE); break;
    case COMMAND_COMMAND_PALETTE: open_prompt(PROMPT_PALETTE); return;
    case COMMAND_FIND_NEXT: find(1); break;
    case COMMAND_FIND_PREVIOUS: find(-1); break;
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
        chord_prefix = SDLK_k;
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
        size_t count = editor_replace_all(&editor, prompt.query, strlen(prompt.query), prompt.replacement, strlen(prompt.replacement));
        char text[64];
        snprintf(text, sizeof text, "%zu replaced", count);
        ui_show_message(text, false);
        break;
    }
    case COMMAND_LEFT: editor_move(&editor, MOTION_LEFT, extend); break;
    case COMMAND_RIGHT: editor_move(&editor, MOTION_RIGHT, extend); break;
    case COMMAND_UP: editor_move(&editor, MOTION_UP, extend); break;
    case COMMAND_DOWN: editor_move(&editor, MOTION_DOWN, extend); break;
    case COMMAND_WORD_LEFT: editor_move(&editor, MOTION_WORD_LEFT, extend); break;
    case COMMAND_WORD_RIGHT: editor_move(&editor, MOTION_WORD_RIGHT, extend); break;
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
    if (completion.open) return CONTEXT_COMPLETION;
    if (prompt.kind != PROMPT_NONE) return CONTEXT_PROMPT;
    return CONTEXT_ANY;
}

bool ui_key(const SDL_KeyboardEvent *event) {
    suppress_text = false;
    bool extend = false;
    SDL_Keycode key = event->keysym.sym;
    if (key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LSHIFT || key == SDLK_RSHIFT || key == SDLK_LALT ||
        key == SDLK_RALT)
        return false; // a modifier alone keeps a pending two-key command
    SDL_Keycode prefix = chord_prefix;
    chord_prefix = 0;
    const struct key_binding *binding =
        keymap_lookup(key, (SDL_Keymod)event->keysym.mod, visible ? key_context() : CONTEXT_ANY, prefix, &extend);
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
    if (suppress_text || !visible || (SDL_GetModState() & (KMOD_CTRL | KMOD_LALT))) return;
    if (help_visible) return;
    if (prompt.kind != PROMPT_NONE) {
        char *field = prompt_field();
        size_t length = strlen(field);
        snprintf(field + length, MAX_PROMPT - length, "%s", text);
        prompt_changed();
        reveal_cursor();
        return;
    }
    editor_type(&editor, text, strlen(text));
    bool word = isalnum((unsigned char)text[0]) || text[0] == '_' || text[0] == '$';
    if (word || completion.open) open_completion(false);
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
    { "?", "toolbar.show-info", COMMAND_TOGGLE_INFO, COMMAND_TOGGLE_HELP, COMMAND_TOGGLE_HELP },
};
#define TOOLBAR_COUNT (sizeof toolbar / sizeof toolbar[0])
static bool gallery_enabled; // the web editor shows "share" only with a gallery server

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
        run_command(modifiers & KMOD_SHIFT ? item->with_shift : modifiers & KMOD_CTRL ? item->with_ctrl : item->command,
                    false);
        return true;
    }
    return false;
}

static void draw_toolbar(void) {
    for (size_t i = 0; i < TOOLBAR_COUNT; i++) {
        if (!toolbar_shown(i)) continue;
        float width, x = toolbar_x(i, &width);
        overlay_rect(x, MARGIN, width, overlay_line_height(), COLOR_TOOLBAR);
        overlay_text(x + overlay_char_width() / 2, MARGIN, toolbar[i].label, strlen(toolbar[i].label), 0, 64, NULL,
                     COLOR_INFO);
        bool hover = pointer_y >= MARGIN && pointer_y <= MARGIN + overlay_line_height() && pointer_x >= x &&
                     pointer_x <= x + width;
        if (!hover) continue;
        const char *title = toolbar[i].command == COMMAND_TOGGLE_INFO && info_visible ? "toolbar.hide-info" : toolbar[i].title;
        char *text = host.call("__translate", title);
        if (!text) continue;
        size_t columns = overlay_text_columns(text, strlen(text));
        float tooltip_width = (float)columns * overlay_char_width();
        float left = x + width - tooltip_width;
        if (left < MARGIN) left = MARGIN;
        float top = MARGIN + overlay_line_height() + TEXT_PADDING;
        overlay_rect(left - TEXT_PADDING, top, tooltip_width + 2 * TEXT_PADDING, overlay_line_height(), COLOR_POPUP_BACKGROUND);
        overlay_text(left, top, text, strlen(text), 0, columns, NULL, COLOR_TEXT);
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

static void draw_info(void) {
    float left, top, width, height, line_height = overlay_line_height(), char_width = overlay_char_width();
    info_panel(&left, &top, &width, &height);
    overlay_rect(left, top, width, height, COLOR_HELP_BACKGROUND);
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
        size_t columns = overlay_text_columns(name, strlen(name)) + 1;
        if (x + (float)columns * char_width > left + width) break;
        snprintf(language_buttons[language_button_count].code, sizeof language_buttons[0].code, "%s", line);
        language_buttons[language_button_count].x = x;
        language_buttons[language_button_count++].width = (float)columns * char_width;
        overlay_rect(x, top + line_height / 2, (float)columns * char_width, line_height,
                     selected && *selected == '1' ? COLOR_POPUP_SELECTED : COLOR_TOOLBAR);
        overlay_text(x + char_width / 2, top + line_height / 2, name, strlen(name), 0, columns, NULL, COLOR_TEXT);
        x += (float)(columns + 1) * char_width;
    }
    free(languages);
    char columns_text[16];
    size_t text_columns = (size_t)(width / char_width) - 4;
    snprintf(columns_text, sizeof columns_text, "%zu", text_columns);
    char *text = host.call("__infoText", columns_text);
    if (!text) return;
    char *saveptr;
    char *direction = strtok_r(text, "\n", &saveptr);
    bool rtl = direction && !strcmp(direction, "rtl");
    size_t rows = (size_t)((height - 3 * line_height) / line_height), row = 0, skipped = 0;
    // strtok_r skips empty lines, so the paragraphs are split by hand.
    for (char *line = saveptr; line && *line != '\0' && row < rows;) {
        char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (skipped++ >= info_scroll) {
            size_t columns = overlay_text_columns(line, length);
            float line_x = rtl ? left + width - 2 * char_width - (float)columns * char_width : left + 2 * char_width;
            overlay_text(line_x, top + line_height * (2.0f + (float)row), line, length, 0, text_columns, NULL, COLOR_TEXT);
            row++;
        }
        line = end ? end + 1 : NULL;
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
    overlay_rect(x, y, width, overlay_line_height(), selected ? COLOR_POPUP_SELECTED : COLOR_TOOLBAR);
    overlay_text(x + overlay_char_width() / 2, y, text, strlen(text), 0, columns, NULL, COLOR_TEXT);
    add_panel_button(x, y, width, overlay_line_height(), action);
    return x + width + overlay_char_width();
}

// The extensions window of the web editor (src/views/extension-info.js): categories, then per entry a thumbnail,
// "name - author", the description, and buttons to add its load code, open its documentation and run its examples.
static void draw_extensions(void) {
    float left, top, width, height, line_height = overlay_line_height(), char_width = overlay_char_width();
    info_panel(&left, &top, &width, &height);
    overlay_rect(left, top, width, height, COLOR_HELP_BACKGROUND);
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
            overlay_text(left + char_width, y + 1.5f * line_height, state, strlen(state), 0, text_columns, NULL, COLOR_INFO);
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
            overlay_text(text_left, entry_top, title, strlen(title), 0, text_columns, NULL, COLOR_HELP_KEYS);
            overlay_text(text_left, entry_top + line_height, fields[3], strlen(fields[3]), 0, text_columns, NULL, COLOR_TEXT);
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

// Opens a URL in the browser of the desktop, like window.open() in the web editor.
static void open_in_browser(const char *url) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(127);
    }
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
        open_in_browser(result);
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

// ---------------------------------------------------------------- help

static void draw_help(int width, int height) {
    overlay_rect(0, 0, (float)width, (float)height, COLOR_HELP_BACKGROUND);
    float line_height = overlay_line_height(), char_width = overlay_char_width();
    const char *title = "hydra-native: live coding with hydra-synth. F1 or Escape closes this help.";
    overlay_text(MARGIN, MARGIN, title, strlen(title), 0, visible_columns(), NULL, COLOR_TEXT);
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
        overlay_text(x, y, keys, strlen(keys), 0, keys_width - 1, NULL, COLOR_HELP_KEYS);
        const char *description = command_description(command);
        overlay_text(x + (float)keys_width * char_width, y, description, strlen(description), 0,
                     column_width - keys_width - 1, NULL, COLOR_TEXT);
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
    close_completion();
    size_t offset = offset_at_point(x, y);
    mouse.dragging = true;
    mouse.column = (modifiers & KMOD_ALT) && (modifiers & KMOD_SHIFT);
    if (mouse.column) {
        size_t line, row, column;
        point_to_position(x, y, &line, &row, &column);
        mouse.anchor_line = line;
        mouse.anchor_column = editor_column_of(&editor, offset);
    } else if (modifiers & KMOD_ALT) {
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
        if (!(modifiers & KMOD_SHIFT)) editor_collapse(&editor);
        editor_set_cursor(&editor, offset, modifiers & KMOD_SHIFT);
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
    if (info_visible || extensions_visible) {
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
        uint32_t color = COLOR_TEXT;
        unsigned char c = (unsigned char)text[i];
        char next = i + 1 < length ? text[i + 1] : '\0';
        if (state->block_comment) {
            color = COLOR_COMMENT;
            while (i < length && !(text[i] == '*' && i + 1 < length && text[i + 1] == '/')) i++;
            if (i < length) { i += 2; state->block_comment = false; }
        } else if (state->template_string || c == '\'' || c == '"' || c == '`') {
            char quote = state->template_string ? '`' : (char)c;
            color = COLOR_STRING;
            if (!state->template_string) i++;
            state->template_string = false;
            while (i < length && text[i] != quote) i += text[i] == '\\' ? 2 : 1;
            if (i < length) i++;
            else if (quote == '`') state->template_string = true;
            if (i > length) i = length;
        } else if (c == '/' && next == '/') {
            color = COLOR_COMMENT;
            i = length;
        } else if (c == '/' && next == '*') {
            state->block_comment = true;
            i += 2;
            if (out) out[start] = out[start + 1] = COLOR_COMMENT;
            continue;
        } else if (isdigit(c) || (c == '.' && isdigit((unsigned char)next))) {
            color = COLOR_NUMBER;
            while (i < length && (isalnum((unsigned char)text[i]) || text[i] == '.')) i++;
        } else if (is_identifier_start(c)) {
            while (i < length && is_identifier_char((unsigned char)text[i])) i++;
            size_t after = i;
            while (after < length && text[after] == ' ') after++;
            if (is_keyword(text + start, i - start)) color = COLOR_KEYWORD;
            else if (after < length && text[after] == '(') color = COLOR_FUNCTION;
        } else if (strchr("([{", c)) {
            color = bracket_colors[state->bracket_depth++ % 3];
            i++;
        } else if (strchr(")]}", c)) {
            if (state->bracket_depth > 0) state->bracket_depth--;
            color = bracket_colors[state->bracket_depth % 3];
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

static size_t columns_between(size_t start, size_t end) {
    size_t columns = 0;
    for (size_t i = start; i < end;) {
        size_t consumed;
        int codepoint = unicode_decode((const unsigned char *)editor.text + i, end - i, &consumed);
        columns += codepoint == '\t' ? EDITOR_TAB_WIDTH - columns % EDITOR_TAB_WIDTH : unicode_is_wide(codepoint) ? 2 : 1;
        i += consumed;
    }
    return columns;
}

// A rectangle over the columns [from, to) of a screen row, clipped to the visible columns.
static void column_rect(float y, size_t from, size_t to, uint32_t color) {
    size_t limit = scroll_column + visible_columns();
    if (to <= scroll_column || from >= limit) return;
    if (from < scroll_column) from = scroll_column;
    if (to > limit) to = limit;
    float x = MARGIN + (float)(from - scroll_column) * overlay_char_width();
    overlay_rect(x, y, (float)(to - from) * overlay_char_width(), overlay_line_height(), color);
}

static void draw_bar(int width, int height, const char *text, uint32_t color) {
    float y = (float)height - bar_height();
    overlay_rect(0, y, (float)width, bar_height(), COLOR_BAR_BACKGROUND);
    overlay_text(MARGIN, y + BAR_PADDING, text, strlen(text), 0, visible_columns(), NULL, color);
}

// Draws one visual row: the bytes [start, end) of `line`, shown from column `indent` on.
static void draw_row(size_t line, size_t row_start, size_t row_end, size_t indent, float y,
                     const struct selection *selections, size_t selection_count, long bracket, long bracket_self) {
    size_t line_start = editor_line_start(&editor, line);
    size_t row_columns = columns_between(row_start, row_end);
    bool last_row = row_end == editor_line_end(&editor, line);
    size_t limit = scroll_column + visible_columns();
    size_t from = indent > scroll_column ? indent : scroll_column, to = indent + row_columns < limit ? indent + row_columns : limit;
    if (row_columns > 0 && to > from) {
        float x = MARGIN + (float)(from - scroll_column) * overlay_char_width();
        overlay_rect(x - TEXT_PADDING, y, (float)(to - from) * overlay_char_width() + 2 * TEXT_PADDING, overlay_line_height(),
                     COLOR_LINE_BACKGROUND);
    }
    if (now < flash_until && flash_start != flash_end && flash_start <= row_end && flash_end >= row_start)
        column_rect(y, indent, indent + (row_columns > 0 ? row_columns : 1), COLOR_FLASH);
    for (size_t i = 0; i < selection_count; i++) {
        size_t start = selections[i].cursor < selections[i].anchor ? selections[i].cursor : selections[i].anchor;
        size_t end = selections[i].cursor < selections[i].anchor ? selections[i].anchor : selections[i].cursor;
        if (start == end || start > row_end || end <= row_start || (start == row_end && !last_row)) continue;
        size_t from = indent + (start > row_start ? columns_between(row_start, start) : 0);
        size_t to = indent + (end > row_end ? row_columns + (last_row ? 1 : 0) : columns_between(row_start, end));
        column_rect(y, from, to, COLOR_SELECTION);
    }
    long positions[2] = { bracket, bracket_self };
    for (int k = 0; k < 2; k++) {
        if (bracket < 0 || positions[k] < (long)row_start || positions[k] >= (long)row_end) continue;
        size_t column = indent + columns_between(row_start, (size_t)positions[k]);
        column_rect(y, column, column + 1, COLOR_BRACKET);
    }
    size_t skip = scroll_column > indent ? scroll_column - indent : 0;
    size_t shift = indent > scroll_column ? indent - scroll_column : 0;
    size_t columns = visible_columns() > shift ? visible_columns() - shift : 0;
    overlay_text(MARGIN + (float)shift * overlay_char_width(), y, editor.text + row_start, row_end - row_start, skip,
                 columns, colors + (row_start - line_start), COLOR_TEXT);
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
                     MARGIN + (float)(row - scroll_row) * line_height, CURSOR_WIDTH, line_height, COLOR_CURSOR);
    }
    // Bookmarks: a mark in the left margin of the first row of the line.
    for (size_t i = 0; i < editor.bookmark_count; i++) {
        size_t line = editor.bookmarks[i];
        if (line >= layout.line_count || layout.first_row[line] < scroll_row || layout.first_row[line] >= scroll_row + rows)
            continue;
        overlay_rect(MARGIN / 4, MARGIN + (float)(layout.first_row[line] - scroll_row) * line_height + line_height / 4,
                     MARGIN / 2, line_height / 2, COLOR_BOOKMARK);
    }
    // The IME composition, underlined at the cursor, until the input method commits it.
    if (composition[0]) {
        size_t x, row = global_row(editor.cursor, &x);
        if (row >= scroll_row && row < scroll_row + rows) {
            float left = MARGIN + (float)(x - scroll_column) * overlay_char_width();
            float top = MARGIN + (float)(row - scroll_row) * line_height;
            size_t length = strlen(composition), columns = 0;
            for (size_t k = 0; k < length; k++) columns += (composition[k] & 0xC0) != 0x80;
            overlay_rect(left, top, (float)columns * overlay_char_width(), line_height, COLOR_POPUP_BACKGROUND);
            overlay_text(left, top, composition, length, 0, columns, NULL, COLOR_TEXT);
            overlay_rect(left, top + line_height - 2, (float)columns * overlay_char_width(), 2, COLOR_TEXT);
        }
    }
    free(selections);
}

static void draw_completion(void) {
    if (!completion.open) return;
    size_t x, row = global_row(editor.cursor, &x);
    if (row < scroll_row || row >= scroll_row + visible_rows()) return;
    float char_width = overlay_char_width(), line_height = overlay_line_height();
    size_t width = 0;
    for (size_t i = 0; i < completion.count; i++)
        if (strlen(completion.items[i]) > width) width = strlen(completion.items[i]);
    float left = MARGIN + ((float)(x - scroll_column) - (float)completion.prefix_length) * char_width;
    float top = MARGIN + (float)(row - scroll_row + 1) * line_height;
    if (top + (float)completion.count * line_height > (float)last_height - bar_height())
        top -= (float)(completion.count + 1) * line_height;
    for (size_t i = 0; i < completion.count; i++) {
        float y = top + (float)i * line_height;
        overlay_rect(left - TEXT_PADDING, y, (float)(width + 1) * char_width + 2 * TEXT_PADDING, line_height,
                     i == completion.selected ? COLOR_POPUP_SELECTED : COLOR_POPUP_BACKGROUND);
        overlay_text(left, y, completion.items[i], strlen(completion.items[i]), 0, width, NULL, COLOR_TEXT);
    }
}

static void draw_prompt(int width, int height) {
    char text[2 * MAX_PROMPT + 128];
    switch (prompt.kind) {
    case PROMPT_FIND: snprintf(text, sizeof text, "find: %s   (Enter: next, Shift+Enter: previous)", prompt.query); break;
    case PROMPT_REPLACE:
        snprintf(text, sizeof text, "find: %s%s   replace: %s%s   (Tab: field, Enter, Ctrl+Alt+Enter: all)", prompt.query,
                 prompt.field == 0 ? "_" : "", prompt.replacement, prompt.field == 1 ? "_" : "");
        break;
    case PROMPT_GO_TO_LINE: snprintf(text, sizeof text, "go to line: %s", prompt.query); break;
    case PROMPT_SHARE: {
        float line_height = overlay_line_height(), y = (float)height - bar_height();
        size_t lines = sizeof share_confirmation / sizeof share_confirmation[0], columns = visible_columns();
        float top = y - (float)(lines + 1) * line_height;
        overlay_rect(0, top, (float)width, (float)(lines + 1) * line_height, COLOR_POPUP_BACKGROUND);
        for (size_t i = 0; i < lines; i++)
            overlay_text(MARGIN, top + (float)i * line_height + line_height / 2, share_confirmation[i],
                         strlen(share_confirmation[i]), 0, columns, NULL, COLOR_TEXT);
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
            overlay_rect(0, row_y, (float)width, line_height, shown == prompt.palette_index ? COLOR_POPUP_SELECTED : COLOR_POPUP_BACKGROUND);
            overlay_text(MARGIN, row_y, row, strlen(row), 0, visible_columns(), NULL, COLOR_TEXT);
            shown++;
        }
        break;
    }
    case PROMPT_NONE: return;
    }
    draw_bar(width, height, text, COLOR_INFO);
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
    draw_code();
    if (info_visible) draw_info();
    if (extensions_visible) draw_extensions();
    draw_toolbar();
    draw_completion();
    if (prompt.kind != PROMPT_NONE) draw_prompt(width, height);
    else if (message[0] && (message_is_error || now < message_until))
        draw_bar(width, height, message, message_is_error ? COLOR_ERROR : COLOR_INFO);
    overlay_end(width, height);
}

// ---------------------------------------------------------------- setup

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

void ui_set_code(const char *code) { editor_set_text(&editor, code, strlen(code)); }

const char *ui_code(void) { return editor.text; }

bool ui_init(const char *font_path, float font_size, const char *path, const struct ui_host *ui_host) {
    host = *ui_host;
    editor_init(&editor);
    if (path) {
        file_path = strdup(path);
        size_t length;
        char *text = read_whole_file(path, &length);
        if (text) editor_set_text(&editor, text, length);
        free(text);
    }
    return overlay_init(font_path, font_size);
}
