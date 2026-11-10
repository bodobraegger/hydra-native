#include "editor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_UNDO_STEPS 1000
#define INDENT "  "
#define INDENT_WIDTH 2
#define COMMENT_TOKEN "//"

// ---------------------------------------------------------------- UTF-8 and character classes

static bool is_continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

static size_t next_char(const struct editor *editor, size_t offset) {
    if (offset >= editor->length) return editor->length;
    offset++;
    while (offset < editor->length && is_continuation((unsigned char)editor->text[offset])) offset++;
    return offset;
}

static size_t previous_char(const struct editor *editor, size_t offset) {
    if (offset == 0) return 0;
    offset--;
    while (offset > 0 && is_continuation((unsigned char)editor->text[offset])) offset--;
    return offset;
}

enum char_class { CLASS_SPACE, CLASS_WORD, CLASS_PUNCTUATION };

static enum char_class class_of(unsigned char byte) {
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') return CLASS_SPACE;
    if (isalnum(byte) || byte == '_' || byte == '$' || byte >= 0x80) return CLASS_WORD;
    return CLASS_PUNCTUATION;
}

static bool is_blank(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') return false;
    return true;
}

static size_t leading_whitespace(const char *text, size_t length) {
    size_t count = 0;
    while (count < length && (text[count] == ' ' || text[count] == '\t')) count++;
    return count;
}

// ---------------------------------------------------------------- buffer and line index

static void rebuild_lines(struct editor *editor) {
    editor->line_count = 0;
    for (size_t offset = 0;; offset++) {
        if (offset == 0 || editor->text[offset - 1] == '\n') {
            if (editor->line_count == editor->line_capacity) {
                editor->line_capacity = editor->line_capacity ? editor->line_capacity * 2 : 64;
                editor->line_starts = realloc(editor->line_starts, editor->line_capacity * sizeof(size_t));
            }
            editor->line_starts[editor->line_count++] = offset;
        }
        if (offset >= editor->length) break;
    }
}

// Replaces the bytes [start, end) with `text`. Callers record undo and set the cursor.
static void replace(struct editor *editor, size_t start, size_t end, const char *text, size_t length) {
    size_t new_length = editor->length - (end - start) + length;
    if (new_length + 1 > editor->capacity) {
        while (new_length + 1 > editor->capacity) editor->capacity = editor->capacity ? editor->capacity * 2 : 4096;
        editor->text = realloc(editor->text, editor->capacity);
    }
    memmove(editor->text + start + length, editor->text + end, editor->length - end);
    memcpy(editor->text + start, text, length);
    editor->length = new_length;
    editor->text[new_length] = '\0';
    editor->modified = true;
    editor->revision++;
    rebuild_lines(editor);
}

void editor_init(struct editor *editor) {
    memset(editor, 0, sizeof *editor);
    replace(editor, 0, 0, "", 0);
    editor->modified = false;
}

static void free_snapshots(struct snapshot_stack *stack) {
    for (size_t i = 0; i < stack->count; i++) free(stack->items[i].text);
    stack->count = 0;
}

void editor_free(struct editor *editor) {
    free_snapshots(&editor->undo);
    free_snapshots(&editor->redo);
    free(editor->undo.items);
    free(editor->redo.items);
    free(editor->text);
    free(editor->line_starts);
    memset(editor, 0, sizeof *editor);
}

size_t editor_line_of(const struct editor *editor, size_t offset) {
    size_t low = 0, high = editor->line_count - 1;
    while (low < high) {
        size_t middle = (low + high + 1) / 2;
        if (editor->line_starts[middle] <= offset) low = middle;
        else high = middle - 1;
    }
    return low;
}

size_t editor_line_start(const struct editor *editor, size_t line) {
    return editor->line_starts[line < editor->line_count ? line : editor->line_count - 1];
}

size_t editor_line_end(const struct editor *editor, size_t line) {
    return line + 1 < editor->line_count ? editor->line_starts[line + 1] - 1 : editor->length;
}

size_t editor_column_of(const struct editor *editor, size_t offset) {
    size_t column = 0;
    for (size_t i = editor_line_start(editor, editor_line_of(editor, offset)); i < offset; i++)
        if (!is_continuation((unsigned char)editor->text[i])) column++;
    return column;
}

size_t editor_offset_at(const struct editor *editor, size_t line, size_t column) {
    size_t offset = editor_line_start(editor, line), end = editor_line_end(editor, line);
    while (column > 0 && offset < end) {
        offset = next_char(editor, offset);
        column--;
    }
    return offset;
}

// ---------------------------------------------------------------- undo

static void push_snapshot(struct snapshot_stack *stack, const struct editor *editor) {
    if (stack->count == MAX_UNDO_STEPS) {
        free(stack->items[0].text);
        memmove(stack->items, stack->items + 1, (stack->count - 1) * sizeof *stack->items);
        stack->count--;
    }
    if (stack->count == stack->capacity) {
        stack->capacity = stack->capacity ? stack->capacity * 2 : 64;
        stack->items = realloc(stack->items, stack->capacity * sizeof *stack->items);
    }
    struct snapshot *snapshot = &stack->items[stack->count++];
    snapshot->text = malloc(editor->length + 1);
    memcpy(snapshot->text, editor->text, editor->length + 1);
    snapshot->length = editor->length;
    snapshot->cursor = editor->cursor;
    snapshot->anchor = editor->anchor;
}

static void begin_edit(struct editor *editor, enum edit_kind kind) {
    bool merge = kind != EDIT_OTHER && kind == editor->last_edit && !editor_has_selection(editor);
    if (!merge) {
        push_snapshot(&editor->undo, editor);
        free_snapshots(&editor->redo);
    }
    editor->last_edit = kind;
    editor->has_goal_column = false;
}

static bool restore(struct editor *editor, struct snapshot_stack *from, struct snapshot_stack *to) {
    if (from->count == 0) return false;
    push_snapshot(to, editor);
    struct snapshot snapshot = from->items[--from->count];
    replace(editor, 0, editor->length, snapshot.text, snapshot.length);
    editor->cursor = snapshot.cursor;
    editor->anchor = snapshot.anchor;
    editor->last_edit = EDIT_OTHER;
    editor->has_goal_column = false;
    free(snapshot.text);
    return true;
}

bool editor_undo(struct editor *editor) { return restore(editor, &editor->undo, &editor->redo); }
bool editor_redo(struct editor *editor) { return restore(editor, &editor->redo, &editor->undo); }

void editor_set_text(struct editor *editor, const char *text, size_t length) {
    replace(editor, 0, editor->length, text, length);
    free_snapshots(&editor->undo);
    free_snapshots(&editor->redo);
    size_t cursor = editor->cursor < editor->length ? editor->cursor : editor->length;
    while (cursor > 0 && is_continuation((unsigned char)editor->text[cursor])) cursor--;
    editor->cursor = editor->anchor = cursor;
    editor->last_edit = EDIT_OTHER;
    editor->modified = false;
}

// ---------------------------------------------------------------- selection and motion

bool editor_has_selection(const struct editor *editor) { return editor->cursor != editor->anchor; }

void editor_selection(const struct editor *editor, size_t *start, size_t *end) {
    *start = editor->cursor < editor->anchor ? editor->cursor : editor->anchor;
    *end = editor->cursor < editor->anchor ? editor->anchor : editor->cursor;
}

void editor_set_cursor(struct editor *editor, size_t offset, bool extend) {
    editor->cursor = offset > editor->length ? editor->length : offset;
    if (!extend) editor->anchor = editor->cursor;
    editor->last_edit = EDIT_OTHER;
    editor->has_goal_column = false;
}

void editor_move_vertical(struct editor *editor, long lines, bool extend) {
    size_t column = editor->has_goal_column ? editor->goal_column : editor_column_of(editor, editor->cursor);
    long target = (long)editor_line_of(editor, editor->cursor) + lines;
    size_t offset;
    if (target < 0) offset = 0;
    else if (target >= (long)editor->line_count) offset = editor->length;
    else offset = editor_offset_at(editor, (size_t)target, column);
    editor_set_cursor(editor, offset, extend);
    editor->goal_column = column;
    editor->has_goal_column = true;
}

static size_t word_left(const struct editor *editor, size_t offset) {
    while (offset > 0 && class_of((unsigned char)editor->text[offset - 1]) == CLASS_SPACE) offset--;
    if (offset == 0) return 0;
    enum char_class class = class_of((unsigned char)editor->text[offset - 1]);
    while (offset > 0 && class_of((unsigned char)editor->text[offset - 1]) == class) offset--;
    return offset;
}

static size_t word_right(const struct editor *editor, size_t offset) {
    while (offset < editor->length && class_of((unsigned char)editor->text[offset]) == CLASS_SPACE) offset++;
    if (offset == editor->length) return offset;
    enum char_class class = class_of((unsigned char)editor->text[offset]);
    while (offset < editor->length && class_of((unsigned char)editor->text[offset]) == class) offset++;
    return offset;
}

void editor_move(struct editor *editor, enum motion motion, bool extend) {
    size_t start, end, offset = editor->cursor;
    editor_selection(editor, &start, &end);
    size_t line = editor_line_of(editor, offset);
    switch (motion) {
    case MOTION_LEFT: offset = editor_has_selection(editor) && !extend ? start : previous_char(editor, offset); break;
    case MOTION_RIGHT: offset = editor_has_selection(editor) && !extend ? end : next_char(editor, offset); break;
    case MOTION_UP: editor_move_vertical(editor, -1, extend); return;
    case MOTION_DOWN: editor_move_vertical(editor, 1, extend); return;
    case MOTION_LINE_START: {
        size_t line_start = editor_line_start(editor, line);
        size_t first = line_start + leading_whitespace(editor->text + line_start, editor_line_end(editor, line) - line_start);
        offset = offset == first ? line_start : first;
        break;
    }
    case MOTION_LINE_END: offset = editor_line_end(editor, line); break;
    case MOTION_WORD_LEFT: offset = word_left(editor, offset); break;
    case MOTION_WORD_RIGHT: offset = word_right(editor, offset); break;
    case MOTION_DOCUMENT_START: offset = 0; break;
    case MOTION_DOCUMENT_END: offset = editor->length; break;
    }
    editor_set_cursor(editor, offset, extend);
}

void editor_select_all(struct editor *editor) {
    editor_set_cursor(editor, 0, false);
    editor_set_cursor(editor, editor->length, true);
}

void editor_select_word_at(struct editor *editor, size_t offset) {
    if (offset > editor->length) offset = editor->length;
    size_t start = offset, end = offset;
    unsigned char byte = offset < editor->length ? (unsigned char)editor->text[offset] : ' ';
    if (class_of(byte) != CLASS_WORD && offset > 0) byte = (unsigned char)editor->text[offset - 1];
    enum char_class class = class_of(byte);
    if (class == CLASS_SPACE) return editor_set_cursor(editor, offset, false);
    while (start > 0 && class_of((unsigned char)editor->text[start - 1]) == class) start--;
    while (end < editor->length && class_of((unsigned char)editor->text[end]) == class) end++;
    editor_set_cursor(editor, start, false);
    editor_set_cursor(editor, end, true);
}

// ---------------------------------------------------------------- insert and delete

static void replace_selection(struct editor *editor, const char *text, size_t length) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    replace(editor, start, end, text, length);
    editor->cursor = editor->anchor = start + length;
}

void editor_insert(struct editor *editor, const char *text, size_t length, enum edit_kind kind) {
    begin_edit(editor, kind);
    replace_selection(editor, text, length);
}

void editor_insert_newline(struct editor *editor) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t line_start = editor_line_start(editor, editor_line_of(editor, start));
    size_t indent = leading_whitespace(editor->text + line_start, start - line_start);
    char *text = malloc(indent + 1);
    text[0] = '\n';
    memcpy(text + 1, editor->text + line_start, indent);
    editor_insert(editor, text, indent + 1, EDIT_OTHER);
    free(text);
}

void editor_delete_selection(struct editor *editor) {
    if (!editor_has_selection(editor)) return;
    begin_edit(editor, EDIT_OTHER);
    replace_selection(editor, "", 0);
}

void editor_backspace(struct editor *editor) {
    if (editor_has_selection(editor)) return editor_delete_selection(editor);
    if (editor->cursor == 0) return;
    begin_edit(editor, EDIT_DELETING);
    size_t start = previous_char(editor, editor->cursor);
    replace(editor, start, editor->cursor, "", 0);
    editor->cursor = editor->anchor = start;
}

void editor_delete_forward(struct editor *editor) {
    if (editor_has_selection(editor)) return editor_delete_selection(editor);
    if (editor->cursor == editor->length) return;
    begin_edit(editor, EDIT_DELETING);
    replace(editor, editor->cursor, next_char(editor, editor->cursor), "", 0);
}

// ---------------------------------------------------------------- line operations

static void selected_lines(const struct editor *editor, size_t *first, size_t *last) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    *first = editor_line_of(editor, start);
    *last = editor_line_of(editor, end);
    if (*last > *first && end == editor_line_start(editor, *last)) (*last)--;
}

// One change inside a line: `removed` bytes at byte column `at` become `insert`.
// With `trim_end`, the spaces and tabs at the end of the line go away too.
struct line_change {
    size_t at;
    size_t removed;
    const char *insert;
    size_t inserted;
    bool trim_end;
};

static size_t map_column(size_t column, const struct line_change *change) {
    if (column < change->at) return column;
    if (column < change->at + change->removed) {
        size_t into = column - change->at;
        return change->at + (into < change->inserted ? into : change->inserted);
    }
    return column - change->removed + change->inserted;
}

static size_t map_offset(const struct editor *editor, size_t offset, size_t first, size_t last,
                         const struct line_change *changes, const size_t *new_starts, const size_t *new_lengths,
                         long total_delta) {
    size_t line = editor_line_of(editor, offset);
    if (line < first) return offset;
    if (line > last) return (size_t)((long)offset + total_delta);
    size_t column = map_column(offset - editor_line_start(editor, line), &changes[line - first]);
    return new_starts[line - first] + (column < new_lengths[line - first] ? column : new_lengths[line - first]);
}

// Applies one change per line for the lines first..last and keeps the cursor and the anchor on their text.
static void change_lines(struct editor *editor, size_t first, size_t last, const struct line_change *changes) {
    size_t count = last - first + 1;
    size_t region_start = editor_line_start(editor, first), region_end = editor_line_end(editor, last);
    size_t capacity = region_end - region_start + 1;
    for (size_t i = 0; i < count; i++) capacity += changes[i].inserted;
    char *text = malloc(capacity);
    size_t *new_starts = malloc(count * sizeof *new_starts);
    size_t *new_lengths = malloc(count * sizeof *new_lengths);
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        size_t start = editor_line_start(editor, first + i), end = editor_line_end(editor, first + i);
        const struct line_change *change = &changes[i];
        if (change->trim_end)
            while (end > start + change->at + change->removed && (editor->text[end - 1] == ' ' || editor->text[end - 1] == '\t')) end--;
        new_starts[i] = region_start + used;
        memcpy(text + used, editor->text + start, change->at);
        used += change->at;
        if (change->inserted) memcpy(text + used, change->insert, change->inserted);
        used += change->inserted;
        size_t rest = change->at + change->removed;
        memcpy(text + used, editor->text + start + rest, end - start - rest);
        used += end - start - rest;
        new_lengths[i] = region_start + used - new_starts[i];
        if (i + 1 < count) text[used++] = '\n';
    }
    if (used == region_end - region_start && memcmp(text, editor->text + region_start, used) == 0) {
        free(text);
        free(new_starts);
        free(new_lengths);
        return;
    }
    long total_delta = (long)used - (long)(region_end - region_start);
    size_t cursor = map_offset(editor, editor->cursor, first, last, changes, new_starts, new_lengths, total_delta);
    size_t anchor = map_offset(editor, editor->anchor, first, last, changes, new_starts, new_lengths, total_delta);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, region_start, region_end, text, used);
    editor->cursor = cursor;
    editor->anchor = anchor;
    free(text);
    free(new_starts);
    free(new_lengths);
}

static const char *line_text(const struct editor *editor, size_t line, size_t *length) {
    size_t start = editor_line_start(editor, line);
    *length = editor_line_end(editor, line) - start;
    return editor->text + start;
}

void editor_toggle_comment(struct editor *editor) {
    size_t first, last;
    selected_lines(editor, &first, &last);
    size_t count = last - first + 1, minimum_indent = (size_t)-1;
    bool all_commented = true, any_code = false;
    for (size_t line = first; line <= last; line++) {
        size_t length;
        const char *text = line_text(editor, line, &length);
        if (is_blank(text, length)) continue;
        any_code = true;
        size_t indent = leading_whitespace(text, length);
        if (indent < minimum_indent) minimum_indent = indent;
        if (length - indent < 2 || strncmp(text + indent, COMMENT_TOKEN, 2) != 0) all_commented = false;
    }
    if (!any_code) return;
    struct line_change *changes = calloc(count, sizeof *changes);
    for (size_t i = 0; i < count; i++) {
        size_t length;
        const char *text = line_text(editor, first + i, &length);
        if (is_blank(text, length)) continue;
        if (all_commented) {
            size_t at = leading_whitespace(text, length);
            size_t removed = length > at + 2 && text[at + 2] == ' ' ? 3 : 2;
            changes[i] = (struct line_change){ .at = at, .removed = removed };
        } else {
            changes[i] = (struct line_change){ .at = minimum_indent, .insert = COMMENT_TOKEN " ", .inserted = 3 };
        }
    }
    change_lines(editor, first, last, changes);
    free(changes);
}

void editor_indent(struct editor *editor, bool outdent) {
    if (!outdent && !editor_has_selection(editor)) return editor_insert(editor, INDENT, INDENT_WIDTH, EDIT_TYPING);
    size_t first, last;
    selected_lines(editor, &first, &last);
    struct line_change *changes = calloc(last - first + 1, sizeof *changes);
    for (size_t line = first; line <= last; line++) {
        size_t length;
        const char *text = line_text(editor, line, &length);
        if (outdent) {
            size_t indent = leading_whitespace(text, length);
            changes[line - first].removed = indent < INDENT_WIDTH ? indent : INDENT_WIDTH;
        } else if (!is_blank(text, length)) {
            changes[line - first] = (struct line_change){ .insert = INDENT, .inserted = INDENT_WIDTH };
        }
    }
    change_lines(editor, first, last, changes);
    free(changes);
}

// The cursor and the anchor as line and byte column, so that line operations can move them by whole lines.
struct line_position {
    size_t cursor_line, cursor_column, anchor_line, anchor_column;
};

static struct line_position line_position(const struct editor *editor) {
    struct line_position position;
    position.cursor_line = editor_line_of(editor, editor->cursor);
    position.anchor_line = editor_line_of(editor, editor->anchor);
    position.cursor_column = editor->cursor - editor_line_start(editor, position.cursor_line);
    position.anchor_column = editor->anchor - editor_line_start(editor, position.anchor_line);
    return position;
}

static void restore_line_position(struct editor *editor, struct line_position position, long lines) {
    editor->cursor = editor_line_start(editor, (size_t)((long)position.cursor_line + lines)) + position.cursor_column;
    editor->anchor = editor_line_start(editor, (size_t)((long)position.anchor_line + lines)) + position.anchor_column;
}

void editor_move_lines(struct editor *editor, int direction) {
    size_t first, last;
    selected_lines(editor, &first, &last);
    if ((direction < 0 && first == 0) || (direction > 0 && last + 1 >= editor->line_count)) return;
    size_t other = direction < 0 ? first - 1 : last + 1;
    size_t block_start = editor_line_start(editor, first), block_end = editor_line_end(editor, last);
    size_t other_start = editor_line_start(editor, other), other_end = editor_line_end(editor, other);
    size_t block_length = block_end - block_start, other_length = other_end - other_start;
    char *text = malloc(block_length + other_length + 1);
    size_t region_start;
    if (direction < 0) {
        memcpy(text, editor->text + block_start, block_length);
        text[block_length] = '\n';
        memcpy(text + block_length + 1, editor->text + other_start, other_length);
        region_start = other_start;
    } else {
        memcpy(text, editor->text + other_start, other_length);
        text[other_length] = '\n';
        memcpy(text + other_length + 1, editor->text + block_start, block_length);
        region_start = block_start;
    }
    struct line_position position = line_position(editor);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, region_start, region_start + block_length + other_length + 1, text, block_length + other_length + 1);
    restore_line_position(editor, position, direction);
    free(text);
}

void editor_duplicate_lines(struct editor *editor) {
    size_t first, last;
    selected_lines(editor, &first, &last);
    size_t block_start = editor_line_start(editor, first), block_end = editor_line_end(editor, last);
    size_t block_length = block_end - block_start;
    char *text = malloc(block_length + 1);
    text[0] = '\n';
    memcpy(text + 1, editor->text + block_start, block_length);
    struct line_position position = line_position(editor);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, block_end, block_end, text, block_length + 1);
    restore_line_position(editor, position, (long)(last - first + 1));
    free(text);
}

// ---------------------------------------------------------------- whole text

void editor_replace_text(struct editor *editor, const char *text, size_t length) {
    if (length == editor->length && memcmp(text, editor->text, length) == 0) return;
    struct line_position position = line_position(editor);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, 0, editor->length, text, length);
    size_t cursor_line = position.cursor_line < editor->line_count ? position.cursor_line : editor->line_count - 1;
    size_t column = editor_line_end(editor, cursor_line) - editor_line_start(editor, cursor_line);
    size_t offset = editor_line_start(editor, cursor_line) + (position.cursor_column < column ? position.cursor_column : column);
    while (offset > 0 && is_continuation((unsigned char)editor->text[offset])) offset--;
    editor->cursor = editor->anchor = offset;
}

// ---------------------------------------------------------------- evaluation ranges

void editor_line_range(const struct editor *editor, size_t *start, size_t *end) {
    if (editor_has_selection(editor)) return editor_selection(editor, start, end);
    size_t line = editor_line_of(editor, editor->cursor);
    *start = editor_line_start(editor, line);
    *end = editor_line_end(editor, line);
}

static bool line_is_blank(const struct editor *editor, size_t line) {
    size_t length;
    const char *text = line_text(editor, line, &length);
    return is_blank(text, length);
}

void editor_block_range(const struct editor *editor, size_t *start, size_t *end) {
    size_t first = editor_line_of(editor, editor->cursor), last = first;
    if (line_is_blank(editor, first)) {
        *start = *end = editor->cursor;
        return;
    }
    while (first > 0 && !line_is_blank(editor, first - 1)) first--;
    while (last + 1 < editor->line_count && !line_is_blank(editor, last + 1)) last++;
    *start = editor_line_start(editor, first);
    *end = editor_line_end(editor, last);
}

// ---------------------------------------------------------------- brackets and search

long editor_matching_bracket(const struct editor *editor, size_t offset) {
    static const char opening[] = "([{", closing[] = ")]}";
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) {
            if (offset == 0) return -1;
            offset--;
        }
        if (offset >= editor->length) continue;
        char c = editor->text[offset];
        const char *open = strchr(opening, c), *close = strchr(closing, c);
        if (!c || (!open && !close)) continue;
        char self = c, other = open ? closing[open - opening] : opening[close - closing];
        int step = open ? 1 : -1, depth = 0;
        for (long i = (long)offset; i >= 0 && i < (long)editor->length; i += step) {
            if (editor->text[i] == self) depth++;
            else if (editor->text[i] == other && --depth == 0) return i;
        }
        return -1;
    }
    return -1;
}

bool editor_find_next(struct editor *editor, const char *needle, size_t length) {
    if (length == 0 || length > editor->length) return false;
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t positions = editor->length - length + 1;
    for (size_t step = 0; step < positions; step++) {
        size_t i = (end + step) % positions;
        if (memcmp(editor->text + i, needle, length) == 0) {
            editor_set_cursor(editor, i, false);
            editor_set_cursor(editor, i + length, true);
            return true;
        }
    }
    return false;
}

void editor_select_next_occurrence(struct editor *editor) {
    if (!editor_has_selection(editor)) return editor_select_word_at(editor, editor->cursor);
    size_t start, end;
    editor_selection(editor, &start, &end);
    char *needle = malloc(end - start);
    memcpy(needle, editor->text + start, end - start);
    editor_find_next(editor, needle, end - start);
    free(needle);
}
