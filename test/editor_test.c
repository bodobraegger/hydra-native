// Checks of the editing core (src/editor.c). Run with `make test`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/editor.h"

static int failures;

#define EXPECT(condition)                                                                       \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #condition);           \
            failures++;                                                                         \
        }                                                                                       \
    } while (0)

static void expect_text(const struct editor *editor, const char *expected, int line) {
    if (strcmp(editor->text, expected) != 0) {
        fprintf(stderr, "%s:%d: text is\n---\n%s\n---\nexpected\n---\n%s\n---\n", __FILE__, line, editor->text,
                expected);
        failures++;
    }
}
#define EXPECT_TEXT(editor, expected) expect_text(editor, expected, __LINE__)

static void load(struct editor *editor, const char *text) {
    editor_set_text(editor, text, strlen(text));
    editor_set_cursor(editor, 0, false);
}

static void type(struct editor *editor, const char *text) {
    for (const char *c = text; *c; c++) {
        if (*c == '\n') editor_insert_newline(editor);
        else editor_insert(editor, c, 1, EDIT_TYPING);
    }
}

static void test_typing_and_undo(void) {
    struct editor editor;
    editor_init(&editor);
    type(&editor, "osc(10)");
    EXPECT_TEXT(&editor, "osc(10)");
    EXPECT(editor.modified);
    editor_move(&editor, MOTION_LEFT, false);
    type(&editor, ", 0.1");
    EXPECT_TEXT(&editor, "osc(10, 0.1)");
    editor_undo(&editor); // the second typing run is one step
    EXPECT_TEXT(&editor, "osc(10)");
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "");
    EXPECT(!editor_undo(&editor));
    editor_redo(&editor);
    editor_redo(&editor);
    EXPECT_TEXT(&editor, "osc(10, 0.1)");
    editor_backspace(&editor);
    editor_backspace(&editor);
    EXPECT_TEXT(&editor, "osc(10, 0)"); // the cursor was before ")"
    editor_undo(&editor); // backspaces in sequence are one step
    EXPECT_TEXT(&editor, "osc(10, 0.1)");

    type(&editor, "\n");
    load(&editor, "  shape(4)");
    editor_move(&editor, MOTION_LINE_END, false);
    type(&editor, "\n.out()");
    EXPECT_TEXT(&editor, "  shape(4)\n  .out()"); // a new line keeps the indentation
    editor_free(&editor);
}

static void test_ranges(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "a = 1\n\nosc()\n  .rotate()\n  .out()\n\nnoise().out()");
    size_t start, end;
    editor_set_cursor(&editor, editor_offset_at(&editor, 3, 4), false);
    editor_block_range(&editor, &start, &end);
    EXPECT(start == 7 && end == strlen("a = 1\n\nosc()\n  .rotate()\n  .out()"));
    editor_line_range(&editor, &start, &end);
    EXPECT(strncmp(editor.text + start, "  .rotate()", end - start) == 0 && end - start == 11);
    editor_set_cursor(&editor, editor_offset_at(&editor, 1, 0), false);
    editor_block_range(&editor, &start, &end);
    EXPECT(start == end); // a blank line is no block
    editor_free(&editor);
}

// Comment toggling and moving lines, with undo.
static void test_line_operations(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "osc()\n  .out()\n\nnoise()");
    editor_set_cursor(&editor, 2, false);
    editor_set_cursor(&editor, editor_offset_at(&editor, 1, 4), true);
    editor_toggle_comment(&editor);
    EXPECT_TEXT(&editor, "// osc()\n//   .out()\n\nnoise()");
    EXPECT(editor.anchor == 5 && editor.cursor == editor_offset_at(&editor, 1, 7)); // the selection keeps its text
    editor_toggle_comment(&editor);
    EXPECT_TEXT(&editor, "osc()\n  .out()\n\nnoise()");
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "// osc()\n//   .out()\n\nnoise()");
    editor_redo(&editor);

    editor_set_cursor(&editor, editor_offset_at(&editor, 3, 2), false);
    editor_move_lines(&editor, -1);
    EXPECT_TEXT(&editor, "osc()\n  .out()\nnoise()\n");
    EXPECT(editor_line_of(&editor, editor.cursor) == 2 && editor_column_of(&editor, editor.cursor) == 2);
    editor_move_lines(&editor, 1);
    EXPECT_TEXT(&editor, "osc()\n  .out()\n\nnoise()");
    editor_free(&editor);
}

// editor_format: tabs by bracket depth, one more level for a chain, no trailing spaces, one undo step.
static void test_format(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "osc(10,\n0.1)\n.rotate()   \n    .out()\nshape(4)");
    editor_format(&editor);
    EXPECT_TEXT(&editor, "osc(10,\n\t0.1)\n\t.rotate()\n\t.out()\nshape(4)");
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "osc(10,\n0.1)\n.rotate()   \n    .out()\nshape(4)");
    editor_free(&editor);
}

static void test_find(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "a b a b a");
    editor_collapse(&editor);
    EXPECT(editor_find(&editor, "a", 1, false, 1) && editor.anchor == 0); // a match at the cursor counts
    EXPECT(editor_find(&editor, "a", 1, false, 1) && editor.anchor == 4);
    EXPECT(editor_find(&editor, "a", 1, false, -1) && editor.anchor == 0);

    // The find bar of the browser: no case.
    load(&editor, "Osc OSC osc");
    struct editor_range *matches;
    EXPECT(editor_find_all(&editor, "osc", 3, true, &matches) == 3);
    free(matches);
    EXPECT(editor_find_all(&editor, "osc", 3, false, &matches) == 1);
    free(matches);
    editor_free(&editor);
}

static void test_multiple_cursors(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "osc()\nnoise()\nshape()");
    editor_set_cursor(&editor, 4, false);
    editor_add_cursor_vertical(&editor, 1);
    editor_add_cursor_vertical(&editor, 1);
    EXPECT(editor_selection_count(&editor) == 3);
    editor_type(&editor, "1", 1);
    EXPECT_TEXT(&editor, "osc(1)\nnois1e()\nshap1e()"); // the same column on every line
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "osc()\nnoise()\nshape()");
    EXPECT(editor_selection_count(&editor) == 3); // undo brings the cursors back
    editor_move(&editor, MOTION_LINE_END, false);
    editor_type(&editor, ".out()", 6);
    EXPECT_TEXT(&editor, "osc().out()\nnoise().out()\nshape().out()");
    editor_backspace(&editor);
    editor_backspace(&editor); // deletes the "(" and its ")" together
    EXPECT_TEXT(&editor, "osc().out\nnoise().out\nshape().out");
    editor_toggle_comment(&editor);
    EXPECT_TEXT(&editor, "// osc().out\n// noise().out\n// shape().out");
    editor_collapse(&editor);
    EXPECT(editor_selection_count(&editor) == 1);
    editor_free(&editor);
}

// Enter: (a) the indentation of the line stays, (b) inside an open ( or [ with entries the new line starts under
// the first entry, (c) directly after an opener one level more, with the closer on its own line. Tab and
// Shift+Tab indent the selected lines.
static void test_indentation(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "  osc(10)");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "  osc(10)\n  "); // (a)

    load(&editor, "shape([4, 3,");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_insert_newline(&editor);
    editor_type(&editor, "5,", 2);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "shape([4, 3,\n       5,\n       "); // (b): under 4, then the entries line up

    load(&editor, "  shape([])");
    editor_set_cursor(&editor, strlen("  shape(["), false);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "  shape([\n    \n  ])"); // (c)
    EXPECT(editor.cursor == strlen("  shape([\n    "));

    load(&editor, "x\n  y");
    editor_select_all(&editor);
    editor_indent_lines(&editor, false);
    EXPECT_TEXT(&editor, "  x\n    y");
    editor_indent_lines(&editor, true);
    editor_indent_lines(&editor, true);
    EXPECT_TEXT(&editor, "x\ny");
    editor_free(&editor);
}

static void test_wrap(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "  osc(10).rotate(0.5).kaleid(4).out()");
    editor.wrap_columns = 16;
    size_t starts[8];
    size_t rows = editor_wrap_rows(&editor, 0, starts, 8);
    EXPECT(rows == 3);
    EXPECT(starts[1] == 16 && starts[2] == 28); // hard breaks; wrapped rows have 2 + 2 columns of indent
    EXPECT(editor_row_indent(&editor, 0) == 4);
    size_t row, x;
    editor_caret_position(&editor, 18, &row, &x);
    EXPECT(row == 1 && x == 6);
    EXPECT(editor_offset_at_visual(&editor, 0, 1, 6) == 18);
    editor_set_cursor(&editor, 3, false);
    editor_move(&editor, MOTION_DOWN, false); // down moves by visual rows
    EXPECT(editor_line_of(&editor, editor.cursor) == 0 && editor.cursor == 16);
    load(&editor, "aaa bbb ccc");
    editor.wrap_columns = 8;
    rows = editor_wrap_rows(&editor, 0, starts, 8);
    EXPECT(rows == 2 && starts[1] == 8); // breaks after the last space
    load(&editor, "a\t\tb"); // a tab is wider than a row of 2 columns: each row still takes one character
    editor.wrap_columns = 2;
    EXPECT(editor_wrap_rows(&editor, 0, starts, 8) == 4);
    editor_free(&editor);
}

static void test_group_motion(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "osc(10).out()\nnoise $x");
    editor_move(&editor, MOTION_GROUP_RIGHT, false);
    EXPECT(editor.cursor == 3); // "osc"
    editor_move(&editor, MOTION_GROUP_RIGHT, false);
    EXPECT(editor.cursor == 4); // "("
    editor_move(&editor, MOTION_DOCUMENT_START, false);
    editor_set_cursor(&editor, 13, false); // the line end
    editor_move(&editor, MOTION_GROUP_RIGHT, false);
    EXPECT(editor.cursor == 14); // one line end is a group of its own
    editor_move(&editor, MOTION_GROUP_RIGHT, false);
    EXPECT(editor.cursor == 19); // "noise"
    editor_move(&editor, MOTION_GROUP_RIGHT, false);
    EXPECT(editor.cursor == 21); // " $": "$" is punctuation in CodeMirror
    editor_move(&editor, MOTION_GROUP_LEFT, false);
    EXPECT(editor.cursor == 20);
    editor_move(&editor, MOTION_GROUP_LEFT, false);
    EXPECT(editor.cursor == 14);
    editor_move(&editor, MOTION_GROUP_LEFT, false);
    EXPECT(editor.cursor == 13);
    editor_free(&editor);
}

// Random editing with a fixed seed: after each step the selections stay inside the text, the line table matches
// the text, and an undo followed by a redo gives the same text. The sanitizers of the unit tests check the rest.
#define RANDOM_STEPS 100000
#define RANDOM_RESTART 5000 // steps until a new empty text
#define RANDOM_MAX_LENGTH 1500

static void up(struct editor *e) { editor_move_lines(e, -1); }
static void down(struct editor *e) { editor_move_lines(e, 1); }
static void cursor_below(struct editor *e) { editor_add_cursor_vertical(e, 1); }
static void word_back(struct editor *e) { editor_delete_word(e, -1); }
static void line_end(struct editor *e) { editor_delete_line_part(e, 1); }
static void sort(struct editor *e) { editor_sort_lines(e, false, true); }
static void upper(struct editor *e) { editor_change_case(e, true); }
static void outdent(struct editor *e) { editor_indent(e, true); }
static void find_next(struct editor *e) { editor_find_under(e, 1); }
static void undo(struct editor *e) { editor_undo(e); }
static void redo(struct editor *e) { editor_redo(e); }

static void test_random_edits(void) {
    static void (*const commands[])(struct editor *) = {
        editor_insert_newline, editor_backspace, editor_delete_forward, editor_delete_selection, editor_transpose,
        editor_toggle_comment, editor_format, editor_duplicate_lines, editor_delete_lines, editor_join_lines,
        editor_select_lines, editor_split_selection_by_line, editor_go_to_bracket, editor_select_between_brackets,
        editor_select_next_occurrence, editor_select_all_under, editor_select_scope, editor_collapse,
        editor_collapse_to_first, editor_select_all, editor_set_mark, editor_select_to_mark, editor_delete_to_mark,
        editor_swap_with_mark, editor_yank, editor_skip_and_select_next, editor_toggle_bookmark,
        editor_select_bookmarks, up, down, cursor_below, word_back, line_end, sort, upper, outdent,
        find_next, undo, redo,
    };
    static const char *const pieces[] = { "osc(", ")", "\n", "\t", "  ", "'", "\"", "{", "}", "[", "a", "B_c", ".",
                                          "\xC3\xA9", "e\xCC\x81", "\xE6\x97\xA5", "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD",
                                          "\xC3\xA9\xC3\xB6", "\xFF", "\xC3", "\xE6\x97", "\r\n", "// ", "=> " };
    enum { COMMAND_COUNT = sizeof commands / sizeof commands[0], PIECE_COUNT = sizeof pieces / sizeof pieces[0] };
    _Static_assert(COMMAND_COUNT < 100, "the command share of the choices");
    struct editor editor;
    editor_init(&editor);
    srand(1);
    for (int step = 0; step < RANDOM_STEPS && !failures; step++) {
        if (step % RANDOM_RESTART == 0) editor_set_text(&editor, "", 0);
        unsigned choice = (unsigned)rand() % 200; // most steps add text, so that the text grows
        const char *piece = pieces[(unsigned)rand() % PIECE_COUNT];
        size_t line = (size_t)rand() % editor.line_count, column = (size_t)rand() % 30;
        if (choice < COMMAND_COUNT) commands[choice](&editor);
        else if (choice < 100) editor_type(&editor, piece, strlen(piece));
        else if (choice < 130) editor_insert(&editor, piece, strlen(piece), EDIT_OTHER);
        else if (choice < 160) editor_move(&editor, (enum motion)(column % 12), step % 3 == 0);
        else if (choice < 170) editor_move_vertical(&editor, (long)column - 15, step % 2);
        else if (choice < 180) editor_set_cursor(&editor, editor_offset_at(&editor, line, column), step % 2);
        else if (choice < 188) editor_add_cursor(&editor, editor_offset_at(&editor, line, column));
        else if (choice < 192) editor_column_select(&editor, line, column, line + 3, column / 2);
        else if (choice < 196) editor.wrap_columns = column < 4 ? 0 : column;
        else editor_find(&editor, piece, strlen(piece), true, step % 2 ? 1 : -1);
        if (editor.length > RANDOM_MAX_LENGTH) editor_replace_text(&editor, "osc()", 5);

        struct selection *list;
        size_t count = editor_selections(&editor, &list), newlines = 0, column_now, row_now;
        for (size_t i = 0; i < count; i++) EXPECT(list[i].cursor <= editor.length && list[i].anchor <= editor.length);
        free(list);
        EXPECT(editor.text[editor.length] == '\0' && editor.line_starts[0] == 0);
        for (size_t i = 0; i < editor.length; i++) newlines += editor.text[i] == '\n';
        EXPECT(editor.line_count == newlines + 1);
        for (size_t i = 1; i < editor.line_count; i++) EXPECT(editor.text[editor.line_starts[i] - 1] == '\n');
        editor_caret_position(&editor, editor.cursor, &row_now, &column_now);
        char *before = strdup(editor.text);
        if (editor_undo(&editor) && editor_redo(&editor)) EXPECT(!strcmp(before, editor.text));
        free(before);
        if (failures) fprintf(stderr, "random edits: step %d, choice %u, piece \"%s\"\n", step, choice, piece);
    }
    editor_free(&editor);
}

int main(void) {
    test_typing_and_undo();
    test_ranges();
    test_line_operations();
    test_format();
    test_find();
    test_multiple_cursors();
    test_indentation();
    test_wrap();
    test_group_motion();
    test_random_edits();
    if (failures) {
        fprintf(stderr, "editor_test: %d failures\n", failures);
        return 1;
    }
    printf("editor_test: ok\n");
    return 0;
}
