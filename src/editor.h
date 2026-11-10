// Text editing core of the code overlay: a UTF-8 buffer with multiple selections, snapshot undo, line commands,
// soft wrap and typing rules (closing brackets and quotes, indentation).
// It has no SDL or GL dependency, so test/editor_test.c can check it alone.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define EDITOR_HANGING_INDENT 2 // extra columns of wrapped rows, like softWrapHangingIndent in Pulsar
#define EDITOR_TAB_WIDTH 4 // the tabSize default of CodeMirror 5 in the web editor
#define EDITOR_HARD_WRAP_COLUMNS 80

enum motion {
    MOTION_LEFT,
    MOTION_RIGHT,
    MOTION_UP, // by visual rows when wrap is on
    MOTION_DOWN,
    MOTION_LINE_START, // first non-blank character, then column 0
    MOTION_LINE_END,
    MOTION_WORD_LEFT,
    MOTION_WORD_RIGHT,
    MOTION_SUBWORD_LEFT, // camelCase parts, Alt+Left in the sublime keymap
    MOTION_SUBWORD_RIGHT,
    MOTION_DOCUMENT_START,
    MOTION_DOCUMENT_END,
};

// Edits of the same kind in sequence merge into one undo step (typing a word, deleting with backspace).
enum edit_kind {
    EDIT_OTHER,
    EDIT_TYPING,
    EDIT_DELETING,
};

// A selection is the text between anchor and cursor; an empty selection is a caret.
struct selection {
    size_t cursor;
    size_t anchor;
    size_t goal_column; // the column (visual with wrap) that up and down motions keep
    bool has_goal_column;
    bool primary;
};

struct snapshot {
    char *text;
    size_t length;
    struct selection *selections; // all selections, the primary one first
    size_t selection_count;
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
    // The primary selection; the overlay scrolls to it and evaluation keys use it.
    size_t cursor;
    size_t anchor;
    size_t goal_column;
    bool has_goal_column;
    // Further selections of multiple cursors, sorted, without overlaps.
    struct selection *extra;
    size_t extra_count;
    size_t extra_capacity;
    size_t *line_starts;
    size_t line_count;
    size_t line_capacity;
    size_t wrap_columns; // 0: no soft wrap
    size_t *bookmarks; // sorted lines
    size_t bookmark_count;
    size_t bookmark_capacity;
    size_t mark; // the "sublime mark" of Ctrl+K Ctrl+Space
    bool has_mark;
    char *killed; // the text of the last "delete to mark", for "yank"
    struct snapshot_stack undo;
    struct snapshot_stack redo;
    enum edit_kind last_edit;
    bool modified; // since the last load or save
    unsigned revision; // changes after each edit
};

void editor_init(struct editor *editor);
void editor_free(struct editor *editor);
void editor_set_text(struct editor *editor, const char *text, size_t length); // clears undo and `modified`
// Replaces all text as one undo step; the cursor keeps its line and column where it can.
void editor_replace_text(struct editor *editor, const char *text, size_t length);

size_t editor_line_of(const struct editor *editor, size_t offset);
size_t editor_line_start(const struct editor *editor, size_t line);
size_t editor_line_end(const struct editor *editor, size_t line); // offset of the newline or of the end
size_t editor_column_of(const struct editor *editor, size_t offset); // in code points
size_t editor_offset_at(const struct editor *editor, size_t line, size_t column);

// Selections. The functions without "all" in the name work on the primary selection.
bool editor_has_selection(const struct editor *editor);
void editor_selection(const struct editor *editor, size_t *start, size_t *end);
size_t editor_selection_count(const struct editor *editor);
size_t editor_selections(const struct editor *editor, struct selection **list); // sorted copy, caller frees
char *editor_selected_text(const struct editor *editor, size_t *length); // all selections joined by newlines
void editor_set_cursor(struct editor *editor, size_t offset, bool extend);
void editor_collapse(struct editor *editor); // keeps only the primary selection
void editor_collapse_to_first(struct editor *editor); // keeps only the first selection (Escape in sublime)
void editor_add_cursor(struct editor *editor, size_t offset); // a new caret becomes primary
void editor_add_cursor_vertical(struct editor *editor, int direction); // above (-1) or below (+1) the outer one
void editor_column_select(struct editor *editor, size_t anchor_line, size_t anchor_column, size_t line, size_t column);
void editor_move(struct editor *editor, enum motion motion, bool extend); // all selections
void editor_move_vertical(struct editor *editor, long rows, bool extend); // all selections
void editor_select_all(struct editor *editor);
void editor_select_word_at(struct editor *editor, size_t offset);
void editor_select_lines(struct editor *editor); // Ctrl+L: whole lines, again to extend
void editor_split_selection_by_line(struct editor *editor);
void editor_go_to_bracket(struct editor *editor);
void editor_select_between_brackets(struct editor *editor);
void editor_go_to_line(struct editor *editor, size_t line); // line counts from 0

// Edits; they work at every selection.
void editor_insert(struct editor *editor, const char *text, size_t length, enum edit_kind kind);
void editor_type(struct editor *editor, const char *text, size_t length); // typing rules for brackets and quotes
void editor_insert_newline(struct editor *editor); // keeps the indentation, indents after an opening bracket
void editor_backspace(struct editor *editor);
void editor_delete_forward(struct editor *editor);
void editor_delete_word(struct editor *editor, int direction);
void editor_delete_selection(struct editor *editor);
void editor_transpose(struct editor *editor);
bool editor_undo(struct editor *editor);
bool editor_redo(struct editor *editor);

// Line commands on the lines that the selections touch.
void editor_toggle_comment(struct editor *editor);
void editor_indent(struct editor *editor, bool outdent); // Tab without selection inserts spaces
void editor_indent_lines(struct editor *editor, bool outdent); // Ctrl+] and Ctrl+[
void editor_move_lines(struct editor *editor, int direction); // -1 up, +1 down
void editor_duplicate_lines(struct editor *editor);
void editor_delete_lines(struct editor *editor);
void editor_join_lines(struct editor *editor);
void editor_sort_lines(struct editor *editor, bool reverse, bool insensitive);
void editor_hard_wrap(struct editor *editor, size_t width); // Alt+Q

// Commands of the CodeMirror 5 sublime keymap.
void editor_toggle_bookmark(struct editor *editor);
bool editor_is_bookmarked(const struct editor *editor, size_t line);
void editor_next_bookmark(struct editor *editor, int direction);
void editor_clear_bookmarks(struct editor *editor);
void editor_select_bookmarks(struct editor *editor);
void editor_find_under(struct editor *editor, int direction);
void editor_select_all_under(struct editor *editor);
void editor_select_scope(struct editor *editor);
void editor_change_case(struct editor *editor, bool upper);
void editor_delete_line_part(struct editor *editor, int direction); // to the line start (-1) or end (+1)
void editor_set_mark(struct editor *editor);
void editor_select_to_mark(struct editor *editor);
void editor_delete_to_mark(struct editor *editor);
void editor_swap_with_mark(struct editor *editor);
void editor_yank(struct editor *editor);
void editor_skip_and_select_next(struct editor *editor);

// Ranges for evaluation, as byte offsets [start, end).
void editor_line_range(const struct editor *editor, size_t *start, size_t *end); // selection, or the cursor line
void editor_block_range(const struct editor *editor, size_t *start, size_t *end); // lines between blank lines

long editor_matching_bracket(const struct editor *editor, size_t offset); // -1 when there is none
bool editor_find(struct editor *editor, const char *needle, size_t length, int direction); // selects the match
void editor_select_next_occurrence(struct editor *editor); // Ctrl+D: the word, then one more selection
size_t editor_replace_all(struct editor *editor, const char *needle, size_t needle_length, const char *replacement,
                          size_t replacement_length); // returns the number of replacements
bool editor_replace_next(struct editor *editor, const char *needle, size_t needle_length, const char *replacement,
                         size_t replacement_length); // replaces the selected match, then selects the next one

// Words for autocomplete: the identifier before the primary cursor, and the identifiers of the text.
size_t editor_word_before_cursor(const struct editor *editor, size_t *start);
size_t editor_words(const struct editor *editor, const char *prefix, size_t prefix_length, char ***words); // caller frees

// Soft wrap: visual rows of a line with editor->wrap_columns columns and the hanging indent.
size_t editor_wrap_rows(const struct editor *editor, size_t line, size_t *row_starts, size_t capacity);
size_t editor_row_indent(const struct editor *editor, size_t line); // columns of the indent of wrapped rows
void editor_visual_position(const struct editor *editor, size_t offset, size_t *row, size_t *x); // row in its line
size_t editor_offset_at_visual(const struct editor *editor, size_t line, size_t row, size_t x);
