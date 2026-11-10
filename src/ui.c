#define _GNU_SOURCE
#include "ui.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "editor.h"
#include "keymap.h"
#include "overlay.h"

#define MARGIN 12.0f
#define TEXT_PADDING 3.0f
#define CURSOR_WIDTH 2.0f
#define BAR_PADDING 4.0f
#define FLASH_MS 250.0
#define INFO_MESSAGE_MS 2000.0
#define CURSOR_BLINK_MS 530.0
#define WHEEL_LINES 3
#define FONT_SIZE_STEP 2.0f
#define MAX_MESSAGE 512
#define MAX_QUERY 256

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
#define COLOR_HELP_BACKGROUND 0x000000E0u
#define COLOR_HELP_KEYS 0x6FDCF2FFu
// The text of clear() in the web editor.
#define CLEARED_CODE "\n \n // Type some code on a new line (such as \"osc().out()\"), and press CTRL+shift+enter"

static const char *const keywords[] = {
    "const", "let", "var", "function", "return", "if", "else", "for", "while", "do", "new", "true", "false",
    "null", "undefined", "this", "of", "in", "typeof", "async", "await", "class", "break", "continue", "switch",
    "case", "default", "try", "catch", "throw",
};

static struct ui_host host;
static struct editor editor;
static char *file_path;
static bool visible = true;
static size_t scroll_line, scroll_column;
static int last_width, last_height;
static size_t flash_start, flash_end;
static double flash_until, message_until, now;
static char message[MAX_MESSAGE];
static bool message_is_error;
static bool searching;
static char query[MAX_QUERY];
static bool evaluating;
static char console_message[MAX_MESSAGE];
static bool suppress_text; // the key press before this text input was a command
static bool dragging;
static bool help_visible;
static double last_activity;
static uint32_t *colors;
static size_t colors_capacity;

// ---------------------------------------------------------------- layout

static size_t visible_rows(void) {
    float height = (float)last_height - 2 * MARGIN - overlay_line_height() - 2 * BAR_PADDING;
    return height > overlay_line_height() ? (size_t)(height / overlay_line_height()) : 1;
}

static size_t visible_columns(void) {
    float width = (float)last_width - 2 * MARGIN;
    return width > overlay_char_width() ? (size_t)(width / overlay_char_width()) : 1;
}

static void reveal_cursor(void) {
    size_t line = editor_line_of(&editor, editor.cursor), column = editor_column_of(&editor, editor.cursor);
    size_t rows = visible_rows(), columns = visible_columns();
    if (line < scroll_line) scroll_line = line;
    if (line >= scroll_line + rows) scroll_line = line - rows + 1;
    if (column < scroll_column) scroll_column = column;
    if (column + 1 >= scroll_column + columns) scroll_column = column + 2 - columns;
    last_activity = now;
}

static size_t offset_at_point(float x, float y) {
    float row = (y - MARGIN) / overlay_line_height();
    float column = (x - MARGIN) / overlay_char_width() + 0.5f;
    size_t line = scroll_line + (row > 0 ? (size_t)row : 0);
    if (line >= editor.line_count) return editor.length;
    return editor_offset_at(&editor, line, scroll_column + (column > 0 ? (size_t)column : 0));
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
    if (!editor_has_selection(&editor)) {
        // Like Pulsar: without a selection, copy and cut take the whole line.
        size_t line = editor_line_of(&editor, editor.cursor);
        editor_set_cursor(&editor, editor_line_start(&editor, line), false);
        size_t end = line + 1 < editor.line_count ? editor_line_start(&editor, line + 1) : editor.length;
        editor_set_cursor(&editor, end, true);
    }
    size_t start, end;
    editor_selection(&editor, &start, &end);
    char *text = strndup(editor.text + start, end - start);
    SDL_SetClipboardText(text);
    free(text);
    if (cut) editor_delete_selection(&editor);
    else editor_set_cursor(&editor, editor.cursor, false);
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

// ---------------------------------------------------------------- commands

static void change_font_size(float step) {
    if (overlay_set_font_size(overlay_font_size() + step)) {
        char text[64];
        snprintf(text, sizeof text, "font size %.0f px", overlay_font_size());
        ui_show_message(text, false);
    }
}

static void find_from_selection_start(void) {
    size_t start, end;
    editor_selection(&editor, &start, &end);
    editor_set_cursor(&editor, start, false);
    if (!editor_find_next(&editor, query, strlen(query))) ui_show_message("not found", false);
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

static void run_command(enum command command, bool extend) {
    size_t start, end;
    long page = (long)visible_rows() - 1;
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
    case COMMAND_ESCAPE:
        if (help_visible) help_visible = false;
        else if (searching) searching = false;
        else if (visible && editor_has_selection(&editor)) editor_set_cursor(&editor, editor.cursor, false);
        else if (host.is_fullscreen()) host.toggle_fullscreen();
        break;
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
    case COMMAND_SELECT_NEXT_OCCURRENCE: editor_select_next_occurrence(&editor); break;
    case COMMAND_FIND:
        if (searching && query[0]) editor_find_next(&editor, query, strlen(query));
        searching = true;
        break;
    case COMMAND_FONT_BIGGER: change_font_size(FONT_SIZE_STEP); break;
    case COMMAND_FONT_SMALLER: change_font_size(-FONT_SIZE_STEP); break;
    case COMMAND_NEWLINE: editor_insert_newline(&editor); break;
    case COMMAND_BACKSPACE: editor_backspace(&editor); break;
    case COMMAND_DELETE: editor_delete_forward(&editor); break;
    case COMMAND_DELETE_WORD_LEFT:
        if (!editor_has_selection(&editor)) editor_move(&editor, MOTION_WORD_LEFT, true);
        editor_delete_selection(&editor);
        break;
    case COMMAND_INDENT: editor_indent(&editor, false); break;
    case COMMAND_OUTDENT: editor_indent(&editor, true); break;
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
        scroll_line = scroll_line > (size_t)page ? scroll_line - (size_t)page : 0;
        break;
    case COMMAND_PAGE_DOWN:
        editor_move_vertical(&editor, page, extend);
        scroll_line += (size_t)page;
        break;
    }
    if (visible) reveal_cursor();
}

static void search_key(SDL_Keycode key) {
    if (key == SDLK_BACKSPACE) {
        size_t length = strlen(query);
        while (length > 0 && (query[--length] & 0xC0) == 0x80) {}
        query[length] = '\0';
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        editor_find_next(&editor, query, strlen(query));
        reveal_cursor();
    }
}

bool ui_key(const SDL_KeyboardEvent *event) {
    suppress_text = false;
    SDL_Keycode key = event->keysym.sym;
    SDL_Keymod modifiers = (SDL_Keymod)event->keysym.mod;
    if (searching && visible && !(modifiers & (KMOD_CTRL | KMOD_LALT)) &&
        (key == SDLK_BACKSPACE || key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
        search_key(key);
        return true;
    }
    bool extend = false;
    const struct key_binding *binding = keymap_lookup(key, modifiers, &extend);
    if (!binding || (!visible && !binding->global)) return false;
    // A held key repeats editing commands, but it does not evaluate or toggle again and again.
    if (event->repeat && binding->global) return true;
    suppress_text = true;
    run_command(binding->command, extend);
    return true;
}

void ui_text_input(const char *text) {
    if (suppress_text || !visible || (SDL_GetModState() & (KMOD_CTRL | KMOD_LALT))) return;
    if (searching) {
        size_t length = strlen(query);
        snprintf(query + length, sizeof query - length, "%s", text);
        find_from_selection_start();
    } else {
        editor_insert(&editor, text, strlen(text), EDIT_TYPING);
    }
    reveal_cursor();
}

// ---------------------------------------------------------------- toolbar

struct toolbar_item {
    const char *label;
    enum command command;
    enum command with_shift; // like Shift+click on the dice of the web editor
    enum command with_ctrl;
};

static const struct toolbar_item toolbar[] = {
    { "run", COMMAND_EVAL_ALL, COMMAND_EVAL_ALL, COMMAND_EVAL_ALL },
    { "clear", COMMAND_CLEAR_ALL, COMMAND_CLEAR_ALL, COMMAND_CLEAR_ALL },
    { "example", COMMAND_SHOW_EXAMPLE, COMMAND_SHOW_EXAMPLE, COMMAND_SHOW_EXAMPLE },
    { "mutate", COMMAND_MUTATE, COMMAND_MUTATE_UNDO, COMMAND_MUTATE_TRANSFORM },
    { "?", COMMAND_TOGGLE_HELP, COMMAND_TOGGLE_HELP, COMMAND_TOGGLE_HELP },
};
#define TOOLBAR_COUNT (sizeof toolbar / sizeof toolbar[0])

// The right-aligned boxes of the toolbar; returns the x of item `index`, and its width in `width`.
static float toolbar_x(size_t index, float *width) {
    float x = (float)last_width - MARGIN;
    for (size_t i = TOOLBAR_COUNT; i-- > index;) {
        *width = ((float)strlen(toolbar[i].label) + 1) * overlay_char_width();
        x -= *width + TEXT_PADDING;
    }
    return x;
}

static bool toolbar_click(float x, float y, SDL_Keymod modifiers) {
    if (y < MARGIN || y > MARGIN + overlay_line_height()) return false;
    for (size_t i = 0; i < TOOLBAR_COUNT; i++) {
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
        float width, x = toolbar_x(i, &width);
        overlay_rect(x, MARGIN, width, overlay_line_height(), COLOR_TOOLBAR);
        overlay_text(x + overlay_char_width() / 2, MARGIN, toolbar[i].label, strlen(toolbar[i].label), 0, 64, NULL,
                     COLOR_INFO);
    }
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
        if (!keys[0]) snprintf(keys, sizeof keys, "(no key)");
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
    bool shift = modifiers & KMOD_SHIFT;
    if (down && help_visible) {
        help_visible = false;
        return;
    }
    if (down && toolbar_click(x, y, modifiers)) return;
    dragging = down;
    if (!down) return;
    size_t offset = offset_at_point(x, y);
    if (clicks == 2) editor_select_word_at(&editor, offset);
    else if (clicks >= 3) {
        size_t line = editor_line_of(&editor, offset);
        editor_set_cursor(&editor, editor_line_start(&editor, line), false);
        editor_set_cursor(&editor, line + 1 < editor.line_count ? editor_line_start(&editor, line + 1) : editor.length, true);
    } else editor_set_cursor(&editor, offset, shift);
    last_activity = now;
}

void ui_mouse_motion(float x, float y) {
    if (!visible || !dragging) return;
    editor_set_cursor(&editor, offset_at_point(x, y), true);
    reveal_cursor();
}

void ui_wheel(int lines) {
    if (!visible) return;
    long target = (long)scroll_line - (long)lines * WHEEL_LINES;
    long last = (long)editor.line_count - 1;
    scroll_line = (size_t)(target < 0 ? 0 : target > last ? last : target);
}

// ---------------------------------------------------------------- highlighting

struct highlight_state {
    bool block_comment;
    bool template_string;
};

static bool is_identifier_start(unsigned char c) { return isalpha(c) || c == '_' || c == '$'; }
static bool is_identifier_char(unsigned char c) { return isalnum(c) || c == '_' || c == '$'; }

static bool is_keyword(const char *word, size_t length) {
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if (strlen(keywords[i]) == length && memcmp(keywords[i], word, length) == 0) return true;
    return false;
}

// Colors one line (one color per byte, when `out` is not NULL) and carries comment and template string state.
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
        } else {
            i++;
        }
        if (out)
            for (size_t k = start; k < i && k < length; k++) out[k] = color;
    }
}

// ---------------------------------------------------------------- drawing

static size_t columns_between(size_t start, size_t end) {
    size_t columns = 0;
    for (size_t i = start; i < end; i++)
        if ((editor.text[i] & 0xC0) != 0x80) columns++;
    return columns;
}

// A rectangle over the columns [from, to) of a row, clipped to the horizontal scroll.
static void column_rect(float y, size_t from, size_t to, uint32_t color) {
    size_t limit = scroll_column + visible_columns();
    if (to <= scroll_column || from >= limit) return;
    if (from < scroll_column) from = scroll_column;
    if (to > limit) to = limit;
    float x = MARGIN + (float)(from - scroll_column) * overlay_char_width();
    overlay_rect(x, y, (float)(to - from) * overlay_char_width(), overlay_line_height(), color);
}

static void draw_bar(int width, int height, const char *text, uint32_t color) {
    float bar_height = overlay_line_height() + 2 * BAR_PADDING;
    float y = (float)height - bar_height;
    overlay_rect(0, y, (float)width, bar_height, COLOR_BAR_BACKGROUND);
    overlay_text(MARGIN, y + BAR_PADDING, text, strlen(text), 0, visible_columns(), NULL, color);
}

static void draw_code(void) {
    float char_width = overlay_char_width(), line_height = overlay_line_height();
    size_t rows = visible_rows(), columns = visible_columns();
    if (scroll_line >= editor.line_count) scroll_line = editor.line_count - 1;
    struct highlight_state state = { 0 };
    for (size_t line = 0; line < scroll_line; line++) {
        size_t start = editor_line_start(&editor, line);
        highlight_line(&state, editor.text + start, editor_line_end(&editor, line) - start, NULL);
    }
    size_t selection_start, selection_end;
    editor_selection(&editor, &selection_start, &selection_end);
    long bracket = editor_has_selection(&editor) ? -1 : editor_matching_bracket(&editor, editor.cursor);
    long bracket_self = -1;
    if (bracket >= 0) {
        bool at_cursor = editor.cursor < editor.length && strchr("()[]{}", editor.text[editor.cursor]);
        bracket_self = at_cursor ? (long)editor.cursor : (long)editor.cursor - 1;
    }
    bool flashing = now < flash_until;

    for (size_t row = 0; row < rows && scroll_line + row < editor.line_count; row++) {
        size_t line = scroll_line + row;
        size_t start = editor_line_start(&editor, line), end = editor_line_end(&editor, line);
        float y = MARGIN + (float)row * line_height;
        size_t line_columns = columns_between(start, end);
        if (line_columns > scroll_column) {
            size_t shown = line_columns - scroll_column < columns ? line_columns - scroll_column : columns;
            overlay_rect(MARGIN - TEXT_PADDING, y, (float)shown * char_width + 2 * TEXT_PADDING, line_height,
                         COLOR_LINE_BACKGROUND);
        }
        if (flashing && flash_start <= end && flash_end >= start && flash_start != flash_end)
            column_rect(y, 0, line_columns > 0 ? line_columns : 1, COLOR_FLASH);
        if (selection_start < selection_end && selection_start <= end && selection_end > start) {
            size_t from = selection_start > start ? columns_between(start, selection_start) : 0;
            size_t to = selection_end > end ? line_columns + 1 : columns_between(start, selection_end);
            column_rect(y, from, to, COLOR_SELECTION);
        }
        for (int k = 0; k < 2; k++) {
            long position = k == 0 ? bracket : bracket_self;
            if (bracket >= 0 && (size_t)position >= start && (size_t)position < end) {
                size_t column = columns_between(start, (size_t)position);
                column_rect(y, column, column + 1, COLOR_BRACKET);
            }
        }
        if (end - start > colors_capacity) {
            colors_capacity = (end - start) * 2;
            colors = realloc(colors, colors_capacity * sizeof *colors);
        }
        highlight_line(&state, editor.text + start, end - start, colors);
        overlay_text(MARGIN, y, editor.text + start, end - start, scroll_column, columns, colors, COLOR_TEXT);
    }

    bool blink_on = fmod(now - last_activity, 2 * CURSOR_BLINK_MS) < CURSOR_BLINK_MS;
    size_t cursor_line = editor_line_of(&editor, editor.cursor);
    size_t cursor_column = editor_column_of(&editor, editor.cursor);
    if (blink_on && cursor_line >= scroll_line && cursor_line < scroll_line + rows && cursor_column >= scroll_column &&
        cursor_column <= scroll_column + columns) {
        float x = MARGIN + (float)(cursor_column - scroll_column) * char_width;
        overlay_rect(x - CURSOR_WIDTH / 2, MARGIN + (float)(cursor_line - scroll_line) * line_height, CURSOR_WIDTH,
                     line_height, COLOR_CURSOR);
    }
}

void ui_draw(int width, int height, double now_ms) {
    now = now_ms;
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
    draw_toolbar();
    if (searching) {
        char text[MAX_QUERY + 64];
        snprintf(text, sizeof text, "find: %s   (Enter: next, Escape: close)", query);
        draw_bar(width, height, text, COLOR_INFO);
    } else if (message[0] && (message_is_error || now < message_until)) {
        draw_bar(width, height, message, message_is_error ? COLOR_ERROR : COLOR_INFO);
    }
    overlay_end(width, height);
}

// ---------------------------------------------------------------- setup

void ui_set_visible(bool show) { visible = show; }

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
