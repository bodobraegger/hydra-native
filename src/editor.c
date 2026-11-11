#include "editor.h"
#include "bidi.h"
#include "unicode.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_UNDO_STEPS 1000
#define MAX_WRAP_ROWS 1024
#define INDENT "  "
#define INDENT_WIDTH 2
#define MAX_NESTING 64
#define COMMENT_TOKEN "//"
#define OPENING_BRACKETS "([{"
#define CLOSING_BRACKETS ")]}"
#define QUOTES "'\"`"
// An opening bracket or quote gets its closing partner when the next character is one of these (or the end).
#define AUTO_CLOSE_BEFORE " \t\n)]};,."

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

static bool is_extending_at(const struct editor *editor, size_t offset) {
    size_t size;
    return unicode_is_codemirror_extending(unicode_decode((const unsigned char *)editor->text + offset, editor->length - offset, &size));
}

// moveCharLogically of CodeMirror 5 (arrow keys, Backspace and Delete): one code point, then over the extending
// characters after it, within the line.
static size_t cursor_step(const struct editor *editor, size_t offset, int direction) {
    size_t line = editor_line_of(editor, offset);
    size_t start = editor_line_start(editor, line), end = editor_line_end(editor, line);
    if (direction < 0) {
        if (offset == 0) return 0;
        offset = previous_char(editor, offset);
        while (offset > start && is_extending_at(editor, offset)) offset = previous_char(editor, offset);
    } else {
        if (offset >= editor->length) return editor->length;
        offset = next_char(editor, offset);
        while (offset < end && is_extending_at(editor, offset)) offset = next_char(editor, offset);
    }
    return offset;
}

enum char_class { CLASS_SPACE, CLASS_WORD, CLASS_PUNCTUATION };

static enum char_class class_of(unsigned char byte) {
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') return CLASS_SPACE;
    if (isalnum(byte) || byte == '_' || byte == '$' || byte >= 0x80) return CLASS_WORD;
    return CLASS_PUNCTUATION;
}

static bool is_identifier_char(unsigned char byte) { return isalnum(byte) || byte == '_' || byte == '$'; }

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

static char char_at(const struct editor *editor, size_t offset) {
    return offset < editor->length ? editor->text[offset] : '\0';
}

static char closing_partner(char c) {
    const char *open = strchr(OPENING_BRACKETS, c);
    return open && c ? CLOSING_BRACKETS[open - OPENING_BRACKETS] : c;
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

static size_t count_newlines(const char *text, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) count += text[i] == '\n';
    return count;
}

// Bookmarks stay on their text: lines after an edit move by the change of the line count.
static void shift_bookmarks(struct editor *editor, size_t start, size_t end, const char *text, size_t length) {
    if (!editor->bookmark_count) return;
    size_t first = editor_line_of(editor, start);
    size_t removed = count_newlines(editor->text + start, end - start), inserted = count_newlines(text, length);
    size_t kept = 0;
    for (size_t i = 0; i < editor->bookmark_count; i++) {
        size_t line = editor->bookmarks[i];
        if (line > first) line = line <= first + removed ? first : line - removed + inserted;
        if (kept == 0 || editor->bookmarks[kept - 1] != line) editor->bookmarks[kept++] = line;
    }
    editor->bookmark_count = kept;
}

// Replaces the bytes [start, end) with `text`. Callers record undo and set the selections.
static void replace(struct editor *editor, size_t start, size_t end, const char *text, size_t length) {
    if (editor->line_count) shift_bookmarks(editor, start, end, text, length);
    size_t new_length = editor->length - (end - start) + length;
    if (new_length + 1 > editor->capacity) {
        while (new_length + 1 > editor->capacity) editor->capacity = editor->capacity ? editor->capacity * 2 : 4096;
        editor->text = realloc(editor->text, editor->capacity);
    }
    memmove(editor->text + start + length, editor->text + end, editor->length - end);
    if (length) memcpy(editor->text + start, text, length);
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
    for (size_t i = 0; i < stack->count; i++) {
        free(stack->items[i].text);
        free(stack->items[i].selections);
    }
    stack->count = 0;
}

void editor_free(struct editor *editor) {
    free_snapshots(&editor->undo);
    free_snapshots(&editor->redo);
    free(editor->undo.items);
    free(editor->redo.items);
    free(editor->text);
    free(editor->line_starts);
    free(editor->extra);
    free(editor->bookmarks);
    free(editor->killed);
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

size_t editor_cluster_end(const struct editor *editor, size_t offset) {
    const unsigned char *text = (const unsigned char *)editor->text;
    if (offset + 1 < editor->length && text[offset] < 0x80 && text[offset + 1] < 0x80 && text[offset] != '\r') return offset + 1;
    return unicode_cluster_end((const unsigned char *)editor->text, editor_line_end(editor, editor_line_of(editor, offset)), offset);
}

// Columns of the grapheme cluster [offset, end) when it starts at `column`: a tab reaches the next multiple of
// EDITOR_TAB_WIDTH, a wide character or an emoji takes two.
static size_t cluster_columns(const struct editor *editor, size_t offset, size_t end, size_t column) {
    if (editor->text[offset] == '\t') return EDITOR_TAB_WIDTH - column % EDITOR_TAB_WIDTH;
    return (size_t)unicode_cluster_columns((const unsigned char *)editor->text, offset, end);
}

// A cluster that `end` cuts counts whole: the cursor of CodeMirror 5 can stop inside a ZWJ sequence, and the
// grid then shows it after the cluster.
size_t editor_columns_between(const struct editor *editor, size_t start, size_t end) {
    size_t columns = 0;
    for (size_t i = start; i < end;) {
        size_t cluster_end = editor_cluster_end(editor, i);
        columns += cluster_columns(editor, i, cluster_end, columns);
        i = cluster_end;
    }
    return columns;
}

size_t editor_column_of(const struct editor *editor, size_t offset) {
    return editor_columns_between(editor, editor_line_start(editor, editor_line_of(editor, offset)), offset);
}

// The offset `columns` columns after `offset` (not after `end`); a tab that does not fit stays after it.
static size_t advance_columns(const struct editor *editor, size_t offset, size_t end, size_t columns) {
    size_t column = 0;
    while (offset < end) {
        size_t cluster_end = editor_cluster_end(editor, offset);
        size_t width = cluster_columns(editor, offset, cluster_end, column);
        if (column + width > columns) break;
        column += width;
        offset = cluster_end;
    }
    return offset;
}

size_t editor_offset_at(const struct editor *editor, size_t line, size_t column) {
    return advance_columns(editor, editor_line_start(editor, line), editor_line_end(editor, line), column);
}

static const char *line_text(const struct editor *editor, size_t line, size_t *length) {
    size_t start = editor_line_start(editor, line);
    *length = editor_line_end(editor, line) - start;
    return editor->text + start;
}

static bool line_is_blank(const struct editor *editor, size_t line) {
    size_t length;
    const char *text = line_text(editor, line, &length);
    return is_blank(text, length);
}

// ---------------------------------------------------------------- selections

static size_t selection_start(const struct selection *selection) {
    return selection->cursor < selection->anchor ? selection->cursor : selection->anchor;
}

static size_t selection_end(const struct selection *selection) {
    return selection->cursor < selection->anchor ? selection->anchor : selection->cursor;
}

static int compare_selections(const void *a, const void *b) {
    size_t start_a = selection_start(a), start_b = selection_start(b);
    if (start_a != start_b) return start_a < start_b ? -1 : 1;
    size_t end_a = selection_end(a), end_b = selection_end(b);
    return end_a < end_b ? -1 : end_a > end_b;
}

static struct selection primary_selection(const struct editor *editor) {
    return (struct selection){ editor->cursor, editor->anchor, editor->goal_column, editor->has_goal_column, true, editor->sticky };
}

size_t editor_selection_count(const struct editor *editor) { return editor->extra_count + 1; }

size_t editor_selections(const struct editor *editor, struct selection **list) {
    size_t count = editor->extra_count + 1;
    *list = malloc(count * sizeof **list);
    (*list)[0] = primary_selection(editor);
    if (editor->extra_count) memcpy(*list + 1, editor->extra, editor->extra_count * sizeof **list);
    qsort(*list, count, sizeof **list, compare_selections);
    return count;
}

// Sorts and merges overlapping selections, then splits them into the primary one and the extra ones.
static void store_selections(struct editor *editor, struct selection *list, size_t count) {
    qsort(list, count, sizeof *list, compare_selections);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (list[i].cursor > editor->length) list[i].cursor = editor->length;
        if (list[i].anchor > editor->length) list[i].anchor = editor->length;
        struct selection *last = kept ? &list[kept - 1] : NULL;
        size_t start = selection_start(&list[i]), end = selection_end(&list[i]);
        bool caret = start == end;
        if (last && (start < selection_end(last) || (start == selection_end(last) && (caret || selection_start(last) == selection_end(last))))) {
            size_t merged_end = end > selection_end(last) ? end : selection_end(last);
            size_t merged_start = selection_start(last);
            bool forward = last->cursor >= last->anchor;
            last->anchor = forward ? merged_start : merged_end;
            last->cursor = forward ? merged_end : merged_start;
            if (list[i].primary) {
                last->primary = true;
                last->goal_column = list[i].goal_column;
                last->has_goal_column = list[i].has_goal_column;
                last->sticky = list[i].sticky;
            }
            continue;
        }
        list[kept++] = list[i];
    }
    size_t primary = kept - 1;
    for (size_t i = 0; i < kept; i++)
        if (list[i].primary) primary = i;
    editor->cursor = list[primary].cursor;
    editor->anchor = list[primary].anchor;
    editor->goal_column = list[primary].goal_column;
    editor->has_goal_column = list[primary].has_goal_column;
    editor->sticky = list[primary].sticky;
    if (kept - 1 > editor->extra_capacity) {
        editor->extra_capacity = kept;
        editor->extra = realloc(editor->extra, editor->extra_capacity * sizeof *editor->extra);
    }
    editor->extra_count = 0;
    for (size_t i = 0; i < kept; i++) {
        if (i == primary) continue;
        editor->extra[editor->extra_count] = list[i];
        editor->extra[editor->extra_count++].primary = false;
    }
}

typedef void (*selection_function)(struct editor *editor, void *data);

// Runs `function` once per selection, from the first to the last, with that selection as the primary one.
// Offsets of the later selections shift by the length change of the earlier edits.
static void for_each_selection(struct editor *editor, selection_function function, void *data) {
    struct selection *list;
    size_t count = editor_selections(editor, &list);
    long delta = 0;
    for (size_t i = 0; i < count; i++) {
        editor->cursor = (size_t)((long)list[i].cursor + delta);
        editor->anchor = (size_t)((long)list[i].anchor + delta);
        editor->goal_column = list[i].goal_column;
        editor->has_goal_column = list[i].has_goal_column;
        editor->sticky = list[i].sticky;
        size_t length_before = editor->length;
        function(editor, data);
        delta += (long)editor->length - (long)length_before;
        list[i].cursor = editor->cursor;
        list[i].anchor = editor->anchor;
        list[i].goal_column = editor->goal_column;
        list[i].has_goal_column = editor->has_goal_column;
        list[i].sticky = editor->sticky;
    }
    store_selections(editor, list, count);
    free(list);
}

static bool any_selection(const struct editor *editor) {
    if (editor->cursor != editor->anchor) return true;
    for (size_t i = 0; i < editor->extra_count; i++)
        if (editor->extra[i].cursor != editor->extra[i].anchor) return true;
    return false;
}

bool editor_has_selection(const struct editor *editor) { return editor->cursor != editor->anchor; }

void editor_selection(const struct editor *editor, size_t *start, size_t *end) {
    *start = editor->cursor < editor->anchor ? editor->cursor : editor->anchor;
    *end = editor->cursor < editor->anchor ? editor->anchor : editor->cursor;
}

char *editor_selected_text(const struct editor *editor, size_t *length) {
    struct selection *list;
    size_t count = editor_selections(editor, &list), used = 0, capacity = count;
    for (size_t i = 0; i < count; i++) capacity += selection_end(&list[i]) - selection_start(&list[i]);
    char *text = malloc(capacity + 1);
    for (size_t i = 0; i < count; i++) {
        size_t start = selection_start(&list[i]), end = selection_end(&list[i]);
        if (i > 0) text[used++] = '\n';
        memcpy(text + used, editor->text + start, end - start);
        used += end - start;
    }
    text[used] = '\0';
    *length = used;
    free(list);
    return text;
}

void editor_set_cursor(struct editor *editor, size_t offset, bool extend) {
    editor->cursor = offset > editor->length ? editor->length : offset;
    if (!extend) editor->anchor = editor->cursor;
    editor->last_edit = EDIT_OTHER;
    editor->has_goal_column = false;
    editor->sticky = STICKY_NONE;
}

void editor_collapse(struct editor *editor) { editor->extra_count = 0; }

void editor_collapse_to_first(struct editor *editor) {
    struct selection *list;
    editor_selections(editor, &list);
    editor->cursor = list[0].cursor;
    editor->anchor = list[0].anchor;
    editor->extra_count = 0;
    free(list);
}

static void add_selection(struct editor *editor, size_t anchor, size_t cursor) {
    struct selection *list;
    size_t count = editor_selections(editor, &list);
    list = realloc(list, (count + 1) * sizeof *list);
    for (size_t i = 0; i < count; i++) list[i].primary = false;
    list[count] = (struct selection){ cursor, anchor, 0, false, true, STICKY_NONE };
    store_selections(editor, list, count + 1);
    free(list);
    editor->last_edit = EDIT_OTHER;
}

void editor_add_cursor(struct editor *editor, size_t offset) {
    add_selection(editor, offset > editor->length ? editor->length : offset, offset > editor->length ? editor->length : offset);
}

void editor_column_select(struct editor *editor, size_t anchor_line, size_t anchor_column, size_t line, size_t column) {
    size_t first = anchor_line < line ? anchor_line : line, last = anchor_line < line ? line : anchor_line;
    if (last >= editor->line_count) last = editor->line_count - 1;
    size_t count = last - first + 1;
    struct selection *list = calloc(count, sizeof *list);
    for (size_t i = 0; i < count; i++) {
        list[i].anchor = editor_offset_at(editor, first + i, anchor_column);
        list[i].cursor = editor_offset_at(editor, first + i, column);
        list[i].primary = first + i == line;
    }
    store_selections(editor, list, count);
    free(list);
    editor->last_edit = EDIT_OTHER;
}

// ---------------------------------------------------------------- soft wrap

size_t editor_row_indent(const struct editor *editor, size_t line) {
    size_t length;
    const char *text = line_text(editor, line, &length);
    size_t start = editor_line_start(editor, line);
    size_t indent = editor_columns_between(editor, start, start + leading_whitespace(text, length)) + EDITOR_HANGING_INDENT;
    return editor->wrap_columns && indent > editor->wrap_columns / 2 ? editor->wrap_columns / 2 : indent;
}

size_t editor_wrap_rows(const struct editor *editor, size_t line, size_t *row_starts, size_t capacity) {
    size_t start = editor_line_start(editor, line), end = editor_line_end(editor, line);
    if (capacity) row_starts[0] = start;
    if (editor->wrap_columns == 0) return 1;
    size_t rows = 1, position = start, width = editor->wrap_columns;
    size_t continuation_width = editor->wrap_columns - editor_row_indent(editor, line);
    if (continuation_width < 1) continuation_width = 1;
    for (;;) {
        size_t limit = advance_columns(editor, position, end, width);
        if (limit >= end) break;
        size_t row_break = limit;
        // Break after the last space of the row, like word wrap in Pulsar; a long word breaks hard.
        // The indentation at the start of the line is no break point.
        size_t content = position == start ? start + leading_whitespace(editor->text + start, end - start) : position;
        for (size_t i = limit; i > content + 1; i--) {
            if (editor->text[i - 1] == ' ') {
                row_break = i;
                break;
            }
        }
        if (rows < capacity) row_starts[rows] = row_break;
        rows++;
        position = row_break;
        width = continuation_width;
    }
    return rows;
}

// ---------------------------------------------------------------- right-to-left text in rows

bool editor_line_has_rtl(const struct editor *editor, size_t line) {
    size_t start = editor_line_start(editor, line);
    return bidi_line_has_rtl(editor->text + start, editor_line_end(editor, line) - start);
}

size_t editor_row_cells(const struct editor *editor, size_t line, size_t row_start, size_t row_end, size_t indent,
                        struct editor_cell **cells) {
    size_t line_start = editor_line_start(editor, line), line_length = editor_line_end(editor, line) - line_start;
    unsigned char *levels = malloc(line_length + 1);
    bidi_row_levels(editor->text + line_start, line_length, row_start - line_start, row_end - line_start, levels);
    size_t count = 0;
    *cells = malloc(sizeof **cells * (row_end - row_start + 1));
    for (size_t i = row_start; i < row_end; i = editor_cluster_end(editor, i))
        (*cells)[count++] = (struct editor_cell){ i, editor_cluster_end(editor, i), 0, 0, levels[i - line_start] };
    free(levels);
    // Rule L2: reverse the sequences at each level, from the highest to the lowest odd level.
    unsigned char highest = 0, lowest_odd = UCHAR_MAX;
    for (size_t i = 0; i < count; i++) {
        if ((*cells)[i].level > highest) highest = (*cells)[i].level;
        if ((*cells)[i].level % 2 && (*cells)[i].level < lowest_odd) lowest_odd = (*cells)[i].level;
    }
    for (int level = highest; lowest_odd != UCHAR_MAX && level >= lowest_odd; level--) {
        for (size_t i = 0; i < count;) {
            if ((*cells)[i].level < level) { i++; continue; }
            size_t end = i;
            while (end < count && (*cells)[end].level >= level) end++;
            for (size_t a = i, b = end - 1; a < b; a++, b--) {
                struct editor_cell swap = (*cells)[a];
                (*cells)[a] = (*cells)[b];
                (*cells)[b] = swap;
            }
            i = end;
        }
    }
    size_t column = 0;
    for (size_t i = 0; i < count; i++) {
        struct editor_cell *cell = &(*cells)[i];
        cell->width = cluster_columns(editor, cell->start, cell->end, column);
        cell->column = indent + column;
        column += cell->width;
    }
    return count;
}

// The visual column of a caret in a row with right-to-left text: at the end of the character before it
// (STICKY_BEFORE, or at the end of the row) or at the start of the character after it, in that character's
// direction.
static size_t caret_column(const struct editor *editor, size_t line, size_t row_start, size_t row_end, size_t indent,
                           size_t offset, enum sticky sticky) {
    struct editor_cell *cells;
    size_t count = editor_row_cells(editor, line, row_start, row_end, indent, &cells), column = indent;
    bool before = (sticky == STICKY_BEFORE && offset > row_start) || offset >= row_end;
    for (size_t i = 0; i < count; i++) {
        const struct editor_cell *cell = &cells[i];
        bool rtl = cell->level % 2;
        if (before && cell->start < offset && offset <= cell->end) column = rtl ? cell->column : cell->column + cell->width;
        else if (!before && cell->start <= offset && offset < cell->end) column = rtl ? cell->column + cell->width : cell->column;
        else continue;
        break;
    }
    free(cells);
    return column;
}

// ---------------------------------------------------------------- visual positions

void editor_caret_position(const struct editor *editor, size_t offset, enum sticky sticky, size_t *row, size_t *x) {
    size_t line = editor_line_of(editor, offset);
    size_t starts[MAX_WRAP_ROWS];
    size_t rows = editor_wrap_rows(editor, line, starts, MAX_WRAP_ROWS);
    if (rows > MAX_WRAP_ROWS) rows = MAX_WRAP_ROWS;
    size_t current = 0;
    while (current + 1 < rows && starts[current + 1] <= offset) current++;
    *row = current;
    size_t indent = current > 0 ? editor_row_indent(editor, line) : 0;
    size_t end = current + 1 < rows ? starts[current + 1] : editor_line_end(editor, line);
    *x = editor_line_has_rtl(editor, line) ? caret_column(editor, line, starts[current], end, indent, offset, sticky)
                                           : editor_columns_between(editor, starts[current], offset) + indent;
}

// The caret position nearest to column x in a row with right-to-left text.
static size_t offset_at_column(const struct editor *editor, size_t line, size_t row_start, size_t row_end, size_t indent, size_t x) {
    struct editor_cell *cells;
    size_t count = editor_row_cells(editor, line, row_start, row_end, indent, &cells), best = row_end, best_distance = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        bool rtl = cells[i].level % 2;
        size_t edges[2][2] = { { cells[i].column, rtl ? cells[i].end : cells[i].start },
                               { cells[i].column + cells[i].width, rtl ? cells[i].start : cells[i].end } };
        for (int k = 0; k < 2; k++) {
            size_t distance = edges[k][0] > x ? edges[k][0] - x : x - edges[k][0];
            if (distance < best_distance) {
                best_distance = distance;
                best = edges[k][1];
            }
        }
    }
    free(cells);
    return best;
}

size_t editor_offset_at_visual(const struct editor *editor, size_t line, size_t row, size_t x) {
    size_t starts[MAX_WRAP_ROWS];
    size_t rows = editor_wrap_rows(editor, line, starts, MAX_WRAP_ROWS);
    if (rows > MAX_WRAP_ROWS) rows = MAX_WRAP_ROWS;
    if (row >= rows) row = rows - 1;
    size_t indent = row > 0 ? editor_row_indent(editor, line) : 0;
    size_t end = row + 1 < rows ? starts[row + 1] : editor_line_end(editor, line);
    if (editor_line_has_rtl(editor, line)) {
        size_t offset = offset_at_column(editor, line, starts[row], end, indent, x);
        return offset == end && row + 1 < rows ? previous_char(editor, end) : offset;
    }
    size_t offset = advance_columns(editor, starts[row], end, x > indent ? x - indent : 0);
    // The end of a wrapped row is the start of the next row; stay on this row.
    if (offset == end && row + 1 < rows) offset = previous_char(editor, end);
    return offset;
}

// ---------------------------------------------------------------- undo

static void push_snapshot(struct snapshot_stack *stack, const struct editor *editor) {
    if (stack->count == MAX_UNDO_STEPS) {
        free(stack->items[0].text);
        free(stack->items[0].selections);
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
    snapshot->selection_count = editor->extra_count + 1;
    snapshot->selections = malloc(snapshot->selection_count * sizeof *snapshot->selections);
    snapshot->selections[0] = primary_selection(editor);
    if (editor->extra_count) memcpy(snapshot->selections + 1, editor->extra, editor->extra_count * sizeof *editor->extra);
}

static void begin_edit(struct editor *editor, enum edit_kind kind) {
    bool merge = kind != EDIT_OTHER && kind == editor->last_edit && !any_selection(editor);
    if (!merge) {
        push_snapshot(&editor->undo, editor);
        free_snapshots(&editor->redo);
    }
    editor->last_edit = kind;
}

static bool restore(struct editor *editor, struct snapshot_stack *from, struct snapshot_stack *to) {
    if (from->count == 0) return false;
    push_snapshot(to, editor);
    struct snapshot snapshot = from->items[--from->count];
    replace(editor, 0, editor->length, snapshot.text, snapshot.length);
    store_selections(editor, snapshot.selections, snapshot.selection_count);
    editor->last_edit = EDIT_OTHER;
    editor->has_goal_column = false;
    free(snapshot.text);
    free(snapshot.selections);
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
    editor->extra_count = 0;
    editor->last_edit = EDIT_OTHER;
    editor->modified = false;
}

void editor_replace_text(struct editor *editor, const char *text, size_t length) {
    if (length == editor->length && memcmp(text, editor->text, length) == 0) return;
    size_t line = editor_line_of(editor, editor->cursor);
    size_t column = editor->cursor - editor_line_start(editor, line);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, 0, editor->length, text, length);
    if (line >= editor->line_count) line = editor->line_count - 1;
    size_t line_length = editor_line_end(editor, line) - editor_line_start(editor, line);
    size_t offset = editor_line_start(editor, line) + (column < line_length ? column : line_length);
    while (offset > 0 && is_continuation((unsigned char)editor->text[offset])) offset--;
    editor->cursor = editor->anchor = offset;
    editor->extra_count = 0;
}

// ---------------------------------------------------------------- motion

static void vertical(struct editor *editor, long rows, bool extend) {
    size_t row, x;
    editor_caret_position(editor, editor->cursor, STICKY_NONE, &row, &x);
    size_t goal = editor->has_goal_column ? editor->goal_column : x;
    size_t line = editor_line_of(editor, editor->cursor);
    size_t starts[1];
    size_t offset = (size_t)-1;
    for (; rows > 0 && offset == (size_t)-1; rows--) {
        if (row + 1 < editor_wrap_rows(editor, line, starts, 1)) row++;
        else if (line + 1 < editor->line_count) line++, row = 0;
        else offset = editor->length;
    }
    for (; rows < 0 && offset == (size_t)-1; rows++) {
        if (row > 0) row--;
        else if (line > 0) line--, row = editor_wrap_rows(editor, line, starts, 1) - 1;
        else offset = 0;
    }
    if (offset == (size_t)-1) offset = editor_offset_at_visual(editor, line, row, goal);
    editor_set_cursor(editor, offset, extend);
    editor->goal_column = goal;
    editor->has_goal_column = true;
}

// ---------------------------------------------------------------- visual cursor motion (CodeMirror 5)

#define MAX_BIDI_SPANS 256
#define NO_POSITION SIZE_MAX

// bidiOrdering of CodeMirror 5 for a line, with offsets of the whole text.
static size_t line_order(const struct editor *editor, size_t line, struct bidi_span *spans) {
    size_t start = editor_line_start(editor, line);
    size_t count = bidi_codemirror_order(editor->text + start, editor_line_end(editor, line) - start, spans, MAX_BIDI_SPANS);
    for (size_t i = 0; i < count; i++) {
        spans[i].from += start;
        spans[i].to += start;
    }
    return count;
}

// getBidiPartAt of CodeMirror 5.
static size_t part_at(const struct bidi_span *order, size_t count, size_t offset, enum sticky sticky) {
    size_t found = NO_POSITION, other = NO_POSITION;
    for (size_t i = 0; i < count; i++) {
        const struct bidi_span *part = &order[i];
        if (part->from < offset && part->to > offset) return i;
        if (part->to == offset) {
            if (part->from != part->to && sticky == STICKY_BEFORE) found = i;
            else other = i;
        }
        if (part->from == offset) {
            if (part->from != part->to && sticky != STICKY_BEFORE) found = i;
            else other = i;
        }
    }
    return found != NO_POSITION ? found : other;
}

// moveCharLogically within the line: NO_POSITION at its edges.
static size_t step_in_line(const struct editor *editor, size_t line, size_t offset, int direction) {
    if (offset == NO_POSITION || (direction < 0 ? offset <= editor_line_start(editor, line) : offset >= editor_line_end(editor, line)))
        return NO_POSITION;
    return cursor_step(editor, offset, direction);
}

struct extent {
    size_t begin, end;
};

// The visual row (wrappedLineExtentChar) that holds `offset`.
static struct extent row_extent(const struct editor *editor, size_t line, size_t offset) {
    size_t starts[MAX_WRAP_ROWS];
    size_t rows = editor_wrap_rows(editor, line, starts, MAX_WRAP_ROWS);
    if (rows > MAX_WRAP_ROWS) rows = MAX_WRAP_ROWS;
    size_t current = 0;
    while (current + 1 < rows && starts[current + 1] <= offset) current++;
    return (struct extent){ starts[current], current + 1 < rows ? starts[current + 1] : editor_line_end(editor, line) };
}

// searchInVisualLine of moveVisually.
static size_t search_in_row(const struct editor *editor, size_t line, const struct bidi_span *order, size_t count, long index,
                            int direction, struct extent extent, enum sticky *sticky) {
    for (; index >= 0 && (size_t)index < count; index += direction) {
        const struct bidi_span *part = &order[index];
        bool storage_order = (direction > 0) == (part->level != 1);
        size_t offset = storage_order ? extent.begin : step_in_line(editor, line, extent.end, -1);
        if (!(part->from <= offset && offset < part->to)) {
            offset = storage_order ? part->from : step_in_line(editor, line, part->to, -1);
            if (!(extent.begin <= offset && offset < extent.end)) continue;
        }
        *sticky = storage_order ? STICKY_BEFORE : STICKY_AFTER;
        return storage_order ? step_in_line(editor, line, offset, 1) : offset;
    }
    return NO_POSITION;
}

// moveVisually of CodeMirror 5 (the arrow keys, as rtlMoveVisually is on outside Windows): in a line with
// right-to-left text, the next position to the left or right on the screen; NO_POSITION at the end of the line.
static size_t move_visually(const struct editor *editor, size_t offset, enum sticky *sticky, int direction) {
    size_t line = editor_line_of(editor, offset), start = editor_line_start(editor, line), end = editor_line_end(editor, line);
    struct bidi_span order[MAX_BIDI_SPANS];
    size_t count = line_order(editor, line, order), next;
    if (!count) {
        next = step_in_line(editor, line, offset, direction);
        *sticky = direction < 0 ? STICKY_AFTER : STICKY_BEFORE;
        return next;
    }
    if (offset >= end) {
        offset = end;
        *sticky = STICKY_BEFORE;
    } else if (offset <= start) {
        offset = start;
        *sticky = STICKY_AFTER;
    }
    size_t index = part_at(order, count, offset, *sticky);
    const struct bidi_span *part = &order[index];
    if (part->level % 2 == 0 && (direction > 0 ? part->to > offset : part->from < offset)) {
        next = step_in_line(editor, line, offset, direction);
        *sticky = direction < 0 ? STICKY_AFTER : STICKY_BEFORE;
        return next;
    }
    struct extent extent = row_extent(editor, line, *sticky == STICKY_BEFORE ? step_in_line(editor, line, offset, -1) : offset);
    if (part->level == 1) {
        bool storage_order = direction < 0;
        next = step_in_line(editor, line, offset, storage_order ? 1 : -1);
        if (next != NO_POSITION && (storage_order ? next <= part->to && next <= extent.end : next >= part->from && next >= extent.begin)) {
            *sticky = storage_order ? STICKY_BEFORE : STICKY_AFTER;
            return next;
        }
    }
    next = search_in_row(editor, line, order, count, (long)index + direction, direction, extent, sticky);
    if (next != NO_POSITION) return next;
    size_t next_row = direction > 0 ? extent.end : step_in_line(editor, line, extent.begin, -1);
    if (next_row != NO_POSITION && !(direction > 0 && next_row == end))
        return search_in_row(editor, line, order, count, direction > 0 ? 0 : (long)count - 1, direction,
                             row_extent(editor, line, next_row), sticky);
    return NO_POSITION;
}

// endOfLine of CodeMirror 5 with visual motion: where the cursor enters a line from the previous one
// (direction 1) or from the next one (direction -1).
static size_t line_visual_edge(const struct editor *editor, size_t line, int direction, enum sticky *sticky) {
    struct bidi_span order[MAX_BIDI_SPANS];
    size_t count = line_order(editor, line, order);
    if (!count) {
        *sticky = direction < 0 ? STICKY_BEFORE : STICKY_AFTER;
        return direction < 0 ? editor_line_end(editor, line) : editor_line_start(editor, line);
    }
    const struct bidi_span *part = direction < 0 ? &order[count - 1] : &order[0];
    bool storage_order = (direction < 0) == (part->level == 1);
    *sticky = storage_order ? STICKY_AFTER : STICKY_BEFORE;
    if (part->level == 0) return direction < 0 ? part->to : part->from;
    size_t offset = (direction < 0) == (part->level == 1) ? part->from : step_in_line(editor, line, part->to, -1);
    return *sticky == STICKY_BEFORE ? step_in_line(editor, line, offset, 1) : offset;
}

// The arrow keys: moveVisually, then into the next or the previous line.
static size_t arrow_step(const struct editor *editor, size_t offset, enum sticky *sticky, int direction) {
    size_t next = move_visually(editor, offset, sticky, direction), line = editor_line_of(editor, offset);
    if (next != NO_POSITION) return next;
    if (direction > 0 ? line + 1 >= editor->line_count : line == 0) return offset;
    return line_visual_edge(editor, line + (size_t)direction, direction, sticky);
}

// The word characters of CodeMirror 5: \w and letters outside ASCII ("$" is no word character there).
static bool is_codemirror_word(const struct editor *editor, size_t offset) {
    unsigned char c = (unsigned char)editor->text[offset];
    return isalnum(c) || c == '_' || c >= 0x80;
}

// findPosH(..., "group") of CodeMirror 5, the motion of Ctrl+Left and Ctrl+Right (goGroupLeft, goGroupRight) and of
// Ctrl+Backspace and Ctrl+Delete: a run of word characters, of punctuation, or one line end; spaces before a run
// go with it.
static size_t group_position(const struct editor *editor, size_t offset, int direction) {
    size_t position = offset;
    char saw = 0;
    // Moves one character; across a line end only on the first move.
    #define MOVE_ONCE(bound_to_line) \
        (direction < 0 ? (position > 0 && (!(bound_to_line) || editor->text[position - 1] != '\n') \
                              ? (position = cursor_step(editor, position, -1), true) : false) \
                       : (position < editor->length && (!(bound_to_line) || editor->text[position] != '\n') \
                              ? (position = cursor_step(editor, position, 1), true) : false))
    for (bool first = true;; first = false) {
        if (direction < 0 && !MOVE_ONCE(!first)) break;
        char type;
        if (position >= editor->length || editor->text[position] == '\n') type = 'n';
        else if (is_codemirror_word(editor, position)) type = 'w';
        else if (isspace((unsigned char)editor->text[position])) type = 0;
        else type = 'p';
        if (!first && !type) type = 's';
        if (saw && saw != type) {
            if (direction < 0) {
                direction = 1;
                MOVE_ONCE(false);
            }
            break;
        }
        if (type) saw = type;
        if (direction > 0 && !MOVE_ONCE(!first)) break;
    }
    #undef MOVE_ONCE
    return position;
}

enum subword_class { SUBWORD_OTHER, SUBWORD_LOWER, SUBWORD_UPPER };

static enum subword_class subword_class(char c) {
    if (c != '_' && class_of((unsigned char)c) == CLASS_WORD) return isupper((unsigned char)c) ? SUBWORD_UPPER : SUBWORD_LOWER;
    return SUBWORD_OTHER;
}

// findPosSubword of the CodeMirror 5 sublime keymap: camelCase parts and words; at a line edge, the next line.
static size_t subword(const struct editor *editor, size_t offset, int direction) {
    size_t line = editor_line_of(editor, offset), start = editor_line_start(editor, line), end = editor_line_end(editor, line);
    if (direction < 0 && offset == start) return line > 0 ? editor_line_end(editor, line - 1) : 0;
    if (direction > 0 && offset >= end) return line + 1 < editor->line_count ? editor_line_start(editor, line + 1) : end;
    enum { STATE_START, STATE_IN } state = STATE_START;
    enum subword_class type = SUBWORD_OTHER;
    size_t position = offset, word_start = offset, limit = direction < 0 ? start : end;
    for (; position != limit; position = direction < 0 ? position - 1 : position + 1) {
        enum subword_class class = subword_class(editor->text[direction < 0 ? position - 1 : position]);
        if (state == STATE_START) {
            if (class != SUBWORD_OTHER) state = STATE_IN, type = class;
            else word_start = direction < 0 ? position - 1 : position + 1;
        } else if (type != class) {
            if (type == SUBWORD_LOWER && class == SUBWORD_UPPER && direction < 0) position--;
            if (type == SUBWORD_UPPER && class == SUBWORD_LOWER && direction > 0) {
                if (position == word_start + 1) {
                    type = SUBWORD_LOWER;
                    continue;
                }
                position--;
            }
            break;
        }
    }
    return position;
}

struct motion_arguments {
    enum motion motion;
    bool extend;
    long rows;
};

static void move_one(struct editor *editor, void *data) {
    const struct motion_arguments *arguments = data;
    size_t start, end, offset = editor->cursor;
    editor_selection(editor, &start, &end);
    size_t line = editor_line_of(editor, offset);
    bool collapse = editor_has_selection(editor) && !arguments->extend;
    switch (arguments->motion) {
    case MOTION_LEFT:
    case MOTION_RIGHT: {
        int direction = arguments->motion == MOTION_LEFT ? -1 : 1;
        enum sticky sticky = editor->sticky;
        if (collapse) offset = direction < 0 ? start : end;
        else offset = arrow_step(editor, offset, &sticky, direction);
        editor_set_cursor(editor, offset, arguments->extend);
        editor->sticky = collapse ? STICKY_NONE : sticky;
        return;
    }
    case MOTION_UP: vertical(editor, -arguments->rows, arguments->extend); return;
    case MOTION_DOWN: vertical(editor, arguments->rows, arguments->extend); return;
    case MOTION_LINE_START: {
        size_t line_start = editor_line_start(editor, line);
        size_t first = line_start + leading_whitespace(editor->text + line_start, editor_line_end(editor, line) - line_start);
        offset = offset == first ? line_start : first;
        break;
    }
    case MOTION_LINE_END: offset = editor_line_end(editor, line); break;
    case MOTION_GROUP_LEFT: offset = group_position(editor, offset, -1); break;
    case MOTION_GROUP_RIGHT: offset = group_position(editor, offset, 1); break;
    case MOTION_SUBWORD_LEFT: offset = collapse ? start : subword(editor, offset, -1); break;
    case MOTION_SUBWORD_RIGHT: offset = collapse ? end : subword(editor, offset, 1); break;
    case MOTION_DOCUMENT_START: offset = 0; break;
    case MOTION_DOCUMENT_END: offset = editor->length; break;
    }
    editor_set_cursor(editor, offset, arguments->extend);
}

void editor_move(struct editor *editor, enum motion motion, bool extend) {
    struct motion_arguments arguments = { motion, extend, 1 };
    for_each_selection(editor, move_one, &arguments);
}

void editor_move_vertical(struct editor *editor, long rows, bool extend) {
    struct motion_arguments arguments = { rows < 0 ? MOTION_UP : MOTION_DOWN, extend, rows < 0 ? -rows : rows };
    for_each_selection(editor, move_one, &arguments);
}

void editor_add_cursor_vertical(struct editor *editor, int direction) {
    struct selection *list;
    size_t count = editor_selections(editor, &list);
    struct selection outer = direction < 0 ? list[0] : list[count - 1];
    free(list);
    struct selection primary = primary_selection(editor);
    editor->cursor = editor->anchor = outer.cursor;
    editor->has_goal_column = outer.has_goal_column;
    editor->goal_column = outer.goal_column;
    vertical(editor, direction, false);
    size_t offset = editor->cursor, goal = editor->goal_column;
    editor->cursor = primary.cursor;
    editor->anchor = primary.anchor;
    if (offset == outer.cursor) return;
    add_selection(editor, offset, offset);
    editor->goal_column = goal;
    editor->has_goal_column = true;
}

void editor_select_all(struct editor *editor) {
    editor_collapse(editor);
    editor_set_cursor(editor, 0, false);
    editor_set_cursor(editor, editor->length, true);
}

static void word_range(const struct editor *editor, size_t offset, size_t *start, size_t *end) {
    if (offset > editor->length) offset = editor->length;
    *start = *end = offset;
    unsigned char byte = offset < editor->length ? (unsigned char)editor->text[offset] : ' ';
    if (class_of(byte) != CLASS_WORD && offset > 0) byte = (unsigned char)editor->text[offset - 1];
    enum char_class class = class_of(byte);
    if (class == CLASS_SPACE) return;
    while (*start > 0 && class_of((unsigned char)editor->text[*start - 1]) == class) (*start)--;
    while (*end < editor->length && class_of((unsigned char)editor->text[*end]) == class) (*end)++;
}

void editor_select_word_at(struct editor *editor, size_t offset) {
    size_t start, end;
    word_range(editor, offset, &start, &end);
    editor_set_cursor(editor, start, false);
    editor_set_cursor(editor, end, true);
}

static void select_lines_one(struct editor *editor, void *data) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t first = editor_line_of(editor, start), last = editor_line_of(editor, end);
    if (end > start && end == editor_line_start(editor, last)) last--;
    size_t line_start = editor_line_start(editor, first);
    size_t next = last + 1 < editor->line_count ? editor_line_start(editor, last + 1) : editor->length;
    if (start == line_start && end == next && next < editor->length)
        next = last + 2 < editor->line_count ? editor_line_start(editor, last + 2) : editor->length;
    editor->anchor = line_start;
    editor->cursor = next;
}

void editor_select_lines(struct editor *editor) { for_each_selection(editor, select_lines_one, NULL); }

void editor_split_selection_by_line(struct editor *editor) {
    struct selection *list;
    size_t count = editor_selections(editor, &list), capacity = count, used = 0;
    struct selection *result = malloc(capacity * sizeof *result);
    for (size_t i = 0; i < count; i++) {
        size_t start = selection_start(&list[i]), end = selection_end(&list[i]);
        size_t first = editor_line_of(editor, start), last = editor_line_of(editor, end);
        for (size_t line = first; line <= last; line++) {
            if (used == capacity) result = realloc(result, (capacity *= 2) * sizeof *result);
            size_t from = line == first ? start : editor_line_start(editor, line);
            size_t to = line == last ? end : editor_line_end(editor, line);
            result[used++] = (struct selection){ to, from, 0, false, list[i].primary && line == last, STICKY_NONE };
        }
    }
    store_selections(editor, result, used);
    free(result);
    free(list);
}

void editor_go_to_bracket(struct editor *editor) {
    long match = editor_matching_bracket(editor, editor->cursor);
    if (match >= 0) editor_set_cursor(editor, (size_t)match, false);
}

void editor_select_between_brackets(struct editor *editor) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    int depth = 0;
    for (size_t i = start; i-- > 0;) {
        char c = editor->text[i];
        if (strchr(CLOSING_BRACKETS, c) && c) depth++;
        else if (strchr(OPENING_BRACKETS, c) && c && depth-- == 0) {
            long match = editor_matching_bracket(editor, i);
            if (match < 0 || (size_t)match < end) continue;
            // A second press selects the brackets too.
            bool inner_selected = start == i + 1 && end == (size_t)match;
            editor_set_cursor(editor, inner_selected ? i : i + 1, false);
            editor_set_cursor(editor, inner_selected ? (size_t)match + 1 : (size_t)match, true);
            return;
        }
    }
}

void editor_go_to_line(struct editor *editor, size_t line) {
    editor_collapse(editor);
    if (line >= editor->line_count) line = editor->line_count - 1;
    size_t start = editor_line_start(editor, line);
    size_t length;
    const char *text = line_text(editor, line, &length);
    editor_set_cursor(editor, start + leading_whitespace(text, length), false);
}

// ---------------------------------------------------------------- insert and delete

struct text_arguments {
    const char *text;
    size_t length;
};

static void replace_selection(struct editor *editor, const char *text, size_t length) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    replace(editor, start, end, text, length);
    editor->cursor = editor->anchor = start + length;
    editor->has_goal_column = false;
}

static void insert_one(struct editor *editor, void *data) {
    const struct text_arguments *arguments = data;
    replace_selection(editor, arguments->text, arguments->length);
}

void editor_insert(struct editor *editor, const char *text, size_t length, enum edit_kind kind) {
    begin_edit(editor, kind);
    struct text_arguments arguments = { text, length };
    for_each_selection(editor, insert_one, &arguments);
}

static size_t line_indent(const struct editor *editor, size_t line) {
    size_t length;
    const char *text = line_text(editor, line, &length);
    return leading_whitespace(text, length);
}

// Re-indents the cursor line like the line of the bracket that `closer_offset` closes.
static void outdent_closer(struct editor *editor, size_t closer_offset) {
    size_t line = editor_line_of(editor, closer_offset), start = editor_line_start(editor, line);
    if (!is_blank(editor->text + start, closer_offset - start)) return;
    long opener = editor_matching_bracket(editor, closer_offset);
    if (opener < 0) return;
    size_t opener_line = editor_line_of(editor, (size_t)opener);
    if (opener_line == line) return;
    size_t indent = line_indent(editor, opener_line), current = closer_offset - start;
    replace(editor, start, start + current, editor->text + editor_line_start(editor, opener_line), indent);
    editor->cursor = editor->anchor = editor->cursor - current + indent;
}

static void type_one(struct editor *editor, void *data) {
    const struct text_arguments *arguments = data;
    char c = arguments->length == 1 ? arguments->text[0] : '\0';
    size_t start, end;
    editor_selection(editor, &start, &end);
    bool selected = start != end;
    char next = char_at(editor, end), previous = start > 0 ? editor->text[start - 1] : '\0';
    bool closing = c && (strchr(CLOSING_BRACKETS, c) || strchr(QUOTES, c));
    if (c && closing && !selected && next == c && !(strchr(QUOTES, c) && is_identifier_char((unsigned char)previous))) {
        editor->cursor = editor->anchor = end + 1; // type over the closing partner
        return;
    }
    bool opening = c && strchr(OPENING_BRACKETS, c);
    bool quote = c && strchr(QUOTES, c);
    if ((opening || quote) && selected) {
        char partner = closing_partner(c);
        char *wrapped = malloc(end - start + 2);
        wrapped[0] = c;
        memcpy(wrapped + 1, editor->text + start, end - start);
        wrapped[end - start + 1] = partner;
        replace(editor, start, end, wrapped, end - start + 2);
        free(wrapped);
        editor->anchor = start + 1;
        editor->cursor = end + 1;
        return;
    }
    bool may_close = next == '\0' || strchr(AUTO_CLOSE_BEFORE, next);
    if (quote && (is_identifier_char((unsigned char)previous) || previous == c)) may_close = false;
    if ((opening || quote) && may_close) {
        char pair[2] = { c, closing_partner(c) };
        replace(editor, start, end, pair, 2);
        editor->cursor = editor->anchor = start + 1;
        return;
    }
    replace_selection(editor, arguments->text, arguments->length);
    if (c && strchr(CLOSING_BRACKETS, c)) outdent_closer(editor, editor->cursor - 1);
}

void editor_type(struct editor *editor, const char *text, size_t length) {
    begin_edit(editor, EDIT_TYPING);
    struct text_arguments arguments = { text, length };
    for_each_selection(editor, type_one, &arguments);
}

// The first entry after the innermost ( or [ of the line before `end` that is still open and has content after it
// on the line, or NO_POSITION. Brackets in strings do not count.
static size_t open_list_entry(const struct editor *editor, size_t line_start, size_t end) {
    size_t openers[MAX_NESTING], depth = 0;
    char quote = 0;
    for (size_t i = line_start; i < end; i++) {
        char c = editor->text[i];
        if (quote) {
            if (c == '\\') i++;
            else if (c == quote) quote = 0;
        } else if (strchr(QUOTES, c)) {
            quote = c;
        } else if (c == '(' || c == '[') {
            if (depth < MAX_NESTING) openers[depth] = i;
            depth++;
        } else if ((c == ')' || c == ']') && depth > 0) {
            depth--;
        }
    }
    if (depth == 0 || depth > MAX_NESTING) return NO_POSITION;
    size_t entry = openers[depth - 1] + 1;
    while (entry < end && (editor->text[entry] == ' ' || editor->text[entry] == '\t')) entry++;
    return entry < end ? entry : NO_POSITION;
}

// Enter keeps the indentation of the line; inside an open ( or [ with entries after it, the new line starts at the
// column of the first entry, so that entries line up; directly after an opening bracket it indents one level, and
// a closing bracket after the cursor moves to its own line (as in VS Code).
static void newline_one(struct editor *editor, void *data) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t line_start = editor_line_start(editor, editor_line_of(editor, start));
    size_t indent = leading_whitespace(editor->text + line_start, start - line_start);
    size_t before = start;
    while (before > line_start && editor->text[before - 1] == ' ') before--;
    char previous = before > line_start ? editor->text[before - 1] : '\0';
    char next = char_at(editor, end);
    bool after_opener = previous && strchr(OPENING_BRACKETS, previous);
    bool between_pair = after_opener && next == closing_partner(previous);
    size_t entry = after_opener ? NO_POSITION : open_list_entry(editor, line_start, start);
    size_t entry_columns = entry != NO_POSITION ? editor_columns_between(editor, line_start, entry) : 0;
    char *text = malloc(2 * indent + entry_columns + INDENT_WIDTH + 3);
    size_t used = 0;
    text[used++] = '\n';
    if (entry != NO_POSITION) {
        memset(text + used, ' ', entry_columns);
        used += entry_columns;
    } else {
        memcpy(text + used, editor->text + line_start, indent);
        used += indent;
    }
    if (after_opener) {
        memcpy(text + used, INDENT, INDENT_WIDTH);
        used += INDENT_WIDTH;
    }
    size_t cursor_at = used;
    if (between_pair) {
        text[used++] = '\n';
        memcpy(text + used, editor->text + line_start, indent);
        used += indent;
    }
    replace(editor, start, end, text, used);
    editor->cursor = editor->anchor = start + cursor_at;
    editor->has_goal_column = false;
    free(text);
}

void editor_insert_newline(struct editor *editor) {
    begin_edit(editor, EDIT_OTHER);
    for_each_selection(editor, newline_one, NULL);
}

static void backspace_one(struct editor *editor, void *data) {
    if (editor_has_selection(editor)) return replace_selection(editor, "", 0);
    if (editor->cursor == 0) return;
    // smartBackspace of the sublime keymap: in the indentation, delete back to the previous indent stop.
    size_t line_start = editor_line_start(editor, editor_line_of(editor, editor->cursor));
    if (editor->cursor > line_start && is_blank(editor->text + line_start, editor->cursor - line_start) &&
        editor->text[editor->cursor - 1] == ' ') {
        size_t spaces = editor->cursor - line_start, remove = spaces % INDENT_WIDTH ? spaces % INDENT_WIDTH : INDENT_WIDTH;
        size_t from = editor->cursor;
        while (remove-- > 0 && from > line_start && editor->text[from - 1] == ' ') from--;
        replace(editor, from, editor->cursor, "", 0);
        editor->cursor = editor->anchor = from;
        return;
    }
    size_t start = cursor_step(editor, editor->cursor, -1);
    char previous = editor->text[start], next = char_at(editor, editor->cursor);
    // An empty pair of brackets or quotes goes away as a whole.
    size_t end = previous && (strchr(OPENING_BRACKETS, previous) || strchr(QUOTES, previous)) &&
                         next == closing_partner(previous)
                     ? editor->cursor + 1
                     : editor->cursor;
    replace(editor, start, end, "", 0);
    editor->cursor = editor->anchor = start;
}

void editor_backspace(struct editor *editor) {
    begin_edit(editor, any_selection(editor) ? EDIT_OTHER : EDIT_DELETING);
    for_each_selection(editor, backspace_one, NULL);
}

static void delete_forward_one(struct editor *editor, void *data) {
    if (editor_has_selection(editor)) return replace_selection(editor, "", 0);
    replace(editor, editor->cursor, cursor_step(editor, editor->cursor, 1), "", 0);
}

void editor_delete_forward(struct editor *editor) {
    begin_edit(editor, any_selection(editor) ? EDIT_OTHER : EDIT_DELETING);
    for_each_selection(editor, delete_forward_one, NULL);
}

static void delete_word_one(struct editor *editor, void *data) {
    int direction = *(const int *)data;
    if (!editor_has_selection(editor)) editor->anchor = group_position(editor, editor->cursor, direction);
    replace_selection(editor, "", 0);
}

void editor_delete_word(struct editor *editor, int direction) {
    begin_edit(editor, EDIT_OTHER);
    for_each_selection(editor, delete_word_one, &direction);
}

void editor_delete_selection(struct editor *editor) {
    if (!any_selection(editor)) return;
    begin_edit(editor, EDIT_OTHER);
    struct text_arguments arguments = { "", 0 };
    for_each_selection(editor, insert_one, &arguments);
}

static void transpose_one(struct editor *editor, void *data) {
    if (editor_has_selection(editor) || editor->cursor == 0) return;
    size_t line = editor_line_of(editor, editor->cursor);
    size_t middle = editor->cursor == editor_line_end(editor, line) ? previous_char(editor, editor->cursor) : editor->cursor;
    size_t start = previous_char(editor, middle), end = next_char(editor, middle);
    if (start >= middle || middle >= end || start < editor_line_start(editor, line)) return;
    char swapped[16];
    size_t first = middle - start, second = end - middle;
    if (first + second > sizeof swapped) return;
    memcpy(swapped, editor->text + middle, second);
    memcpy(swapped + second, editor->text + start, first);
    replace(editor, start, end, swapped, first + second);
    editor->cursor = editor->anchor = end;
}

void editor_transpose(struct editor *editor) {
    begin_edit(editor, EDIT_OTHER);
    for_each_selection(editor, transpose_one, NULL);
}

// ---------------------------------------------------------------- line commands

// Marks the lines that any selection touches; returns the first and the last of them.
static bool *touched_lines(const struct editor *editor, size_t *first, size_t *last) {
    struct selection *list;
    size_t count = editor_selections(editor, &list);
    bool *touched = calloc(editor->line_count, sizeof *touched);
    *first = (size_t)-1;
    *last = 0;
    for (size_t i = 0; i < count; i++) {
        size_t start = selection_start(&list[i]), end = selection_end(&list[i]);
        size_t from = editor_line_of(editor, start), to = editor_line_of(editor, end);
        if (to > from && end == editor_line_start(editor, to)) to--;
        for (size_t line = from; line <= to; line++) touched[line] = true;
        if (from < *first) *first = from;
        if (to > *last) *last = to;
    }
    free(list);
    return touched;
}

// One change inside a line: `removed` bytes at byte column `at` become `insert`; `trim_end` also removes the
// spaces and tabs at the end of the line.
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

// Applies one change per line for the lines first..last and keeps every selection on its text.
static void change_lines(struct editor *editor, size_t first, size_t last, const struct line_change *changes) {
    size_t count = last - first + 1;
    size_t region_start = editor_line_start(editor, first), region_end = editor_line_end(editor, last);
    size_t capacity = region_end - region_start + 1;
    for (size_t i = 0; i < count; i++) capacity += changes[i].inserted;
    char *text = malloc(capacity);
    size_t *new_starts = malloc(count * sizeof *new_starts), *new_lengths = malloc(count * sizeof *new_lengths);
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
    long delta = (long)used - (long)(region_end - region_start);
    struct selection *list;
    size_t selection_count = editor_selections(editor, &list);
    for (size_t i = 0; i < selection_count; i++) {
        size_t *offsets[2] = { &list[i].cursor, &list[i].anchor };
        for (int k = 0; k < 2; k++) {
            size_t line = editor_line_of(editor, *offsets[k]);
            if (line > last) *offsets[k] = (size_t)((long)*offsets[k] + delta);
            else if (line >= first) {
                size_t column = map_column(*offsets[k] - editor_line_start(editor, line), &changes[line - first]);
                *offsets[k] = new_starts[line - first] + (column < new_lengths[line - first] ? column : new_lengths[line - first]);
            }
        }
    }
    begin_edit(editor, EDIT_OTHER);
    replace(editor, region_start, region_end, text, used);
    store_selections(editor, list, selection_count);
    free(list);
    free(text);
    free(new_starts);
    free(new_lengths);
}

// ---------------------------------------------------------------- format

#define MAX_FORMAT_DEPTH 256

struct scan_state {
    size_t depth;
    size_t levels[MAX_FORMAT_DEPTH]; // the indent level of the line of each open bracket
    char string_quote; // 0, '\'', '"' or '`'
    bool block_comment;
};

// Updates the open brackets over one line at indent `level`, outside of strings and comments.
static void scan_line(struct scan_state *state, const char *text, size_t length, size_t level) {
    for (size_t i = 0; i < length; i++) {
        char c = text[i], next = i + 1 < length ? text[i + 1] : '\0';
        if (state->block_comment) {
            if (c == '*' && next == '/') {
                state->block_comment = false;
                i++;
            }
        } else if (state->string_quote) {
            if (c == '\\') i++;
            else if (c == state->string_quote) state->string_quote = 0;
        } else if (c == '/' && next == '/') {
            return;
        } else if (c == '/' && next == '*') {
            state->block_comment = true;
            i++;
        } else if (c == '\'' || c == '"' || c == '`') {
            state->string_quote = c;
        } else if (c == '(' || c == '[' || c == '{') {
            if (state->depth < MAX_FORMAT_DEPTH) state->levels[state->depth] = level;
            state->depth++;
        } else if ((c == ')' || c == ']' || c == '}') && state->depth > 0) {
            state->depth--;
        }
    }
    if (state->string_quote != '`') state->string_quote = 0; // only template strings span lines
}

static size_t opener_level(const struct scan_state *state, size_t index) {
    return state->levels[index < MAX_FORMAT_DEPTH ? index : MAX_FORMAT_DEPTH - 1];
}

void editor_format(struct editor *editor) {
    static const char tabs[] = "\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t";
    size_t count = editor->line_count;
    struct line_change *changes = calloc(count, sizeof *changes);
    struct scan_state *state = calloc(1, sizeof *state);
    for (size_t line = 0; line < count; line++) {
        size_t length;
        const char *text = line_text(editor, line, &length);
        size_t indent = leading_whitespace(text, length), level = 0;
        if (!state->string_quote && !state->block_comment) {
            // A line inside brackets goes one level deeper than the line of the innermost open bracket; a line that
            // starts with closing brackets goes to the level of the line that opened the outermost of them.
            size_t closers = 0;
            for (size_t i = indent; i < length && strchr(")]}", text[i]) && closers < state->depth; i++) closers++;
            if (closers) level = opener_level(state, state->depth - closers);
            else if (state->depth) level = opener_level(state, state->depth - 1) + 1;
            if (indent < length && text[indent] == '.') level++;
            changes[line] = (struct line_change){ .removed = indent, .insert = tabs, .trim_end = true };
            if (indent < length) changes[line].inserted = level < sizeof tabs - 1 ? level : sizeof tabs - 1;
        } else if (state->depth) {
            level = opener_level(state, state->depth - 1) + 1; // the text stays; brackets after the string count from here
        }
        scan_line(state, text, length, level);
    }
    free(state);
    change_lines(editor, 0, count - 1, changes);
    free(changes);
}

void editor_toggle_comment(struct editor *editor) {
    size_t first, last;
    bool *touched = touched_lines(editor, &first, &last);
    size_t minimum_indent = (size_t)-1;
    bool all_commented = true, any_code = false;
    for (size_t line = first; line <= last; line++) {
        size_t length;
        const char *text = line_text(editor, line, &length);
        if (!touched[line] || is_blank(text, length)) continue;
        any_code = true;
        size_t indent = leading_whitespace(text, length);
        if (indent < minimum_indent) minimum_indent = indent;
        if (length - indent < 2 || strncmp(text + indent, COMMENT_TOKEN, 2) != 0) all_commented = false;
    }
    if (any_code) {
        struct line_change *changes = calloc(last - first + 1, sizeof *changes);
        for (size_t line = first; line <= last; line++) {
            size_t length;
            const char *text = line_text(editor, line, &length);
            if (!touched[line] || is_blank(text, length)) continue;
            if (all_commented) {
                size_t at = leading_whitespace(text, length);
                size_t removed = length > at + 2 && text[at + 2] == ' ' ? 3 : 2;
                changes[line - first] = (struct line_change){ .at = at, .removed = removed };
            } else {
                changes[line - first] = (struct line_change){ .at = minimum_indent, .insert = COMMENT_TOKEN " ", .inserted = 3 };
            }
        }
        change_lines(editor, first, last, changes);
        free(changes);
    }
    free(touched);
}

void editor_indent_lines(struct editor *editor, bool outdent) {
    size_t first, last;
    bool *touched = touched_lines(editor, &first, &last);
    struct line_change *changes = calloc(last - first + 1, sizeof *changes);
    for (size_t line = first; line <= last; line++) {
        size_t length;
        const char *text = line_text(editor, line, &length);
        if (!touched[line]) continue;
        if (outdent) {
            size_t indent = leading_whitespace(text, length);
            changes[line - first].removed = indent < INDENT_WIDTH ? indent : INDENT_WIDTH;
        } else if (!is_blank(text, length)) {
            changes[line - first] = (struct line_change){ .insert = INDENT, .inserted = INDENT_WIDTH };
        }
    }
    change_lines(editor, first, last, changes);
    free(changes);
    free(touched);
}

void editor_indent(struct editor *editor, bool outdent) {
    if (outdent || any_selection(editor)) return editor_indent_lines(editor, outdent);
    editor_insert(editor, INDENT, INDENT_WIDTH, EDIT_TYPING);
}

struct line_column {
    size_t line, column;
    bool primary;
};

// Remembers line and byte column of every selection end, to restore them after a line edit shifted by `lines`.
static struct line_column *remember_positions(const struct editor *editor, size_t *count) {
    struct selection *list;
    *count = editor_selections(editor, &list);
    struct line_column *positions = malloc(*count * 2 * sizeof *positions);
    for (size_t i = 0; i < *count; i++) {
        size_t offsets[2] = { list[i].cursor, list[i].anchor };
        for (int k = 0; k < 2; k++) {
            size_t line = editor_line_of(editor, offsets[k]);
            positions[2 * i + k] = (struct line_column){ line, offsets[k] - editor_line_start(editor, line), list[i].primary };
        }
    }
    free(list);
    return positions;
}

static void restore_positions(struct editor *editor, const struct line_column *positions, size_t count, long lines) {
    struct selection *list = calloc(count, sizeof *list);
    for (size_t i = 0; i < count; i++) {
        size_t offsets[2];
        for (int k = 0; k < 2; k++) {
            size_t line = (size_t)((long)positions[2 * i + k].line + lines);
            offsets[k] = editor_line_start(editor, line) + positions[2 * i + k].column;
        }
        list[i] = (struct selection){ offsets[0], offsets[1], 0, false, positions[2 * i].primary, STICKY_NONE };
    }
    store_selections(editor, list, count);
    free(list);
}

void editor_move_lines(struct editor *editor, int direction) {
    size_t first, last;
    free(touched_lines(editor, &first, &last));
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
    size_t count;
    struct line_column *positions = remember_positions(editor, &count);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, region_start, region_start + block_length + other_length + 1, text, block_length + other_length + 1);
    restore_positions(editor, positions, count, direction);
    free(positions);
    free(text);
}

void editor_duplicate_lines(struct editor *editor) {
    size_t first, last;
    free(touched_lines(editor, &first, &last));
    size_t block_start = editor_line_start(editor, first), block_end = editor_line_end(editor, last);
    size_t block_length = block_end - block_start;
    char *text = malloc(block_length + 1);
    text[0] = '\n';
    memcpy(text + 1, editor->text + block_start, block_length);
    size_t count;
    struct line_column *positions = remember_positions(editor, &count);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, block_end, block_end, text, block_length + 1);
    restore_positions(editor, positions, count, (long)(last - first + 1));
    free(positions);
    free(text);
}

void editor_delete_lines(struct editor *editor) {
    size_t first, last;
    free(touched_lines(editor, &first, &last));
    size_t start = editor_line_start(editor, first);
    size_t end = last + 1 < editor->line_count ? editor_line_start(editor, last + 1) : editor->length;
    if (end == editor->length && start > 0) start--; // the last line takes the newline before it
    begin_edit(editor, EDIT_OTHER);
    replace(editor, start, end, "", 0);
    editor_collapse(editor);
    size_t line = editor_line_of(editor, start < editor->length && start > 0 && editor->text[start] == '\n' ? start + 1 : start);
    editor_set_cursor(editor, editor_line_start(editor, line), false);
}

static void join_one(struct editor *editor, void *data) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t first = editor_line_of(editor, start), last = editor_line_of(editor, end);
    if (last == first) last = first + 1;
    if (last >= editor->line_count) return;
    size_t cursor = 0;
    for (size_t joins = last - first; joins > 0; joins--) {
        size_t newline = editor_line_end(editor, first);
        size_t next_start = newline + 1, next_end = editor_line_end(editor, first + 1);
        size_t indent = leading_whitespace(editor->text + next_start, next_end - next_start);
        bool space = newline > editor_line_start(editor, first) && next_start + indent < next_end;
        replace(editor, newline, next_start + indent, " ", space ? 1 : 0);
        cursor = newline;
    }
    editor->cursor = editor->anchor = cursor;
}

void editor_join_lines(struct editor *editor) {
    begin_edit(editor, EDIT_OTHER);
    for_each_selection(editor, join_one, NULL);
}

static int compare_strings(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int compare_strings_insensitive(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

void editor_sort_lines(struct editor *editor, bool reverse, bool insensitive) {
    size_t first, last;
    free(touched_lines(editor, &first, &last));
    if (last == first) return;
    size_t count = last - first + 1, start = editor_line_start(editor, first), end = editor_line_end(editor, last);
    char **lines = malloc(count * sizeof *lines);
    for (size_t i = 0; i < count; i++) {
        size_t length;
        const char *text = line_text(editor, first + i, &length);
        lines[i] = strndup(text, length);
    }
    qsort(lines, count, sizeof *lines, insensitive ? compare_strings_insensitive : compare_strings);
    char *text = malloc(end - start + 1);
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        char *line = lines[reverse ? count - 1 - i : i];
        size_t length = strlen(line);
        memcpy(text + used, line, length);
        used += length;
        if (i + 1 < count) text[used++] = '\n';
        free(line);
    }
    begin_edit(editor, EDIT_OTHER);
    replace(editor, start, end, text, used);
    editor_collapse(editor);
    editor->anchor = start;
    editor->cursor = start + used;
    free(text);
    free(lines);
}

// ---------------------------------------------------------------- evaluation ranges

void editor_line_range(const struct editor *editor, size_t *start, size_t *end) {
    if (editor_has_selection(editor)) return editor_selection(editor, start, end);
    size_t line = editor_line_of(editor, editor->cursor);
    *start = editor_line_start(editor, line);
    *end = editor_line_end(editor, line);
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

// ---------------------------------------------------------------- brackets, search and replace

long editor_matching_bracket(const struct editor *editor, size_t offset) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) {
            if (offset == 0) return -1;
            offset--;
        }
        if (offset >= editor->length) continue;
        char c = editor->text[offset];
        const char *open = strchr(OPENING_BRACKETS, c), *close = strchr(CLOSING_BRACKETS, c);
        if (!c || (!open && !close)) continue;
        char self = c, other = open ? CLOSING_BRACKETS[open - OPENING_BRACKETS] : OPENING_BRACKETS[close - CLOSING_BRACKETS];
        int step = open ? 1 : -1, depth = 0;
        for (long i = (long)offset; i >= 0 && i < (long)editor->length; i += step) {
            if (editor->text[i] == self) depth++;
            else if (editor->text[i] == other && --depth == 0) return i;
        }
        return -1;
    }
    return -1;
}

// The base letters of U+00C0 to U+017F, or a space where the letter has no accent to drop.
static const char latin_base_letters[] =
    "aaaaaa ceeeeiiii nooooo ouuuuy  aaaaaa ceeeeiiii nooooo ouuuuy yaaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiii"
    "iiii  jjkk llllllllllnnnnnn   oooooo  rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzz ";

// U+3041 to U+30FF
static const uint16_t kana_base[] = {
    0x3042, 0x3042, 0x3044, 0x3044, 0x3046, 0x3046, 0x3048, 0x3048, 0x304A, 0x304A, 0x304B, 0x304B,
    0x304D, 0x304D, 0x304F, 0x304F, 0x3051, 0x3051, 0x3053, 0x3053, 0x3055, 0x3055, 0x3057, 0x3057,
    0x3059, 0x3059, 0x305B, 0x305B, 0x305D, 0x305D, 0x305F, 0x305F, 0x3061, 0x3061, 0x3064, 0x3064,
    0x3064, 0x3066, 0x3066, 0x3068, 0x3068, 0x306A, 0x306B, 0x306C, 0x306D, 0x306E, 0x306F, 0x306F,
    0x306F, 0x3072, 0x3072, 0x3072, 0x3075, 0x3075, 0x3075, 0x3078, 0x3078, 0x3078, 0x307B, 0x307B,
    0x307B, 0x307E, 0x307F, 0x3080, 0x3081, 0x3082, 0x3084, 0x3084, 0x3086, 0x3086, 0x3088, 0x3088,
    0x3089, 0x308A, 0x308B, 0x308C, 0x308D, 0x308F, 0x308F, 0x3090, 0x3091, 0x3092, 0x3093, 0x3046,
    0x304B, 0x3051, 0x3097, 0x3098, 0x3099, 0x309A, 0x0020, 0x0020, 0x309D, 0x309D, 0x309F, 0x30A0,
    0x3042, 0x3042, 0x3044, 0x3044, 0x3046, 0x3046, 0x3048, 0x3048, 0x304A, 0x304A, 0x304B, 0x304B,
    0x304D, 0x304D, 0x304F, 0x304F, 0x3051, 0x3051, 0x3053, 0x3053, 0x3055, 0x3055, 0x3057, 0x3057,
    0x3059, 0x3059, 0x305B, 0x305B, 0x305D, 0x305D, 0x305F, 0x305F, 0x3061, 0x3061, 0x3064, 0x3064,
    0x3064, 0x3066, 0x3066, 0x3068, 0x3068, 0x306A, 0x306B, 0x306C, 0x306D, 0x306E, 0x306F, 0x306F,
    0x306F, 0x3072, 0x3072, 0x3072, 0x3075, 0x3075, 0x3075, 0x3078, 0x3078, 0x3078, 0x307B, 0x307B,
    0x307B, 0x307E, 0x307F, 0x3080, 0x3081, 0x3082, 0x3084, 0x3084, 0x3086, 0x3086, 0x3088, 0x3088,
    0x3089, 0x308A, 0x308B, 0x308C, 0x308D, 0x308F, 0x308F, 0x3090, 0x3091, 0x3092, 0x3093, 0x3046,
    0x304B, 0x3051, 0x308F, 0x3090, 0x3091, 0x3092, 0x30FB, 0x30FC, 0x30FD, 0x30FD, 0x30FF,
};
// U+FF66 to U+FF9F
static const uint16_t halfwidth_kana_base[] = {
    0x3092, 0x3042, 0x3044, 0x3046, 0x3048, 0x304A, 0x3084, 0x3086, 0x3088, 0x3064, 0x30FC, 0x3042,
    0x3044, 0x3046, 0x3048, 0x304A, 0x304B, 0x304D, 0x304F, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059,
    0x305B, 0x305D, 0x305F, 0x3061, 0x3064, 0x3066, 0x3068, 0x306A, 0x306B, 0x306C, 0x306D, 0x306E,
    0x306F, 0x3072, 0x3075, 0x3078, 0x307B, 0x307E, 0x307F, 0x3080, 0x3081, 0x3082, 0x3084, 0x3086,
    0x3088, 0x3089, 0x308A, 0x308B, 0x308C, 0x308D, 0x308F, 0x3093, 0xFF9E, 0xFF9F,
};

// Folds a code point as the find bar of Chrome compares it (ICU collation at primary strength): without case,
// accents, the size and voicing of kana, hiragana against katakana and full width. Some letters fold to two or
// three (ß to ss, æ to ae, ﬃ to ffi). Returns the count of folded code points in `out`.
static int fold(int codepoint, int out[3]) {
    static const struct { int codepoint; const char *folded; } expansions[] = {
        { 0xDF, "ss" }, { 0x1E9E, "ss" }, { 0xC6, "ae" }, { 0xE6, "ae" }, { 0x152, "oe" }, { 0x153, "oe" },
        { 0x132, "ij" }, { 0x133, "ij" }, { 0xFB00, "ff" }, { 0xFB01, "fi" }, { 0xFB02, "fl" }, { 0xFB03, "ffi" },
        { 0xFB04, "ffl" }, { 0xFB05, "st" }, { 0xFB06, "st" },
    };
    for (size_t i = 0; i < sizeof expansions / sizeof expansions[0]; i++) {
        if (expansions[i].codepoint != codepoint) continue;
        int count = 0;
        for (const char *c = expansions[i].folded; *c; c++) out[count++] = *c;
        return count;
    }
    int folded = codepoint;
    if (codepoint >= 0xFF01 && codepoint <= 0xFF5E) folded = codepoint - 0xFEE0; // full-width ASCII
    else if (codepoint >= 0x3041 && codepoint <= 0x30FF) folded = kana_base[codepoint - 0x3041];
    else if (codepoint >= 0xFF66 && codepoint <= 0xFF9F) folded = halfwidth_kana_base[codepoint - 0xFF66];
    if (folded < 0x80) folded = tolower(folded);
    else if (folded >= 0xC0 && folded < 0x180) {
        char base = latin_base_letters[folded - 0xC0];
        if (base != ' ') folded = base;
        else if (folded < 0xDF && folded != 0xD7) folded += 0x20; // Ð, Þ
        else if (folded == 0x14A) folded = 0x14B; // Ŋ
    } else if ((folded >= 0x391 && folded <= 0x3AB) || (folded >= 0x410 && folded <= 0x42F)) {
        folded += 0x20;
    } else if (folded >= 0x400 && folded <= 0x40F) {
        folded += 0x50;
    }
    out[0] = folded;
    return 1;
}

// Combining accents and the voicing marks of kana, which primary strength ignores.
static bool is_combining_mark(int codepoint) {
    return (codepoint >= 0x300 && codepoint <= 0x36F) || codepoint == 0x3099 || codepoint == 0x309A || codepoint == 0xFF9E ||
           codepoint == 0xFF9F;
}

// The folded code points of the code point at `*position` (none for a mark); advances `*position`.
static int fold_at(const unsigned char *text, size_t length, size_t *position, int out[3]) {
    size_t size;
    int codepoint = unicode_decode(text + *position, length - *position, &size);
    *position += size;
    return is_combining_mark(codepoint) ? 0 : fold(codepoint, out);
}

// The length in bytes of the match of `needle` at `at`, or 0. Without case, marks after a letter belong to the
// match, and a match does not end inside a letter that folds to more than one (ß against "s").
static size_t match_at(const struct editor *editor, size_t at, const char *needle, size_t length, bool ignore_case) {
    if (!ignore_case) return at + length <= editor->length && memcmp(editor->text + at, needle, length) == 0 ? length : 0;
    const unsigned char *text = (const unsigned char *)editor->text;
    size_t position = at, index = 0, size;
    if ((text[at] & 0xC0) == 0x80 || is_combining_mark(unicode_decode(text + at, editor->length - at, &size))) return 0;
    int wanted[3], found[3], wanted_count = 0, found_count = 0, wanted_index = 0, found_index = 0;
    for (;;) {
        if (wanted_index == wanted_count) {
            if (index >= length) break;
            wanted_count = fold_at((const unsigned char *)needle, length, &index, wanted);
            wanted_index = 0;
            continue;
        }
        if (found_index == found_count) {
            if (position >= editor->length) return 0;
            found_count = fold_at(text, editor->length, &position, found);
            found_index = 0;
            continue;
        }
        if (wanted[wanted_index++] != found[found_index++]) return 0;
    }
    if (found_index != found_count || position == at) return 0;
    while (position < editor->length && is_combining_mark(unicode_decode(text + position, editor->length - position, &size)))
        position += size;
    return position - at;
}

// The start of the first match from `from` on (or before `from` for a negative direction), around the end.
static long find_from(const struct editor *editor, const char *needle, size_t length, bool ignore_case, size_t from,
                      int direction, size_t *match_length) {
    if (length == 0 || editor->length == 0) return -1;
    size_t positions = editor->length;
    for (size_t step = 0; step < positions; step++) {
        size_t i = direction > 0 ? (from + step) % positions : (from + positions - 1 - step % positions) % positions;
        if ((*match_length = match_at(editor, i, needle, length, ignore_case))) return (long)i;
    }
    return -1;
}

bool editor_find(struct editor *editor, const char *needle, size_t length, bool ignore_case, int direction) {
    size_t start, end, match_length;
    editor_selection(editor, &start, &end);
    long match = find_from(editor, needle, length, ignore_case, direction > 0 ? end : start, direction, &match_length);
    if (match < 0) return false;
    editor_collapse(editor);
    editor_set_cursor(editor, (size_t)match, false);
    editor_set_cursor(editor, (size_t)match + match_length, true);
    return true;
}

size_t editor_find_all(const struct editor *editor, const char *needle, size_t length, bool ignore_case,
                       struct editor_range **matches) {
    size_t count = 0, capacity = 0;
    *matches = NULL;
    for (size_t i = 0; length && i < editor->length;) {
        size_t match_length = match_at(editor, i, needle, length, ignore_case);
        if (!match_length) {
            i++;
            continue;
        }
        if (count == capacity) *matches = realloc(*matches, (capacity = capacity ? capacity * 2 : 16) * sizeof **matches);
        (*matches)[count++] = (struct editor_range){ i, i + match_length };
        i += match_length;
    }
    return count;
}

static bool is_selected_range(const struct editor *editor, size_t start, size_t end) {
    struct selection *list;
    size_t count = editor_selections(editor, &list);
    bool found = false;
    for (size_t i = 0; i < count && !found; i++)
        found = selection_start(&list[i]) == start && selection_end(&list[i]) == end;
    free(list);
    return found;
}

void editor_select_next_occurrence(struct editor *editor) {
    if (!editor_has_selection(editor)) return editor_select_word_at(editor, editor->cursor);
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t length = end - start;
    char *needle = strndup(editor->text + start, length);
    struct selection *list;
    size_t count = editor_selections(editor, &list);
    size_t from = selection_end(&list[count - 1]);
    free(list);
    size_t match_length;
    long match = find_from(editor, needle, length, false, from, 1, &match_length);
    for (size_t tries = 0; match >= 0 && tries < editor->length; tries++) {
        if (!is_selected_range(editor, (size_t)match, (size_t)match + length)) {
            add_selection(editor, (size_t)match, (size_t)match + length);
            break;
        }
        match = find_from(editor, needle, length, false, (size_t)match + 1, 1, &match_length);
        if (match >= 0 && (size_t)match == start) break; // every match is selected
    }
    free(needle);
}

size_t editor_replace_all(struct editor *editor, const char *needle, size_t needle_length, const char *replacement,
                          size_t replacement_length, bool ignore_case) {
    if (needle_length == 0) return 0;
    size_t count = 0, capacity = editor->length + 1, used = 0;
    char *text = malloc(capacity);
    for (size_t i = 0; i < editor->length;) {
        size_t match_length = match_at(editor, i, needle, needle_length, ignore_case);
        size_t piece = match_length ? replacement_length : 1;
        if (used + piece + 1 > capacity) text = realloc(text, capacity = (used + piece + 1) * 2);
        if (match_length) {
            memcpy(text + used, replacement, replacement_length);
            i += match_length;
            count++;
        } else {
            text[used] = editor->text[i++];
        }
        used += piece;
    }
    if (count) editor_replace_text(editor, text, used);
    free(text);
    return count;
}

bool editor_replace_next(struct editor *editor, const char *needle, size_t needle_length, const char *replacement,
                         size_t replacement_length, bool ignore_case) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    bool replaced = needle_length && end > start && match_at(editor, start, needle, needle_length, ignore_case) == end - start;
    if (replaced) {
        editor_collapse(editor);
        begin_edit(editor, EDIT_OTHER);
        replace(editor, start, end, replacement, replacement_length);
        editor->cursor = editor->anchor = start + replacement_length;
    }
    editor_find(editor, needle, needle_length, ignore_case, 1);
    return replaced;
}

// ---------------------------------------------------------------- words for autocomplete

size_t editor_word_before_cursor(const struct editor *editor, size_t *start) {
    size_t offset = editor->cursor;
    while (offset > 0 && is_identifier_char((unsigned char)editor->text[offset - 1])) offset--;
    *start = offset;
    return editor->cursor - offset;
}

size_t editor_words(const struct editor *editor, const char *prefix, size_t prefix_length, char ***words) {
    size_t count = 0, capacity = 16;
    *words = malloc(capacity * sizeof **words);
    size_t current_start;
    editor_word_before_cursor(editor, &current_start);
    for (size_t i = 0; i < editor->length;) {
        if (!is_identifier_char((unsigned char)editor->text[i]) || isdigit((unsigned char)editor->text[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < editor->length && is_identifier_char((unsigned char)editor->text[i])) i++;
        size_t length = i - start;
        if (start == current_start || length <= prefix_length || strncmp(editor->text + start, prefix, prefix_length) != 0)
            continue;
        bool known = false;
        for (size_t k = 0; k < count && !known; k++) known = strlen((*words)[k]) == length && !strncmp((*words)[k], editor->text + start, length);
        if (known) continue;
        if (count == capacity) *words = realloc(*words, (capacity *= 2) * sizeof **words);
        (*words)[count++] = strndup(editor->text + start, length);
    }
    return count;
}

// ---------------------------------------------------------------- commands of the sublime keymap

static size_t primary_line(const struct editor *editor) { return editor_line_of(editor, editor->cursor); }

void editor_toggle_bookmark(struct editor *editor) {
    size_t line = primary_line(editor), at = 0;
    while (at < editor->bookmark_count && editor->bookmarks[at] < line) at++;
    if (at < editor->bookmark_count && editor->bookmarks[at] == line) {
        memmove(editor->bookmarks + at, editor->bookmarks + at + 1, (editor->bookmark_count - at - 1) * sizeof(size_t));
        editor->bookmark_count--;
        return;
    }
    if (editor->bookmark_count == editor->bookmark_capacity) {
        editor->bookmark_capacity = editor->bookmark_capacity ? editor->bookmark_capacity * 2 : 16;
        editor->bookmarks = realloc(editor->bookmarks, editor->bookmark_capacity * sizeof(size_t));
    }
    memmove(editor->bookmarks + at + 1, editor->bookmarks + at, (editor->bookmark_count - at) * sizeof(size_t));
    editor->bookmarks[at] = line;
    editor->bookmark_count++;
}

void editor_next_bookmark(struct editor *editor, int direction) {
    if (!editor->bookmark_count) return;
    size_t line = primary_line(editor), target = direction > 0 ? editor->bookmarks[0] : editor->bookmarks[editor->bookmark_count - 1];
    for (size_t i = 0; i < editor->bookmark_count; i++) {
        size_t candidate = direction > 0 ? editor->bookmarks[i] : editor->bookmarks[editor->bookmark_count - 1 - i];
        if (direction > 0 ? candidate > line : candidate < line) {
            target = candidate;
            break;
        }
    }
    editor_collapse(editor);
    editor_set_cursor(editor, editor_line_start(editor, target), false);
}

void editor_clear_bookmarks(struct editor *editor) { editor->bookmark_count = 0; }

void editor_select_bookmarks(struct editor *editor) {
    if (!editor->bookmark_count) return;
    struct selection *list = calloc(editor->bookmark_count, sizeof *list);
    for (size_t i = 0; i < editor->bookmark_count; i++) {
        size_t line = editor->bookmarks[i];
        list[i] = (struct selection){ editor_line_end(editor, line), editor_line_start(editor, line), 0, false, i == 0, STICKY_NONE };
    }
    store_selections(editor, list, editor->bookmark_count);
    free(list);
}

// findUnder and findUnderPrevious: the word or the selection, then its next match (one selection).
void editor_find_under(struct editor *editor, int direction) {
    if (!editor_has_selection(editor)) editor_select_word_at(editor, editor->cursor);
    size_t start, end;
    editor_selection(editor, &start, &end);
    if (start == end) return;
    char *needle = strndup(editor->text + start, end - start);
    editor_find(editor, needle, end - start, false, direction);
    free(needle);
}

// findAllUnder: every match of the word or the selection becomes a selection.
void editor_select_all_under(struct editor *editor) {
    if (!editor_has_selection(editor)) editor_select_word_at(editor, editor->cursor);
    size_t start, end;
    editor_selection(editor, &start, &end);
    size_t length = end - start, count = 0, capacity = 16;
    if (!length) return;
    struct selection *list = malloc(capacity * sizeof *list);
    for (size_t i = 0; i + length <= editor->length; i++) {
        if (memcmp(editor->text + i, editor->text + start, length) != 0) continue;
        if (count == capacity) list = realloc(list, (capacity *= 2) * sizeof *list);
        list[count++] = (struct selection){ i + length, i, 0, false, i == start, STICKY_NONE };
        i += length - 1;
    }
    store_selections(editor, list, count);
    free(list);
}

void editor_select_scope(struct editor *editor) {
    size_t start, end;
    editor_selection(editor, &start, &end);
    editor_select_between_brackets(editor);
    size_t new_start, new_end;
    editor_selection(editor, &new_start, &new_end);
    if (new_start == start && new_end == end) editor_select_all(editor);
}

static void case_one(struct editor *editor, void *data) {
    bool upper = *(const bool *)data;
    size_t start, end;
    bool selected = editor_has_selection(editor);
    if (selected) editor_selection(editor, &start, &end);
    else word_range(editor, editor->cursor, &start, &end);
    char *text = strndup(editor->text + start, end - start);
    for (char *c = text; *c; c++) *c = (char)(upper ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    size_t cursor = editor->cursor, anchor = editor->anchor;
    replace(editor, start, end, text, end - start);
    editor->cursor = cursor;
    editor->anchor = anchor;
    free(text);
}

void editor_change_case(struct editor *editor, bool upper) {
    begin_edit(editor, EDIT_OTHER);
    for_each_selection(editor, case_one, &upper);
}

static void delete_line_part_one(struct editor *editor, void *data) {
    int direction = *(const int *)data;
    size_t line = editor_line_of(editor, editor->cursor);
    size_t start = direction < 0 ? editor_line_start(editor, line) : editor->cursor;
    size_t end = direction < 0 ? editor->cursor : editor_line_end(editor, line);
    replace(editor, start, end, "", 0);
    editor->cursor = editor->anchor = start;
}

void editor_delete_line_part(struct editor *editor, int direction) {
    begin_edit(editor, EDIT_OTHER);
    for_each_selection(editor, delete_line_part_one, &direction);
}

void editor_set_mark(struct editor *editor) {
    editor->mark = editor->cursor;
    editor->has_mark = true;
}

void editor_select_to_mark(struct editor *editor) {
    if (!editor->has_mark) return;
    editor_collapse(editor);
    editor->anchor = editor->mark < editor->length ? editor->mark : editor->length;
}

void editor_delete_to_mark(struct editor *editor) {
    if (!editor->has_mark) return;
    size_t mark = editor->mark < editor->length ? editor->mark : editor->length;
    size_t start = mark < editor->cursor ? mark : editor->cursor, end = mark < editor->cursor ? editor->cursor : mark;
    free(editor->killed);
    editor->killed = strndup(editor->text + start, end - start);
    editor_collapse(editor);
    begin_edit(editor, EDIT_OTHER);
    replace(editor, start, end, "", 0);
    editor->cursor = editor->anchor = start;
}

void editor_swap_with_mark(struct editor *editor) {
    if (!editor->has_mark) return;
    size_t mark = editor->mark < editor->length ? editor->mark : editor->length;
    editor->mark = editor->cursor;
    editor_collapse(editor);
    editor_set_cursor(editor, mark, false);
}

void editor_yank(struct editor *editor) {
    if (editor->killed) editor_insert(editor, editor->killed, strlen(editor->killed), EDIT_OTHER);
}

// Re-wraps the paragraphs of the touched lines at `width` columns; a common "// " prefix and the indentation stay.
void editor_hard_wrap(struct editor *editor, size_t width) {
    size_t first, last;
    free(touched_lines(editor, &first, &last));
    if (editor_selection_count(editor) == 1 && !editor_has_selection(editor)) {
        while (first > 0 && !line_is_blank(editor, first - 1)) first--;
        while (last + 1 < editor->line_count && !line_is_blank(editor, last + 1)) last++;
    }
    if (line_is_blank(editor, first)) return;
    size_t length;
    const char *text = line_text(editor, first, &length);
    size_t prefix_length = leading_whitespace(text, length);
    if (length >= prefix_length + 3 && !strncmp(text + prefix_length, "// ", 3)) prefix_length += 3;
    char *prefix = strndup(text, prefix_length);
    size_t start = editor_line_start(editor, first), end = editor_line_end(editor, last);
    size_t capacity = (end - start) * 2 + prefix_length * (last - first + 2) + 16, used = 0, column = 0;
    char *wrapped = malloc(capacity);
    memcpy(wrapped, prefix, prefix_length);
    used = column = prefix_length;
    bool line_has_words = false;
    for (size_t line = first; line <= last; line++) {
        const char *content = line_text(editor, line, &length);
        size_t skip = length >= prefix_length && !strncmp(content, prefix, prefix_length) ? prefix_length : leading_whitespace(content, length);
        for (size_t i = skip; i < length;) {
            while (i < length && content[i] == ' ') i++;
            size_t word = i;
            while (i < length && content[i] != ' ') i++;
            if (i == word) break;
            if (line_has_words && column + 1 + (i - word) > width) {
                wrapped[used++] = '\n';
                memcpy(wrapped + used, prefix, prefix_length);
                used += prefix_length;
                column = prefix_length;
                line_has_words = false;
            }
            if (line_has_words) wrapped[used++] = ' ', column++;
            if (used + (i - word) + prefix_length + 2 > capacity) wrapped = realloc(wrapped, capacity = (capacity + i - word) * 2);
            memcpy(wrapped + used, content + word, i - word);
            used += i - word;
            column += i - word;
            line_has_words = true;
        }
    }
    begin_edit(editor, EDIT_OTHER);
    replace(editor, start, end, wrapped, used);
    editor_collapse(editor);
    editor->cursor = editor->anchor = start + used;
    free(wrapped);
    free(prefix);
}

// skipAndSelectNextOccurrence (Ctrl+K Ctrl+D): the next occurrence replaces the primary selection.
void editor_skip_and_select_next(struct editor *editor) {
    if (!editor_has_selection(editor)) return editor_select_word_at(editor, editor->cursor);
    struct selection old = primary_selection(editor);
    size_t count_before = editor_selection_count(editor);
    editor_select_next_occurrence(editor);
    if (editor_selection_count(editor) == count_before) return;
    struct selection *list;
    size_t count = editor_selections(editor, &list), kept = 0;
    for (size_t i = 0; i < count; i++)
        if (!(selection_start(&list[i]) == selection_start(&old) && selection_end(&list[i]) == selection_end(&old)))
            list[kept++] = list[i];
    store_selections(editor, list, kept);
    free(list);
}
