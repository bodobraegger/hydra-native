// Draws colored rectangles and monospace text over the hydra output in one draw call per frame.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OVERLAY_MIN_FONT_SIZE 8.0f
#define OVERLAY_MAX_FONT_SIZE 56.0f
#define OVERLAY_TAB_WIDTH 4 // a tab reaches the next multiple of 4 columns

bool overlay_init(const char *font_path, const char *emoji_font_path, float font_size);
bool overlay_set_font_size(float font_size);
bool overlay_set_font(const char *font_path); // the code font; false keeps the old one
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
void overlay_image(float x, float y, float width, float height, unsigned texture); // an RGBA GL texture
void overlay_end(int width, int height);
// Layers, drawn in this order, each in one draw call. What is drawn goes into the current layer, which is
// OVERLAY_TOP after overlay_begin(). Images draw after OVERLAY_TOP.
enum overlay_layer {
    OVERLAY_CODE_BACKGROUND, // the boxes behind the code lines
    OVERLAY_CODE_MARKS, // flash, selection, matches, brackets and whitespace marks
    OVERLAY_CODE_TEXT,
    OVERLAY_TOP, // cursor, toolbar, panels, bars
    // The boxes of the autocomplete list and the signature help, always with blend mode difference. The code under
    // a box does not show: the box composites over the hydra output alone.
    OVERLAY_POPUP,
    OVERLAY_POPUP_TEXT, // what the popup boxes hold, with normal blending
    OVERLAY_LAYER_COUNT
};
void overlay_set_layer(enum overlay_layer layer);
// A layer with `difference` composites with blend mode difference (|below - layer|) over what is drawn before it.
void overlay_set_difference(enum overlay_layer layer, bool difference);
const char *overlay_difference_method(void); // "KHR_blend_equation_advanced", "shader", or "none" before the first use
int overlay_columns(int codepoint); // 2 for wide East Asian characters, else 1
size_t overlay_text_columns(const char *text, size_t length);
// Proportional text, shaped with HarfBuzz, in visual order after a bidi pass; returns the width in pixels.
// For panels and prompts; the code uses the monospace grid of overlay_text().
float overlay_text_shaped(float x, float y, const char *text, size_t length, bool right_to_left, uint32_t color);
// Right-to-left text shaped as one run and drawn into `width` pixels from the right, shrunk when it is wider.
void overlay_text_fitted(float x, float y, const char *text, size_t length, float width, uint32_t color);
float overlay_text_shaped_width(const char *text, size_t length, bool right_to_left);
