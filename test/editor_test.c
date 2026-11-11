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
    editor_move(&editor, MOTION_GROUP_RIGHT, false);
    EXPECT(editor.cursor == 3);
    editor_move(&editor, MOTION_GROUP_RIGHT, true);
    EXPECT(editor.cursor == 4 && editor.anchor == 3);
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_move(&editor, MOTION_GROUP_LEFT, false);
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

// editor_format: tabs by bracket depth, one more level for a chain, no trailing spaces, comments and template
// strings kept, every cursor on its text, one undo step.
static void test_format(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "osc(10,\n0.1)\n.rotate()   \n    .out()\nshape(4)");
    editor_format(&editor);
    EXPECT_TEXT(&editor, "osc(10,\n\t0.1)\n\t.rotate()\n\t.out()\nshape(4)");
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "osc(10,\n0.1)\n.rotate()   \n    .out()\nshape(4)");

    load(&editor, "src(o0)\n  .modulate(\n  noise([1, 2,\n3]),\n  () => 0.1\n)\n  .out()\n   \n");
    editor_format(&editor);
    EXPECT_TEXT(&editor, "src(o0)\n\t.modulate(\n\t\tnoise([1, 2,\n\t\t\t3]),\n\t\t() => 0.1\n\t)\n\t.out()\n\n");

    load(&editor, "/* (\n   kept */\nosc(`(\n  kept`,\n 1) // (\n  .out()");
    editor_format(&editor);
    EXPECT_TEXT(&editor, "/* (\n   kept */\nosc(`(\n  kept`,\n\t1) // (\n\t.out()");

    // Two cursors, each after the "1" of its line, stay there.
    load(&editor, "a(\n    1)\nb(\n 1)");
    editor_set_cursor(&editor, editor_offset_at(&editor, 1, 5), false);
    editor_add_cursor(&editor, editor_offset_at(&editor, 3, 2));
    editor_format(&editor);
    EXPECT_TEXT(&editor, "a(\n\t1)\nb(\n\t1)");
    struct selection *selections;
    size_t count = editor_selections(&editor, &selections);
    EXPECT(count == 2 && selections[0].cursor == editor_line_start(&editor, 1) + 2 &&
           selections[1].cursor == editor_line_start(&editor, 3) + 2); // byte offsets; a tab is wider in the grid
    free(selections);
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
    EXPECT(editor_find(&editor, "a", 1, false, 1) && editor.anchor == 0); // a match at the cursor counts
    EXPECT(editor_find(&editor, "a", 1, false, 1) && editor.anchor == 4);
    EXPECT(editor_find(&editor, "a", 1, false, -1) && editor.anchor == 0);
    EXPECT(editor_replace_next(&editor, "a", 1, "xy", 2, false)); // replaces the selected "a", selects the next
    EXPECT_TEXT(&editor, "xy b a b a");
    EXPECT(editor.anchor == 5);
    EXPECT(editor_replace_all(&editor, "a", 1, "c", 1, false) == 2);
    EXPECT_TEXT(&editor, "xy b c b c");
    editor_undo(&editor);
    EXPECT_TEXT(&editor, "xy b a b a");

    // The find bar of the browser: no case, no accents, combining accents belong to the match, no overlaps.
    load(&editor, "Osc \xC3\x89t\xC3\xA9 e\xCC\x81 OSC aaa");
    struct editor_range *matches;
    size_t count = editor_find_all(&editor, "osc", 3, true, &matches);
    EXPECT(count == 2 && matches[0].start == 0 && matches[1].start == 14);
    free(matches);
    count = editor_find_all(&editor, "ete", 3, true, &matches); // "Été"
    EXPECT(count == 1 && matches[0].start == 4 && matches[0].end == 9);
    free(matches);
    count = editor_find_all(&editor, "e", 1, true, &matches); // "É", "é" and "e" with U+0301
    EXPECT(count == 3 && matches[2].start == 10 && matches[2].end == 13);
    free(matches);
    count = editor_find_all(&editor, "aa", 2, true, &matches);
    EXPECT(count == 1);
    free(matches);
    EXPECT(editor_find_all(&editor, "osc", 3, false, &matches) == 0);
    free(matches);
    editor_collapse(&editor);
    editor_set_cursor(&editor, 0, false);
    EXPECT(editor_find(&editor, "osc", 3, true, -1) && editor.anchor == 14); // around the start
    EXPECT(editor_replace_all(&editor, "\xC3\xA9T\xC3\x89", 5, "x", 1, true) == 1);
    EXPECT_TEXT(&editor, "Osc x e\xCC\x81 OSC aaa");
    load(&editor, "Stra\xC3\x9F" "e \xE3\x81\x8C \xEF\xBD\x8F\xEF\xBD\x93\xEF\xBD\x83"); // "Straße が ｏｓｃ"
    count = editor_find_all(&editor, "strasse", 7, true, &matches); // ß is ss
    EXPECT(count == 1 && matches[0].start == 0 && matches[0].end == 7);
    free(matches);
    EXPECT(editor_find_all(&editor, "stras", 5, true, &matches) == 0); // no match ends inside ß
    free(matches);
    count = editor_find_all(&editor, "\xE3\x82\xAB", 3, true, &matches); // katakana KA finds hiragana GA
    EXPECT(count == 1 && matches[0].start == 8 && matches[0].end == 11);
    free(matches);
    count = editor_find_all(&editor, "OSC", 3, true, &matches); // full width
    EXPECT(count == 1 && matches[0].start == 12);
    free(matches);

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
    editor_sort_lines(&editor, false, false);
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
    editor_delete_word(&editor, -1); // the brackets are one group of punctuation
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
    editor_caret_position(&editor, 18, STICKY_NONE, &row, &x);
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

// Grapheme clusters (UAX #29) for the grid, and the cursor stops of CodeMirror 5 (extending characters only).
static void test_clusters(void) {
    struct editor editor;
    editor_init(&editor);
    // "a", a family (man ZWJ woman ZWJ girl), "e" with U+0301, keycap 1, the flag of Switzerland, "b"
    load(&editor, "a\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
                  "e\xCC\x81" "1\xEF\xB8\x8F\xE2\x83\xA3" "\xF0\x9F\x87\xA8\xF0\x9F\x87\xAD" "b");
    EXPECT(editor_cluster_end(&editor, 1) == 19);  // the family is one cluster
    EXPECT(editor_cluster_end(&editor, 19) == 22); // e and the accent
    EXPECT(editor_cluster_end(&editor, 22) == 29); // the keycap
    EXPECT(editor_cluster_end(&editor, 29) == 37); // the flag
    EXPECT(editor_column_of(&editor, editor.length) == 1 + 2 + 1 + 2 + 2 + 1);
    size_t stops[] = { 1, 8, 15, 19, 22, 29, 33, 37, 38 }; // CodeMirror stops inside the family and the flag
    editor_set_cursor(&editor, 0, false);
    for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++) {
        editor_move(&editor, MOTION_RIGHT, false);
        EXPECT(editor.cursor == stops[i]);
    }
    editor_set_cursor(&editor, 22, false);
    editor_backspace(&editor); // e and its accent go together
    EXPECT(editor.cursor == 19 && editor.length == 35);
    editor_free(&editor);
}

#define MAX_STOPS 16

// The arrow keys in lines with right-to-left text, against moveVisually of CodeMirror 5.65.9: the stops come from
// running its kh() and bidiOrdering on the same lines in Node.js (offsets converted to bytes).
static void test_visual_motion(void) {
    static const struct {
        const char *text;
        struct { size_t offset; enum sticky sticky; } right[MAX_STOPS];
        size_t right_count;
        struct { size_t offset; enum sticky sticky; } left[MAX_STOPS];
        size_t left_count;
    } cases[] = {
    { "abc \xD7\x90" "\xD7\x91" "\xD7\x92" " 123 def", { { 1, STICKY_BEFORE }, { 2, STICKY_BEFORE }, { 3, STICKY_BEFORE }, { 4, STICKY_BEFORE }, { 12, STICKY_BEFORE }, { 13, STICKY_BEFORE }, { 14, STICKY_BEFORE }, { 10, STICKY_AFTER }, { 8, STICKY_AFTER }, { 6, STICKY_AFTER }, { 4, STICKY_AFTER }, { 15, STICKY_BEFORE }, { 16, STICKY_BEFORE }, { 17, STICKY_BEFORE }, { 18, STICKY_BEFORE } }, 15, { { 17, STICKY_AFTER }, { 16, STICKY_AFTER }, { 15, STICKY_AFTER }, { 14, STICKY_AFTER }, { 6, STICKY_BEFORE }, { 8, STICKY_BEFORE }, { 10, STICKY_BEFORE }, { 11, STICKY_BEFORE }, { 13, STICKY_AFTER }, { 12, STICKY_AFTER }, { 11, STICKY_AFTER }, { 3, STICKY_AFTER }, { 2, STICKY_AFTER }, { 1, STICKY_AFTER }, { 0, STICKY_AFTER } }, 15 },
    { "x = '\xD9\x85" "\xD8\xB1" "\xD8\xAD" "\xD8\xA8" "\xD8\xA7" "' + 1", { { 1, STICKY_BEFORE }, { 2, STICKY_BEFORE }, { 3, STICKY_BEFORE }, { 4, STICKY_BEFORE }, { 5, STICKY_BEFORE }, { 20, STICKY_BEFORE }, { 18, STICKY_AFTER }, { 17, STICKY_AFTER }, { 16, STICKY_AFTER }, { 15, STICKY_AFTER }, { 13, STICKY_AFTER }, { 11, STICKY_AFTER }, { 9, STICKY_AFTER }, { 7, STICKY_AFTER }, { 5, STICKY_AFTER } }, 15, { { 19, STICKY_AFTER }, { 4, STICKY_AFTER }, { 3, STICKY_AFTER }, { 2, STICKY_AFTER }, { 1, STICKY_AFTER }, { 0, STICKY_AFTER } }, 6 },
    { "\xD7\xA9" "\xD7\x9C" "\xD7\x95" "\xD7\x9D" " world", { { 9, STICKY_BEFORE }, { 10, STICKY_BEFORE }, { 11, STICKY_BEFORE }, { 12, STICKY_BEFORE }, { 13, STICKY_BEFORE }, { 14, STICKY_BEFORE } }, 6, { { 13, STICKY_AFTER }, { 12, STICKY_AFTER }, { 11, STICKY_AFTER }, { 10, STICKY_AFTER }, { 9, STICKY_AFTER }, { 8, STICKY_AFTER }, { 2, STICKY_BEFORE }, { 4, STICKY_BEFORE }, { 6, STICKY_BEFORE }, { 8, STICKY_BEFORE } }, 10 },
    };
    struct editor editor;
    editor_init(&editor);
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        load(&editor, cases[c].text);
        editor_set_cursor(&editor, 0, false);
        for (size_t i = 0; i < cases[c].right_count; i++) {
            editor_move(&editor, MOTION_RIGHT, false);
            if (editor.cursor != cases[c].right[i].offset || editor.sticky != cases[c].right[i].sticky)
                fprintf(stderr, "case %zu right %zu: %zu/%d, expected %zu/%d\n", c, i, editor.cursor, editor.sticky,
                        cases[c].right[i].offset, cases[c].right[i].sticky);
            EXPECT(editor.cursor == cases[c].right[i].offset && editor.sticky == cases[c].right[i].sticky);
        }
        editor_set_cursor(&editor, editor.length, false);
        for (size_t i = 0; i < cases[c].left_count; i++) {
            editor_move(&editor, MOTION_LEFT, false);
            if (editor.cursor != cases[c].left[i].offset || editor.sticky != cases[c].left[i].sticky)
                fprintf(stderr, "case %zu left %zu: %zu/%d, expected %zu/%d\n", c, i, editor.cursor, editor.sticky,
                        cases[c].left[i].offset, cases[c].left[i].sticky);
            EXPECT(editor.cursor == cases[c].left[i].offset && editor.sticky == cases[c].left[i].sticky);
        }
    }
    // The caret before the Hebrew word sits at the right edge of its first letter, which the row shows rightmost.
    load(&editor, "abc \xD7\x90\xD7\x91\xD7\x92");
    size_t row, x;
    editor_caret_position(&editor, 4, STICKY_AFTER, &row, &x);
    EXPECT(row == 0 && x == 7);
    editor_caret_position(&editor, 4, STICKY_BEFORE, &row, &x); // after the space
    EXPECT(x == 4);
    EXPECT(editor_offset_at_visual(&editor, 0, 0, 7) == 4);
    editor_free(&editor);
}

// Enter: (a) the indentation of the line stays, (b) inside an open ( or [ with entries the new line starts under
// the first entry, (c) directly after an opener one level more, with the closer on its own line.
static void test_enter_indentation(void) {
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

    load(&editor, "osc(10, noise(3,");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "osc(10, noise(3,\n              "); // the innermost open bracket
    load(&editor, "  osc(10, noise(3), '(',");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "  osc(10, noise(3), '(',\n      "); // noise( is closed, the bracket in the string does not count

    load(&editor, "  shape([])");
    editor_set_cursor(&editor, strlen("  shape(["), false);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "  shape([\n    \n  ])"); // (c)
    EXPECT(editor.cursor == strlen("  shape([\n    "));

    // Wrapped lines change nothing: the columns are those of the line, not of its rows.
    editor.wrap_columns = 8;
    load(&editor, "src(o0).blend(o1, [0.1, 0.2,");
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_insert_newline(&editor);
    EXPECT_TEXT(&editor, "src(o0).blend(o1, [0.1, 0.2,\n                   ");
    editor_free(&editor);
}

static void test_sublime_commands(void) {
    struct editor editor;
    editor_init(&editor);
    load(&editor, "modulateScale(o0)");
    editor_move(&editor, MOTION_SUBWORD_RIGHT, false);
    EXPECT(editor.cursor == 8); // "modulate|Scale"
    editor_move(&editor, MOTION_SUBWORD_RIGHT, false);
    EXPECT(editor.cursor == 13);
    editor_move(&editor, MOTION_SUBWORD_LEFT, false);
    EXPECT(editor.cursor == 8);

    load(&editor, "a\nb\nc");
    editor_set_cursor(&editor, 4, false);
    editor_toggle_bookmark(&editor); // line 2
    editor_set_cursor(&editor, 0, false);
    editor_insert(&editor, "x\n", 2, EDIT_OTHER); // a new line above moves the bookmark down
    EXPECT(editor.bookmark_count == 1 && editor.bookmarks[0] == 3);
    editor_next_bookmark(&editor, 1);
    EXPECT(editor_line_of(&editor, editor.cursor) == 3);
    editor_select_bookmarks(&editor);
    EXPECT(editor.anchor == editor_line_start(&editor, 3) && editor.cursor == editor.length);

    load(&editor, "osc noise osc");
    editor_find_under(&editor, 1); // the word, then its next match
    EXPECT(editor.anchor == 10 && editor.cursor == 13);
    editor_select_all_under(&editor);
    EXPECT(editor_selection_count(&editor) == 2);
    editor_change_case(&editor, true);
    EXPECT_TEXT(&editor, "OSC noise OSC");
    editor_collapse_to_first(&editor);
    EXPECT(editor.anchor == 0 && editor_selection_count(&editor) == 1);

    load(&editor, "abc def");
    editor_set_cursor(&editor, 4, false);
    editor_set_mark(&editor);
    editor_move(&editor, MOTION_DOCUMENT_END, false);
    editor_delete_to_mark(&editor);
    EXPECT_TEXT(&editor, "abc ");
    editor_move(&editor, MOTION_DOCUMENT_START, false);
    editor_yank(&editor);
    EXPECT_TEXT(&editor, "defabc ");
    editor_delete_line_part(&editor, 1);
    EXPECT_TEXT(&editor, "def");

    load(&editor, "// one two three four five six");
    editor_hard_wrap(&editor, 14);
    EXPECT_TEXT(&editor, "// one two\n// three four\n// five six");

    load(&editor, "\tosc()");
    EXPECT(editor_column_of(&editor, 1) == EDITOR_TAB_WIDTH); // a tab is 4 columns wide
    EXPECT(editor_offset_at(&editor, 0, EDITOR_TAB_WIDTH + 1) == 2);

    load(&editor, "     x");
    editor_set_cursor(&editor, 5, false);
    editor_backspace(&editor); // smart backspace: back to the indent stop at 4
    EXPECT_TEXT(&editor, "    x");
    editor_backspace(&editor);
    EXPECT_TEXT(&editor, "  x");

    load(&editor, "a a a");
    editor_select_word_at(&editor, 0);
    editor_skip_and_select_next(&editor);
    EXPECT(editor.anchor == 2 && editor_selection_count(&editor) == 1);
    editor_free(&editor);
}

int main(void) {
    test_typing_and_undo();
    test_utf8_and_motion();
    test_ranges();
    test_line_operations();
    test_format();
    test_search_and_brackets();
    test_multiple_cursors();
    test_typing_rules();
    test_more_line_commands();
    test_wrap();
    test_sublime_commands();
    test_group_motion();
    test_clusters();
    test_enter_indentation();
    test_visual_motion();
    if (failures) {
        fprintf(stderr, "editor_test: %d failures\n", failures);
        return 1;
    }
    printf("editor_test: ok\n");
    return 0;
}
