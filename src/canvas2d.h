#pragma once

#include "quickjs.h"

// Adds the canvas* functions of the 2D canvas to the `native` object.
void canvas2d_install(JSContext *ctx, JSValue native);

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// true when the download was an image of the canvas (new Image(), createElement('img')).
bool canvas2d_fetch_done(int fetch, bool ok, const char *data, size_t length);
// The pixels of a canvas or image as RGBA rows from the top (malloc'd), straight or premultiplied alpha.
unsigned char *canvas2d_rgba(int id, bool premultiplied, int *width, int *height);
// Replaces the pixels of a canvas with RGBA pixels, straight or premultiplied alpha, rows from the top or the bottom.
void canvas2d_put_rgba(int id, const unsigned char *rgba, int width, int height, bool premultiplied, bool flip_y);
// Composites one premultiplied ARGB pixel over another with a blend mode of BLEND_MODES in js/canvas.js.
uint32_t canvas2d_composite_pixel(uint32_t destination, uint32_t source, int mode);
