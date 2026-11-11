// Small UTF-8 helpers that the editor core and the overlay share, so that both count the same columns:
// extended grapheme clusters (UAX #29) and their widths in the monospace grid.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "unicode_tables.h"

// Decodes one code point; invalid bytes count as one byte of U+FFFD.
static inline int unicode_decode(const unsigned char *text, size_t length, size_t *consumed) {
    unsigned char first = text[0];
    int count = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
    if ((size_t)count > length || (first >= 0x80 && first < 0xC0)) {
        *consumed = 1;
        return 0xFFFD;
    }
    int codepoint = count == 1 ? first : first & (0x3F >> (count - 1));
    for (int i = 1; i < count; i++) codepoint = (codepoint << 6) | (text[i] & 0x3F);
    *consumed = (size_t)count;
    return codepoint;
}

// The value of the range that contains `codepoint`, or 0.
static inline int unicode_lookup(const struct unicode_range *ranges, size_t count, int codepoint) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if ((uint32_t)codepoint > ranges[middle].last) low = middle + 1;
        else if ((uint32_t)codepoint < ranges[middle].first) high = middle;
        else return ranges[middle].value;
    }
    return 0;
}

#define UNICODE_LOOKUP(table, codepoint) unicode_lookup(table, sizeof table / sizeof table[0], codepoint)

// Wide (East_Asian_Width W or F) characters take two columns of the monospace grid.
static inline bool unicode_is_wide(int codepoint) { return UNICODE_LOOKUP(wide_ranges, codepoint) != 0; }

// Columns of a single code point: 0 for joiners, marks and variation selectors, 2 for wide ones.
static inline int unicode_columns(int codepoint) {
    if (codepoint == 0x200D || (codepoint >= 0xFE00 && codepoint <= 0xFE0F)) return 0;
    if (UNICODE_LOOKUP(grapheme_break_ranges, codepoint) == 4) return 0; // Extend
    return unicode_is_wide(codepoint) ? 2 : 1;
}

// Grapheme_Cluster_Break, in the order of BREAK_CLASSES of tools/unicode_tables.py.
enum unicode_break {
    BREAK_OTHER, BREAK_CR, BREAK_LF, BREAK_CONTROL, BREAK_EXTEND, BREAK_ZWJ, BREAK_REGIONAL_INDICATOR, BREAK_PREPEND,
    BREAK_SPACING_MARK, BREAK_L, BREAK_V, BREAK_T, BREAK_LV, BREAK_LVT,
};

#define HANGUL_FIRST 0xAC00
#define HANGUL_LAST 0xD7A3
#define HANGUL_T_COUNT 28

static inline enum unicode_break unicode_break_class(int codepoint) {
    if (codepoint >= HANGUL_FIRST && codepoint <= HANGUL_LAST)
        return (codepoint - HANGUL_FIRST) % HANGUL_T_COUNT == 0 ? BREAK_LV : BREAK_LVT;
    return (enum unicode_break)UNICODE_LOOKUP(grapheme_break_ranges, codepoint);
}

// The end of the extended grapheme cluster that starts at `offset`: rules GB3 to GB13 of UAX #29 (ZWJ emoji
// sequences, flags, keycaps, variation selectors, Hangul syllables), without the Indic conjuncts of GB9c.
static inline size_t unicode_cluster_end(const unsigned char *text, size_t length, size_t offset) {
    if (offset >= length) return length;
    // ASCII before ASCII never joins, except CR LF.
    bool ascii_follows = offset + 1 >= length || text[offset + 1] < 0x80;
    if (text[offset] < 0x80 && text[offset] != '\r' && ascii_follows) return offset + 1;
    size_t size, position = offset;
    int codepoint = unicode_decode(text + position, length - position, &size);
    position += size;
    enum unicode_break previous = unicode_break_class(codepoint);
    // GB11: an Extended_Pictographic, Extend*, then ZWJ joins the next Extended_Pictographic.
    enum { EMOJI_NONE, EMOJI_SEEN, EMOJI_JOINED } emoji = EMOJI_NONE;
    if (UNICODE_LOOKUP(extended_pictographic_ranges, codepoint)) emoji = EMOJI_SEEN;
    int regional_indicators = previous == BREAK_REGIONAL_INDICATOR;
    while (position < length) {
        codepoint = unicode_decode(text + position, length - position, &size);
        enum unicode_break next = unicode_break_class(codepoint);
        bool pictographic = UNICODE_LOOKUP(extended_pictographic_ranges, codepoint) != 0;
        bool join;
        if (previous == BREAK_CR && next == BREAK_LF) join = true;
        else if (previous == BREAK_CR || previous == BREAK_LF || previous == BREAK_CONTROL) join = false;
        else if (next == BREAK_CR || next == BREAK_LF || next == BREAK_CONTROL) join = false;
        else if (previous == BREAK_L && (next == BREAK_L || next == BREAK_V || next == BREAK_LV || next == BREAK_LVT))
            join = true;
        else if ((previous == BREAK_LV || previous == BREAK_V) && (next == BREAK_V || next == BREAK_T)) join = true;
        else if ((previous == BREAK_LVT || previous == BREAK_T) && next == BREAK_T) join = true;
        else if (next == BREAK_EXTEND || next == BREAK_ZWJ || next == BREAK_SPACING_MARK || previous == BREAK_PREPEND)
            join = true;
        else if (previous == BREAK_ZWJ && emoji == EMOJI_JOINED && pictographic) join = true;
        else if (previous == BREAK_REGIONAL_INDICATOR && next == BREAK_REGIONAL_INDICATOR)
            join = regional_indicators % 2 == 1;
        else join = false;
        if (!join) break;
        if (pictographic) emoji = EMOJI_SEEN;
        else if (next == BREAK_ZWJ && emoji == EMOJI_SEEN) emoji = EMOJI_JOINED;
        else if (next != BREAK_EXTEND) emoji = EMOJI_NONE;
        regional_indicators = next == BREAK_REGIONAL_INDICATOR ? regional_indicators + 1 : 0;
        previous = next;
        position += size;
    }
    return position;
}

// Columns of the cluster [offset, end) in the grid: two for wide characters, emoji presentation (also by VS16),
// flags and keycaps, none for a lone mark, else one.
static inline int unicode_cluster_columns(const unsigned char *text, size_t offset, size_t end) {
    if (end == offset + 1 && text[offset] < 0x80) return 1;
    size_t size;
    int first = unicode_decode(text + offset, end - offset, &size);
    if (unicode_is_wide(first) || UNICODE_LOOKUP(emoji_presentation_ranges, first)) return 2;
    for (size_t position = offset + size; position < end; position += size) {
        int codepoint = unicode_decode(text + position, end - position, &size);
        if (codepoint == 0xFE0F || codepoint == 0x20E3) return 2; // emoji presentation, keycap
        if (codepoint == 0xFE0E) return 1;                         // text presentation
    }
    return unicode_columns(first);
}

// The cursor of CodeMirror 5 moves over a code point and then over the extending characters after it
// (combining marks, joiners, variation selectors); it stops inside other clusters, such as ZWJ sequences.
static inline bool unicode_is_codemirror_extending(int codepoint) {
    return codepoint >= 0x300 && UNICODE_LOOKUP(codemirror_extending_ranges, codepoint) != 0;
}
