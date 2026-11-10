// SVG filter primitives for `filter: url(#id)` of the 2D canvas and the Gaussian blur of canvas shadows.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The primitives, in the order of SVG_PRIMITIVES in js/canvas.js.
enum svg_primitive {
    SVG_GAUSSIAN_BLUR, SVG_COLOR_MATRIX, SVG_COMPONENT_TRANSFER, SVG_OFFSET, SVG_BLEND, SVG_COMPOSITE, SVG_FLOOD,
    SVG_MERGE, SVG_MORPHOLOGY, SVG_TURBULENCE, SVG_DISPLACEMENT_MAP,
};
#define SVG_SOURCE_GRAPHIC -1
#define SVG_SOURCE_ALPHA -2

// The approximation of a Gaussian blur of the Filter Effects specification (three box blurs per axis) on one
// channel of `width` by `height` floats.
void gaussian_blur(float *channel, int width, int height, float sigma_x, float sigma_y);

// Applies a filter program of js/canvas.js (compileSvgFilter) to premultiplied ARGB pixels in place:
// for each primitive: type, in, in2, linearRGB (0 or 1), the count of parameters, the parameters.
void svg_filter_apply(uint32_t *pixels, int stride, int width, int height, const float *program, size_t length);
