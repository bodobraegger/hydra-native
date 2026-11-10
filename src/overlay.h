// Draws colored rectangles and monospace text over the hydra output in one draw call per frame.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OVERLAY_MIN_FONT_SIZE 8.0f
#define OVERLAY_MAX_FONT_SIZE 56.0f

bool overlay_init(const char *font_path, float font_size);
bool overlay_set_font_size(float font_size);
float overlay_font_size(void);
float overlay_char_width(void);
float overlay_line_height(void);

// Colors are 0xRRGGBBAA.
void overlay_begin(void);
void overlay_rect(float x, float y, float width, float height, uint32_t color);
// Draws `length` bytes of UTF-8 from column `first_column` on, at most `max_columns` columns.
// `colors` gives one color per byte, or is NULL for `color`.
void overlay_text(float x, float y, const char *text, size_t length, size_t first_column, size_t max_columns,
                  const uint32_t *colors, uint32_t color);
void overlay_end(int width, int height);
