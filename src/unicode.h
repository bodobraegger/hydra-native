// Small UTF-8 helpers that the editor core and the overlay share, so that both count the same columns.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Wide East Asian characters take two columns of the monospace grid.
static inline bool unicode_is_wide(int codepoint) {
    return (codepoint >= 0x1100 && codepoint <= 0x115F) || (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7A3) || (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0xFE30 && codepoint <= 0xFE4F) || (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
           (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) || (codepoint >= 0x20000 && codepoint <= 0x3FFFD);
}

// Columns of a code point: 0 for joiners and variation selectors (as in emoji sequences), 2 for wide ones.
static inline int unicode_columns(int codepoint) {
    if (codepoint == 0x200D || (codepoint >= 0xFE00 && codepoint <= 0xFE0F)) return 0;
    if (unicode_is_wide(codepoint) || (codepoint >= 0x1F300 && codepoint <= 0x1FAFF)) return 2;
    return 1;
}

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
