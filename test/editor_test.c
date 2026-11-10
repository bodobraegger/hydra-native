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

    load(&editor, "osc(10,\n0.1)\n.rotate()   \n    .out()\nshape(4)");
    editor_format(&editor);
    EXPECT_TEXT(&editor, "osc(10,\n  0.1)\n  .rotate()\n  .out()\nshape(4)");
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
    EXPECT(editor.anchor == 4 && editor.cursor == 6);
    editor_select_next_occurrence(&editor);
    EXPECT(editor.anchor == 12 && editor.cursor == 14);
    editor_select_next_occurrence(&editor); // wraps around
    EXPECT(editor.anchor == 4);
    editor_free(&editor);
}

int main(void) {
    test_typing_and_undo();
    test_utf8_and_motion();
    test_ranges();
    test_line_operations();
    test_search_and_brackets();
    if (failures) {
        fprintf(stderr, "editor_test: %d failures\n", failures);
        return 1;
    }
    printf("editor_test: ok\n");
    return 0;
}
