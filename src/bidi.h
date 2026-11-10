// Right-to-left text in code lines, as in the web editor: the browser draws a line with the full Unicode bidi
// algorithm (SheenBidi here), and CodeMirror 5 moves the cursor with its own simpler bidiOrdering.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// A run of one embedding level, as a BidiSpan of CodeMirror 5; offsets are bytes of the line.
struct bidi_span {
    unsigned char level;
    size_t from, to;
};

// true when the line has a character that makes CodeMirror 5 compute an order (Hebrew, Arabic, Syriac, Thaana).
bool bidi_line_has_rtl(const char *text, size_t length);

// bidiOrdering of CodeMirror 5 for a left-to-right document: the spans of the line in visual order, or 0 when the
// line has no right-to-left text. `spans` gets at most `capacity` spans.
size_t bidi_codemirror_order(const char *text, size_t length, struct bidi_span *spans, size_t capacity);

// The levels of the full Unicode bidi algorithm for each byte of the line (paragraph level 0), with rule L1 for
// the row [row_start, row_end) applied; `levels` has `length` entries.
void bidi_row_levels(const char *text, size_t length, size_t row_start, size_t row_end, unsigned char *levels);
