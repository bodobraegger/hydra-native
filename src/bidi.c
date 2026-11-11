// Ports bidiOrdering of CodeMirror 5.65.9 (MIT, lib/codemirror.js of codemirror-minified) and wraps SheenBidi 2.9.0
// (Apache-2.0) for the levels that the browser draws. See THIRD_PARTY.md.
#include "bidi.h"
#include "unicode.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <SheenBidi/SheenBidi.h>

// The character types of CodeMirror 5 for U+0000 to U+00F7 and for U+0600 to U+06F9.
static const char low_types[] =
    "bbbbbbbbbtstwsbbbbbbbbbbbbbbssstwNN%%%NNNNNN,N,N1111111111NNNNNNNLLLLLLLLLLLLLLLLLLLLLLLLLLNNNNNNLLLLLLLLLLLLL"
    "LLLLLLLLLLLLLNNNNbbbbbbsbbbbbbbbbbbbbbbbbbbbbbbbbb,N%%%%NNNNLNNNNN%%11NLNNN1LNNNNNLLLLLLLLLLLLLLLLLLLLLLLNLLLL"
    "LLLLLLLLLLLLLLLLLLLLLLLLLLLN";
static const char arabic_types[] =
    "nnnnnnNNr%%r,rNNmmmmmmmmmmmrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrmmmmmmmmmmmmmmmmmmmmmnnnnnnnnnn%nnr"
    "rrmrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrmmmmmm"
    "mnNmmmmmmrrmmNmmmmrr1111111111";

static char char_type(int codepoint) {
    if (codepoint <= 0xF7) return low_types[codepoint];
    if (codepoint >= 0x590 && codepoint <= 0x5F4) return 'R';
    if (codepoint >= 0x600 && codepoint <= 0x6F9) return arabic_types[codepoint - 0x600];
    if (codepoint >= 0x6EE && codepoint <= 0x8AC) return 'r';
    if (codepoint >= 0x2000 && codepoint <= 0x200B) return 'w';
    if (codepoint == 0x200C) return 'b';
    return 'L';
}

static bool is_rtl_trigger(int codepoint) {
    return (codepoint >= 0x590 && codepoint <= 0x5F4) || (codepoint >= 0x600 && codepoint <= 0x6FF) ||
           (codepoint >= 0x700 && codepoint <= 0x8AC);
}

// \s of JavaScript.
static bool is_js_space(int codepoint) {
    return codepoint == ' ' || (codepoint >= 0x9 && codepoint <= 0xD) || codepoint == 0xA0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 || codepoint == 0x2029 || codepoint == 0x202F ||
           codepoint == 0x205F || codepoint == 0x3000 || codepoint == 0xFEFF;
}

bool bidi_line_has_rtl(const char *text, size_t length) {
    size_t size;
    for (size_t i = 0; i < length; i += size)
        if (is_rtl_trigger(unicode_decode((const unsigned char *)text + i, length - i, &size))) return true;
    return false;
}

static bool is_strong(char type) { return type == 'L' || type == 'R' || type == 'r'; }
static bool is_neutral(char type) { return type == 's' || type == 't' || type == 'w' || type == 'N'; }
static bool counts_as_left(char type) { return type == 'L' || type == 'b' || type == '1' || type == 'n'; }
static bool counts_as_number(char type) { return type == '1' || type == 'n'; }

static void insert_span(struct bidi_span *spans, size_t *count, size_t capacity, size_t at, struct bidi_span span) {
    if (*count >= capacity) return;
    memmove(spans + at + 1, spans + at, (*count - at) * sizeof *spans);
    spans[at] = span;
    (*count)++;
}

size_t bidi_codemirror_order(const char *text, size_t length, struct bidi_span *spans, size_t capacity) {
    if (length == 0 || !bidi_line_has_rtl(text, length) || capacity < 3) return 0;
    // Types and byte offsets per code point; offsets[count] is the length.
    size_t count = 0, size;
    char *types = malloc(length + 1);
    size_t *offsets = malloc(sizeof(size_t) * (length + 1));
    for (size_t i = 0; i < length; i += size) {
        offsets[count] = i;
        types[count++] = char_type(unicode_decode((const unsigned char *)text + i, length - i, &size));
    }
    offsets[count] = length;
    const char outer = 'L';
    char previous = outer;
    for (size_t i = 0; i < count; i++) { // W1
        if (types[i] == 'm') types[i] = previous;
        else previous = types[i];
    }
    char current = outer;
    for (size_t i = 0; i < count; i++) { // W2, W3
        if (types[i] == '1' && current == 'r') types[i] = 'n';
        else if (is_strong(types[i])) {
            current = types[i];
            if (types[i] == 'r') types[i] = 'R';
        }
    }
    previous = types[0];
    for (size_t i = 1; i + 1 < count; i++) { // W4
        char type = types[i];
        if (type == '+' && previous == '1' && types[i + 1] == '1') types[i] = '1';
        else if (type == ',' && previous == types[i + 1] && (previous == '1' || previous == 'n')) types[i] = previous;
        previous = type;
    }
    for (size_t i = 0; i < count; i++) { // W5, W6
        if (types[i] == ',') {
            types[i] = 'N';
        } else if (types[i] == '%') {
            size_t end = i + 1;
            while (end < count && types[end] == '%') end++;
            char replace = (i && types[i - 1] == '!') || (end < count && types[end] == '1') ? '1' : 'N';
            for (size_t j = i; j < end; j++) types[j] = replace;
            i = end - 1;
        }
    }
    current = outer;
    for (size_t i = 0; i < count; i++) { // W7
        if (current == 'L' && types[i] == '1') types[i] = 'L';
        else if (is_strong(types[i])) current = types[i];
    }
    for (size_t i = 0; i < count; i++) { // N1, N2
        if (!is_neutral(types[i])) continue;
        size_t end = i + 1;
        while (end < count && is_neutral(types[end])) end++;
        bool before = (i ? types[i - 1] : outer) == 'L', after = (end < count ? types[end] : outer) == 'L';
        char replace = before == after ? (before ? 'L' : 'R') : outer;
        for (size_t j = i; j < end; j++) types[j] = replace;
        i = end - 1;
    }
    size_t span_count = 0;
    for (size_t i = 0; i < count;) {
        if (counts_as_left(types[i])) {
            size_t start = i;
            for (++i; i < count && counts_as_left(types[i]); ++i) {}
            insert_span(spans, &span_count, capacity, span_count, (struct bidi_span){ 0, offsets[start], offsets[i] });
            continue;
        }
        size_t position = i, at = span_count; // in a left-to-right document, the spans of an RTL run go in reverse
        for (++i; i < count && types[i] != 'L'; ++i) {}
        for (size_t j = position; j < i;) {
            if (counts_as_number(types[j])) {
                if (position < j) insert_span(spans, &span_count, capacity, at, (struct bidi_span){ 1, offsets[position], offsets[j] });
                size_t number_start = j;
                for (++j; j < i && counts_as_number(types[j]); ++j) {}
                insert_span(spans, &span_count, capacity, at, (struct bidi_span){ 2, offsets[number_start], offsets[j] });
                position = j;
            } else {
                ++j;
            }
        }
        if (position < i) insert_span(spans, &span_count, capacity, at, (struct bidi_span){ 1, offsets[position], offsets[i] });
    }
    // Leading and trailing white space of a line that starts or ends right-to-left stays left-to-right.
    if (span_count && spans[0].level == 1) {
        size_t space = 0;
        while (space < length && is_js_space(unicode_decode((const unsigned char *)text + space, length - space, &size))) space += size;
        if (space) {
            spans[0].from = space;
            insert_span(spans, &span_count, capacity, 0, (struct bidi_span){ 0, 0, space });
        }
    }
    if (span_count && spans[span_count - 1].level == 1) {
        size_t space = length;
        while (space > 0) {
            size_t start = space - 1;
            while (start > 0 && (text[start] & 0xC0) == 0x80) start--;
            if (!is_js_space(unicode_decode((const unsigned char *)text + start, length - start, &size))) break;
            space = start;
        }
        if (space < length) {
            spans[span_count - 1].to = space;
            insert_span(spans, &span_count, capacity, span_count, (struct bidi_span){ 0, space, length });
        }
    }
    free(types);
    free(offsets);
    return span_count;
}

void bidi_visual_order(const unsigned char *levels, size_t count, size_t *order) {
    unsigned char highest = 0, lowest_odd = UCHAR_MAX;
    for (size_t i = 0; i < count; i++) {
        order[i] = i;
        if (levels[i] > highest) highest = levels[i];
        if (levels[i] % 2 && levels[i] < lowest_odd) lowest_odd = levels[i];
    }
    // Reverses the sequences at each level, from the highest to the lowest odd level.
    for (int level = highest; lowest_odd != UCHAR_MAX && level >= lowest_odd; level--) {
        for (size_t i = 0; i < count;) {
            if (levels[order[i]] < level) {
                i++;
                continue;
            }
            size_t end = i;
            while (end < count && levels[order[end]] >= level) end++;
            for (size_t a = i, b = end - 1; a < b; a++, b--) {
                size_t swap = order[a];
                order[a] = order[b];
                order[b] = swap;
            }
            i = end;
        }
    }
}

void bidi_row_levels(const char *text, size_t length, size_t row_start, size_t row_end, unsigned char *levels) {
    memset(levels, 0, length);
    if (!bidi_line_has_rtl(text, length)) return;
    SBCodepointSequence sequence = { SBStringEncodingUTF8, text, (SBUInteger)length };
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, 0, (SBUInteger)length, 0);
    SBUInteger paragraph_length = SBParagraphGetLength(paragraph);
    if (row_end > paragraph_length) row_end = paragraph_length;
    if (row_start < row_end) {
        SBLineRef line = SBParagraphCreateLine(paragraph, (SBUInteger)row_start, (SBUInteger)(row_end - row_start));
        const SBRun *runs = SBLineGetRunsPtr(line);
        for (SBUInteger r = 0; r < SBLineGetRunCount(line); r++)
            memset(levels + runs[r].offset, runs[r].level, runs[r].length);
        SBLineRelease(line);
    }
    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
}
