// Text editing core of the code overlay: a UTF-8 buffer with one cursor and selection, and snapshot undo.
// It has no SDL or GL dependency, so test/editor_test.c can check it alone.
#pragma once

#include <stdbool.h>
#include <stddef.h>

enum motion {
    MOTION_LEFT,
    MOTION_RIGHT,
    MOTION_UP,
    MOTION_DOWN,
    MOTION_LINE_START, // first non-blank character, then column 0
    MOTION_LINE_END,
    MOTION_WORD_LEFT,
    MOTION_WORD_RIGHT,
    MOTION_DOCUMENT_START,
    MOTION_DOCUMENT_END,
};

// Edits of the same kind in sequence merge into one undo step (typing a word, deleting with backspace).
enum edit_kind {
    EDIT_OTHER,
    EDIT_TYPING,
    EDIT_DELETING,
};

struct snapshot {
    char *text;
    size_t length;
    size_t cursor;
    size_t anchor;
};

struct snapshot_stack {
    struct snapshot *items;
    size_t count;
    size_t capacity;
};

struct editor {
    char *text;
    size_t length;
    size_t capacity;
    size_t cursor;
    size_t anchor; // the selection is between anchor and cursor
    size_t goal_column; // column that up and down motions keep
    bool has_goal_column;
    size_t *line_starts;
    size_t line_count;
    size_t line_capacity;
    struct snapshot_stack undo;
    struct snapshot_stack redo;
    enum edit_kind last_edit;
    bool modified; // since the last load or save
    unsigned revision; // changes after each edit
};

void editor_init(struct editor *editor);
void editor_free(struct editor *editor);
void editor_set_text(struct editor *editor, const char *text, size_t length); // clears undo and `modified`

size_t editor_line_of(const struct editor *editor, size_t offset);
size_t editor_line_start(const struct editor *editor, size_t line);
size_t editor_line_end(const struct editor *editor, size_t line); // offset of the newline or of the end
size_t editor_column_of(const struct editor *editor, size_t offset); // in code points
size_t editor_offset_at(const struct editor *editor, size_t line, size_t column);

bool editor_has_selection(const struct editor *editor);
void editor_selection(const struct editor *editor, size_t *start, size_t *end);
void editor_set_cursor(struct editor *editor, size_t offset, bool extend);
void editor_move(struct editor *editor, enum motion motion, bool extend);
void editor_move_vertical(struct editor *editor, long lines, bool extend);
void editor_select_all(struct editor *editor);
void editor_select_word_at(struct editor *editor, size_t offset);

void editor_insert(struct editor *editor, const char *text, size_t length, enum edit_kind kind);
void editor_insert_newline(struct editor *editor); // keeps the indentation of the current line
void editor_backspace(struct editor *editor);
void editor_delete_forward(struct editor *editor);
void editor_delete_selection(struct editor *editor);
bool editor_undo(struct editor *editor);
bool editor_redo(struct editor *editor);

void editor_toggle_comment(struct editor *editor);
void editor_move_lines(struct editor *editor, int direction); // -1 up, +1 down
void editor_duplicate_lines(struct editor *editor);
void editor_format(struct editor *editor);
void editor_indent(struct editor *editor, bool outdent);

// Ranges for evaluation, as byte offsets [start, end).
void editor_line_range(const struct editor *editor, size_t *start, size_t *end); // selection, or the cursor line
void editor_block_range(const struct editor *editor, size_t *start, size_t *end); // lines between blank lines

long editor_matching_bracket(const struct editor *editor, size_t offset); // -1 when there is none
bool editor_find_next(struct editor *editor, const char *needle, size_t length); // selects the next match
void editor_select_next_occurrence(struct editor *editor); // Ctrl+D with one selection
