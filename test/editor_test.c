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
        fprintf(stderr, "%s:%d: text is\n---\n%s\n---\nexpected\n---\n%s\n---\n", __FILE__, line, editor->text, expected);
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

static void test_utf8_and_motion(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "ä€x\nab");
    editor_move(&editor, MOTION_RIGHT, false);
    EXPECT(editor.cursor == 2);
    editor_move(&editor, MOTION_RIGHT, false);
    EXPECT(editor.cursor == 5);
    EXPECT(editor_column_of(&editor, editor.cursor) == 2);
    editor_move(&editor, MOTION_DOWN, false);
    EXPECT(editor.cursor == 9); // line 1, column 2 is the end of "ab"
    editor_move(&editor, MOTION_UP, false);
    EXPECT(editor.cursor == 5); // the goal column comes back
    editor_backspace(&editor);
    EXPECT_TEXT(&editor, "äx\nab");

    load(&editor, "osc(10).out(o1)");
    editor_move(&editor, MOTION_WORD_RIGHT, false);
    EXPECT(editor.cursor == 3);
    editor_move(&editor, MOTION_WORD_RIGHT, true);
    EXPECT(editor.cursor == 4 && editor.anchor == 3);
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_move(&editor, MOTION_WORD_LEFT, false);
    EXPECT(editor.cursor == 14);

    load(&editor, "    noise()");
    editor_move(&editor, MOTION_LINE_START, false);
    EXPECT(editor.cursor == 4);
    editor_move(&editor, MOTION_LINE_START, false);
    EXPECT(editor.cursor == 0);
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

    editor_set_cursor(&editor, 1, false);
    editor_duplicate_lines(&editor);
    EXPECT_TEXT(&editor, "osc()\nosc()\n  .out()\n\nnoise()");
    EXPECT(editor.cursor == 7);

    load(&editor, "osc()\n  .out()");
    editor_set_cursor(&editor, editor_offset_at(&editor, 1, 4), false);
    editor_replace_text(&editor, "osc(10)\n.out()", 14);
    EXPECT(editor.cursor == editor_offset_at(&editor, 1, 4)); // the cursor keeps line and column
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "osc()\n  .out()");
    editor_free(&editor);
}

static void test_search_and_brackets(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "osc(o0).out(o0)");
    EXPECT(editor_matching_bracket(&editor, 3) == 6);
    EXPECT(editor_matching_bracket(&editor, 7) == 3); // the bracket before the cursor
    EXPECT(editor_matching_bracket(&editor, 1) == -1);
    editor_set_cursor(&editor, 5, false);
    editor_select_next_occurrence(&editor);
    EXPECT(editor.anchor == 4 && editor.cursor == 6 && editor_selection_count(&editor) == 1);
    editor_select_next_occurrence(&editor); // adds the second "o0" as the primary selection
    EXPECT(editor.anchor == 12 && editor.cursor == 14 && editor_selection_count(&editor) == 2);
    editor_select_next_occurrence(&editor); // every match is selected already
    EXPECT(editor_selection_count(&editor) == 2);
    editor_type(&editor, "o1", 2); // typing replaces both
    EXPECT_TEXT(&editor, "osc(o1).out(o1)");

    load(&editor, "a b a b a");
    editor_collapse(&editor);
    EXPECT(editor_find(&editor, "a", 1, 1) && editor.anchor == 0); // a match at the cursor counts
    EXPECT(editor_find(&editor, "a", 1, 1) && editor.anchor == 4);
    EXPECT(editor_find(&editor, "a", 1, -1) && editor.anchor == 0);
    EXPECT(editor_replace_next(&editor, "a", 1, "xy", 2)); // replaces the selected "a", selects the next
    EXPECT_TEXT(&editor, "xy b a b a");
    EXPECT(editor.anchor == 5);
    EXPECT(editor_replace_all(&editor, "a", 1, "c", 1) == 2);
    EXPECT_TEXT(&editor, "xy b c b c");
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "xy b a b a");

    load(&editor, "osc(shape(4), 0.5)");
    editor_set_cursor(&editor, 11, false);
    editor_select_between_brackets(&editor);
    EXPECT(editor.anchor == 10 && editor.cursor == 11);
    editor_select_between_brackets(&editor); // again: with the brackets
    EXPECT(editor.anchor == 9 && editor.cursor == 12);
    editor_set_cursor(&editor, 3, false);
    editor_go_to_bracket(&editor);
    EXPECT(editor.cursor == 17);
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

    load(&editor, "ab\ncd\nef");
    editor_column_select(&editor, 0, 0, 2, 1);
    size_t length;
    char *text = editor_selected_text(&editor, &length);
    EXPECT(strcmp(text, "a\nc\ne") == 0);
    free(text);
    editor_select_all(&editor);
    editor_split_selection_by_line(&editor);
    EXPECT(editor_selection_count(&editor) == 3);
    editor_collapse(&editor);
    editor_add_cursor(&editor, 4);
    EXPECT(editor.cursor == 4 && editor_selection_count(&editor) == 2);
    editor_free(&editor);
}

static void test_typing_rules(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "");
    editor_type(&editor, "(", 1);
    EXPECT_TEXT(&editor, "()");
    EXPECT(editor.cursor == 1);
    editor_type(&editor, ")", 1); // types over the closing bracket
    EXPECT_TEXT(&editor, "()");
    EXPECT(editor.cursor == 2);
    load(&editor, "osc");
    editor_select_all(&editor);
    editor_type(&editor, "[", 1); // wraps the selection
    EXPECT_TEXT(&editor, "[osc]");
    load(&editor, "don");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_type(&editor, "'", 1); // no pair after a word character
    EXPECT_TEXT(&editor, "don'");
    load(&editor, "x = ");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_type(&editor, "\"", 1);
    EXPECT_TEXT(&editor, "x = \"\"");

    load(&editor, "  f(");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_type(&editor, "{", 1);
    editor_insert_newline(&editor); // between a pair: indent one level, the closer on its own line
    EXPECT_TEXT(&editor, "  f({\n    \n  }");
    EXPECT(editor.cursor == strlen("  f({\n    "));
    load(&editor, "{\n    ");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_type(&editor, "}", 1); // a closer on an empty line goes to the indentation of its opener
    EXPECT_TEXT(&editor, "{\n}");
    editor_free(&editor);
}

static void test_more_line_commands(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "a\nb\nc");
    editor_select_lines(&editor);
    EXPECT(editor.anchor == 0 && editor.cursor == 2);
    editor_select_lines(&editor); // again: the next line too
    EXPECT(editor.cursor == 4);
    editor_delete_lines(&editor);
    EXPECT_TEXT(&editor, "c");
    load(&editor, "osc()\n  .out()");
    editor_join_lines(&editor);
    EXPECT_TEXT(&editor, "osc() .out()");
    load(&editor, "c\na\nb");
    editor_select_all(&editor);
    editor_sort_lines(&editor);
    EXPECT_TEXT(&editor, "a\nb\nc");
    load(&editor, "ab");
    editor_set_cursor(&editor, 1, false);
    editor_transpose(&editor);
    EXPECT_TEXT(&editor, "ba");
    load(&editor, "x\n  y");
    editor_select_all(&editor);
    editor_indent_lines(&editor, false);
    EXPECT_TEXT(&editor, "  x\n    y");
    editor_indent_lines(&editor, true);
    editor_indent_lines(&editor, true);
    EXPECT_TEXT(&editor, "x\ny");
    load(&editor, "osc() noise()");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_delete_word(&editor, -1); // the brackets are one word of punctuation
    EXPECT_TEXT(&editor, "osc() noise");
    editor_go_to_line(&editor, 0);
    EXPECT(editor.cursor == 0);

    char **words;
    load(&editor, "colorama(); color(); speed = 1; col");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    size_t start, length = editor_word_before_cursor(&editor, &start);
    EXPECT(length == 3);
    size_t count = editor_words(&editor, "col", 3, &words);
    EXPECT(count == 2 && !strcmp(words[0], "colorama") && !strcmp(words[1], "color"));
    for (size_t i = 0; i < count; i++) free(words[i]);
    free(words);
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
    editor_visual_position(&editor, 18, &row, &x);
    EXPECT(row == 1 && x == 6);
    EXPECT(editor_offset_at_visual(&editor, 0, 1, 6) == 18);
    editor_set_cursor(&editor, 3, false);
    editor_move(&editor, MOTION_DOWN, false); // down moves by visual rows
    EXPECT(editor_line_of(&editor, editor.cursor) == 0 && editor.cursor == 16);
    load(&editor, "aaa bbb ccc");
    editor.wrap_columns = 8;
    rows = editor_wrap_rows(&editor, 0, starts, 8);
    EXPECT(rows == 2 && starts[1] == 8); // breaks after the last space
    editor_free(&editor);
}

int main(void) {
    test_typing_and_undo();
    test_utf8_and_motion();
    test_ranges();
    test_line_operations();
    test_search_and_brackets();
    test_multiple_cursors();
    test_typing_rules();
    test_more_line_commands();
    test_wrap();
    if (failures) {
        fprintf(stderr, "editor_test: %d failures\n", failures);
        return 1;
    }
    printf("editor_test: ok\n");
    return 0;
}
