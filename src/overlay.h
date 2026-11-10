// Draws colored rectangles and monospace text over the hydra output in one draw call per frame.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OVERLAY_MIN_FONT_SIZE 8.0f
#define OVERLAY_MAX_FONT_SIZE 56.0f
#define OVERLAY_TAB_WIDTH 4 // a tab reaches the next multiple of 4 columns

bool overlay_init(const char *font_path, float font_size);
bool overlay_set_font_size(float font_size);
float overlay_font_size(void);
float overlay_char_width(void);
float overlay_line_height(void);

// Colors are 0xRRGGBBAA.
void overlay_begin(void);
void overlay_rect(float x, float y, float width, float height, uint32_t color);
// Draws `length` bytes of UTF-8 from column `first_column` on, at most `max_columns` columns.
// Glyphs render on their first use; characters that the code font lacks come from fontconfig fallback fonts.
// `colors` gives one color per byte, or is NULL for `color`.
void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color);
void overlay_end(int width, int height);
int overlay_columns(int codepoint); // 2 for wide East Asian characters, else 1
size_t overlay_text_columns(const char *text, size_t length);
