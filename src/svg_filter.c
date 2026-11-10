// SVG filter primitives (Filter Effects Module Level 1) for `filter: url(#id)` of the 2D canvas. Images are
// premultiplied RGBA floats in 0..1, in linearRGB or sRGB as color-interpolation-filters asks. The turbulence
// follows the reference code of the specification.
#include "svg_filter.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "canvas2d.h"

struct image {
    float *pixels; // premultiplied RGBA
    bool linear;
};

// ---------------------------------------------------------------- Gaussian blur

// One box blur of a line: each output is the mean of the inputs [x - left, x + right]; outside is transparent.
static void box_line(const float *in, float *out, int count, int left, int right) {
    float sum = 0;
    for (int x = -left; x <= right; x++) sum += x >= 0 && x < count ? in[x] : 0;
    float size = (float)(left + right + 1);
    for (int x = 0; x < count; x++) {
        out[x] = sum / size;
        int leaving = x - left, entering = x + right + 1;
        sum += (entering < count ? in[entering] : 0) - (leaving >= 0 ? in[leaving] : 0);
    }
}

// The three boxes of the specification for a standard deviation: size d = floor(s * 3 * sqrt(2 pi) / 4 + 0.5);
// an even d gives two boxes of size d (shifted left and right) and one of size d + 1.
static void blur_line(float *line, float *scratch, int count, float sigma) {
    int d = (int)floorf(sigma * 3 * sqrtf(2 * (float)M_PI) / 4 + 0.5f);
    if (d < 1) return;
    if (d % 2) {
        for (int pass = 0; pass < 3; pass++) {
            box_line(line, scratch, count, d / 2, d / 2);
            memcpy(line, scratch, sizeof(float) * (size_t)count);
        }
        return;
    }
    box_line(line, scratch, count, d / 2, d / 2 - 1);
    box_line(scratch, line, count, d / 2 - 1, d / 2);
    box_line(line, scratch, count, d / 2, d / 2);
    memcpy(line, scratch, sizeof(float) * (size_t)count);
}

// ponytail: box blurs also for a standard deviation below 2, where the specification asks for the true kernel.
void gaussian_blur(float *channel, int width, int height, float sigma_x, float sigma_y) {
    int longest = width > height ? width : height;
    float *line = malloc(sizeof(float) * (size_t)longest), *scratch = malloc(sizeof(float) * (size_t)longest);
    for (int y = 0; sigma_x > 0 && y < height; y++) blur_line(channel + (size_t)y * (size_t)width, scratch, width, sigma_x);
    for (int x = 0; sigma_y > 0 && x < width; x++) {
        for (int y = 0; y < height; y++) line[y] = channel[(size_t)y * (size_t)width + (size_t)x];
        blur_line(line, scratch, height, sigma_y);
        for (int y = 0; y < height; y++) channel[(size_t)y * (size_t)width + (size_t)x] = line[y];
    }
    free(line);
    free(scratch);
}

// ---------------------------------------------------------------- color spaces

static float to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

static void convert(struct image *image, size_t count, bool linear) {
    if (image->linear == linear) return;
    for (size_t i = 0; i < count; i++) {
        float *p = image->pixels + i * 4;
        if (p[3] <= 0) continue;
        for (int c = 0; c < 3; c++) p[c] = (linear ? to_linear(p[c] / p[3]) : to_srgb(p[c] / p[3])) * p[3];
    }
    image->linear = linear;
}

// ---------------------------------------------------------------- turbulence (reference code of the specification)

#define TURBULENCE_SIZE 0x100
#define TURBULENCE_MASK 0xff
#define PERLIN_N 0x1000
#define RANDOM_M 2147483647
#define RANDOM_A 16807
#define RANDOM_Q 127773
#define RANDOM_R 2836

struct turbulence {
    int lattice[TURBULENCE_SIZE + TURBULENCE_SIZE + 2];
    double gradient[4][TURBULENCE_SIZE + TURBULENCE_SIZE + 2][2];
};

struct stitch {
    int width, height, wrap_x, wrap_y;
};

static long random_next(long seed) {
    long result = RANDOM_A * (seed % RANDOM_Q) - RANDOM_R * (seed / RANDOM_Q);
    return result <= 0 ? result + RANDOM_M : result;
}

static void turbulence_init(struct turbulence *t, long seed) {
    if (seed <= 0) seed = -(seed % (RANDOM_M - 1)) + 1;
    if (seed > RANDOM_M - 1) seed = RANDOM_M - 1;
    int i = 0, j, k;
    for (k = 0; k < 4; k++) {
        for (i = 0; i < TURBULENCE_SIZE; i++) {
            t->lattice[i] = i;
            for (j = 0; j < 2; j++)
                t->gradient[k][i][j] = (double)(((seed = random_next(seed)) % (TURBULENCE_SIZE + TURBULENCE_SIZE)) - TURBULENCE_SIZE) / TURBULENCE_SIZE;
            double s = sqrt(t->gradient[k][i][0] * t->gradient[k][i][0] + t->gradient[k][i][1] * t->gradient[k][i][1]);
            if (s > 0) {
                t->gradient[k][i][0] /= s;
                t->gradient[k][i][1] /= s;
            }
        }
    }
    while (--i) {
        k = t->lattice[i];
        t->lattice[i] = t->lattice[j = (int)((seed = random_next(seed)) % TURBULENCE_SIZE)];
        t->lattice[j] = k;
    }
    for (i = 0; i < TURBULENCE_SIZE + 2; i++) {
        t->lattice[TURBULENCE_SIZE + i] = t->lattice[i];
        for (k = 0; k < 4; k++)
            for (j = 0; j < 2; j++) t->gradient[k][TURBULENCE_SIZE + i][j] = t->gradient[k][i][j];
    }
}

static double s_curve(double t) { return t * t * (3. - 2. * t); }
static double mix(double t, double a, double b) { return a + t * (b - a); }

static double noise2(const struct turbulence *t, int channel, const double vec[2], const struct stitch *stitch) {
    double tx = vec[0] + PERLIN_N, ty = vec[1] + PERLIN_N;
    int bx0 = (int)tx & TURBULENCE_MASK, bx1 = (bx0 + 1) & TURBULENCE_MASK;
    int by0 = (int)ty & TURBULENCE_MASK, by1 = (by0 + 1) & TURBULENCE_MASK;
    double rx0 = tx - (int)tx, rx1 = rx0 - 1.0, ry0 = ty - (int)ty, ry1 = ry0 - 1.0;
    if (stitch) {
        if (bx0 >= stitch->wrap_x) bx0 -= stitch->width;
        if (bx1 >= stitch->wrap_x) bx1 -= stitch->width;
        if (by0 >= stitch->wrap_y) by0 -= stitch->height;
        if (by1 >= stitch->wrap_y) by1 -= stitch->height;
    }
    bx0 &= TURBULENCE_MASK;
    bx1 &= TURBULENCE_MASK;
    by0 &= TURBULENCE_MASK;
    by1 &= TURBULENCE_MASK;
    int i = t->lattice[bx0], j = t->lattice[bx1];
    int b00 = t->lattice[i + by0], b10 = t->lattice[j + by0], b01 = t->lattice[i + by1], b11 = t->lattice[j + by1];
    double sx = s_curve(rx0), sy = s_curve(ry0);
    const double *q = t->gradient[channel][b00];
    double u = rx0 * q[0] + ry0 * q[1];
    q = t->gradient[channel][b10];
    double v = rx1 * q[0] + ry0 * q[1];
    double a = mix(sx, u, v);
    q = t->gradient[channel][b01];
    u = rx0 * q[0] + ry1 * q[1];
    q = t->gradient[channel][b11];
    v = rx1 * q[0] + ry1 * q[1];
    return mix(sy, a, mix(sx, u, v));
}

static double turbulence_at(const struct turbulence *t, int channel, double x, double y, double frequency_x, double frequency_y,
                            int octaves, bool fractal, bool stitching, double tile_width, double tile_height) {
    struct stitch stitch, *stitch_info = NULL;
    if (stitching) {
        if (frequency_x != 0.0) {
            double low = floor(tile_width * frequency_x) / tile_width, high = ceil(tile_width * frequency_x) / tile_width;
            frequency_x = frequency_x / low < high / frequency_x ? low : high;
        }
        if (frequency_y != 0.0) {
            double low = floor(tile_height * frequency_y) / tile_height, high = ceil(tile_height * frequency_y) / tile_height;
            frequency_y = frequency_y / low < high / frequency_y ? low : high;
        }
        stitch_info = &stitch;
        stitch.width = (int)(tile_width * frequency_x + 0.5);
        stitch.wrap_x = (int)(PERLIN_N + stitch.width);
        stitch.height = (int)(tile_height * frequency_y + 0.5);
        stitch.wrap_y = (int)(PERLIN_N + stitch.height);
    }
    double sum = 0, vec[2] = { x * frequency_x, y * frequency_y }, ratio = 1;
    for (int octave = 0; octave < octaves; octave++) {
        double noise = noise2(t, channel, vec, stitch_info);
        sum += (fractal ? noise : fabs(noise)) / ratio;
        vec[0] *= 2;
        vec[1] *= 2;
        ratio *= 2;
        if (stitch_info) {
            stitch.width *= 2;
            stitch.wrap_x = 2 * stitch.wrap_x - PERLIN_N;
            stitch.height *= 2;
            stitch.wrap_y = 2 * stitch.wrap_y - PERLIN_N;
        }
    }
    return sum;
}

// ---------------------------------------------------------------- primitives

static float clamp01(float value) { return value < 0 ? 0 : value > 1 ? 1 : value; }

static uint32_t to_argb(const float *p) {
    return (uint32_t)(clamp01(p[3]) * 255 + 0.5f) << 24 | (uint32_t)(clamp01(p[0]) * 255 + 0.5f) << 16 |
           (uint32_t)(clamp01(p[1]) * 255 + 0.5f) << 8 | (uint32_t)(clamp01(p[2]) * 255 + 0.5f);
}

static void from_argb(uint32_t pixel, float *p) {
    p[0] = (float)((pixel >> 16) & 0xFF) / 255;
    p[1] = (float)((pixel >> 8) & 0xFF) / 255;
    p[2] = (float)(pixel & 0xFF) / 255;
    p[3] = (float)(pixel >> 24) / 255;
}

// A component transfer function: type, slope, intercept, amplitude, exponent, offset, count, table values.
static float transfer(const float *function, float c) {
    int type = (int)function[0], n = (int)function[6];
    const float *table = function + 7;
    switch (type) {
    case 1: { // table
        if (n < 1) return c;
        if (n == 1 || c >= 1) return table[n - 1];
        int k = (int)(c * (float)(n - 1));
        return table[k] + (c * (float)(n - 1) - (float)k) * (table[k + 1] - table[k]);
    }
    case 2: { // discrete
        if (n < 1) return c;
        int k = (int)(c * (float)n);
        return table[k < n ? k : n - 1];
    }
    case 3: return function[1] * c + function[2];                         // linear
    case 4: return function[3] * powf(c, function[4]) + function[5];      // gamma
    default: return c;                                                    // identity
    }
}

static float arithmetic(const float *k, float i1, float i2) { return clamp01(k[0] * i1 * i2 + k[1] * i1 + k[2] * i2 + k[3]); }

void svg_filter_apply(uint32_t *pixels, int stride, int width, int height, const float *program, size_t length) {
    size_t count = (size_t)width * (size_t)height, bytes = count * 4 * sizeof(float);
    int primitive_count = length ? (int)program[0] : 0;
    if (primitive_count <= 0) { // a filter element without primitives draws nothing
        for (int y = 0; y < height; y++) memset(pixels + (size_t)y * (size_t)(stride / 4), 0, (size_t)width * 4);
        return;
    }
    struct image *results = calloc((size_t)primitive_count + 1, sizeof *results);
    struct image source = { malloc(bytes), false }, source_alpha = { calloc(count * 4, sizeof(float)), false };
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            size_t i = (size_t)y * (size_t)width + (size_t)x;
            from_argb(pixels[(size_t)y * (size_t)(stride / 4) + (size_t)x], source.pixels + i * 4);
            source_alpha.pixels[i * 4 + 3] = source.pixels[i * 4 + 3];
        }
    struct image *last = &source;
    size_t at = 1;
    for (int p = 0; p < primitive_count && at + 5 <= length; p++) {
        int type = (int)program[at], inputs[2] = { (int)program[at + 1], (int)program[at + 2] };
        bool linear = program[at + 3] != 0;
        int parameter_count = (int)program[at + 4];
        const float *parameters = program + at + 5;
        at += 5 + (size_t)parameter_count;
        if (at > length) break;
        // The inputs, converted into the color space of this primitive.
        struct image input[2];
        for (int k = 0; k < 2; k++) {
            const struct image *from = inputs[k] == SVG_SOURCE_GRAPHIC ? &source : inputs[k] == SVG_SOURCE_ALPHA ? &source_alpha
                                     : inputs[k] >= 0 && inputs[k] < p ? &results[inputs[k]] : last;
            input[k] = (struct image){ malloc(bytes), from->linear };
            memcpy(input[k].pixels, from->pixels, bytes);
            convert(&input[k], count, linear);
        }
        struct image out = { calloc(count * 4, sizeof(float)), linear };
        float *a = input[0].pixels, *b = input[1].pixels, *o = out.pixels;
        switch (type) {
        case SVG_GAUSSIAN_BLUR: {
            float *channel = malloc(sizeof(float) * count);
            for (int c = 0; c < 4; c++) {
                for (size_t i = 0; i < count; i++) channel[i] = a[i * 4 + (size_t)c];
                gaussian_blur(channel, width, height, parameters[0], parameters[1]);
                for (size_t i = 0; i < count; i++) o[i * 4 + (size_t)c] = channel[i];
            }
            free(channel);
            break;
        }
        case SVG_COLOR_MATRIX:
        case SVG_COMPONENT_TRANSFER:
            for (size_t i = 0; i < count; i++) {
                float color[4], alpha = a[i * 4 + 3], result[4];
                for (int c = 0; c < 3; c++) color[c] = alpha > 0 ? a[i * 4 + (size_t)c] / alpha : 0;
                color[3] = alpha;
                if (type == SVG_COLOR_MATRIX) {
                    for (int r = 0; r < 4; r++) {
                        const float *row = parameters + r * 5;
                        result[r] = clamp01(row[0] * color[0] + row[1] * color[1] + row[2] * color[2] + row[3] * color[3] + row[4]);
                    }
                } else {
                    const float *function = parameters;
                    for (int c = 0; c < 4; c++) {
                        result[c] = clamp01(transfer(function, color[c]));
                        function += 7 + (int)function[6];
                    }
                }
                for (int c = 0; c < 3; c++) o[i * 4 + (size_t)c] = result[c] * result[3];
                o[i * 4 + 3] = result[3];
            }
            break;
        case SVG_OFFSET: {
            int dx = (int)lroundf(parameters[0]), dy = (int)lroundf(parameters[1]);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++) {
                    int sx = x - dx, sy = y - dy;
                    if (sx >= 0 && sx < width && sy >= 0 && sy < height)
                        memcpy(o + ((size_t)y * (size_t)width + (size_t)x) * 4, a + ((size_t)sy * (size_t)width + (size_t)sx) * 4, 4 * sizeof(float));
                }
            break;
        }
        case SVG_BLEND: // in over in2 with the blend mode
            for (size_t i = 0; i < count; i++)
                from_argb(canvas2d_composite_pixel(to_argb(b + i * 4), to_argb(a + i * 4), (int)parameters[0]), o + i * 4);
            break;
        case SVG_COMPOSITE: {
            int operator = (int)parameters[0];
            for (size_t i = 0; i < count; i++) {
                const float *s = a + i * 4, *d = b + i * 4;
                float fs, fd; // the Porter-Duff factors of source (in) and destination (in2)
                switch (operator) {
                case 1: fs = d[3]; fd = 0; break;                  // in
                case 2: fs = 1 - d[3]; fd = 0; break;              // out
                case 3: fs = d[3]; fd = 1 - s[3]; break;           // atop
                case 4: fs = 1 - d[3]; fd = 1 - s[3]; break;       // xor
                case 6: fs = 1; fd = 1; break;                     // lighter
                default: fs = 1; fd = 1 - s[3]; break;             // over
                }
                for (int c = 0; c < 4; c++)
                    o[i * 4 + (size_t)c] = operator == 5 ? arithmetic(parameters + 1, s[c], d[c]) : clamp01(s[c] * fs + d[c] * fd);
                if (operator == 5) for (int c = 0; c < 3; c++) o[i * 4 + (size_t)c] = fminf(o[i * 4 + (size_t)c], o[i * 4 + 3]);
            }
            break;
        }
        case SVG_FLOOD: {
            float color[4] = { parameters[0], parameters[1], parameters[2], parameters[3] };
            for (int c = 0; c < 3 && linear; c++) color[c] = to_linear(color[c]);
            for (size_t i = 0; i < count; i++)
                for (int c = 0; c < 4; c++) o[i * 4 + (size_t)c] = c < 3 ? color[c] * color[3] : color[3];
            break;
        }
        case SVG_MERGE: // the inputs of the merge nodes, one over the other
            for (int m = 0; m < (int)parameters[0]; m++) {
                int index = (int)parameters[1 + m];
                struct image node = { malloc(bytes), false };
                const struct image *from = index == SVG_SOURCE_GRAPHIC ? &source : index == SVG_SOURCE_ALPHA ? &source_alpha
                                         : index >= 0 && index < p ? &results[index] : last;
                node.linear = from->linear;
                memcpy(node.pixels, from->pixels, bytes);
                convert(&node, count, linear);
                for (size_t i = 0; i < count; i++)
                    for (int c = 0; c < 4; c++) o[i * 4 + (size_t)c] = node.pixels[i * 4 + (size_t)c] + o[i * 4 + (size_t)c] * (1 - node.pixels[i * 4 + 3]);
                free(node.pixels);
            }
            break;
        case SVG_MORPHOLOGY: { // the minimum (erode) or maximum (dilate) over the rectangle, one axis after the other
            bool dilate = parameters[0] != 0;
            int radius[2] = { (int)parameters[1], (int)parameters[2] };
            float *pass = malloc(bytes);
            memcpy(pass, a, bytes);
            for (int axis = 0; axis < 2; axis++) {
                for (int y = 0; y < height; y++)
                    for (int x = 0; x < width; x++)
                        for (int c = 0; c < 4; c++) {
                            float value = dilate ? 0 : 1;
                            for (int k = -radius[axis]; k <= radius[axis]; k++) {
                                int sx = axis == 0 ? x + k : x, sy = axis == 1 ? y + k : y;
                                float sample = sx >= 0 && sx < width && sy >= 0 && sy < height ? pass[((size_t)sy * (size_t)width + (size_t)sx) * 4 + (size_t)c] : 0;
                                value = dilate ? fmaxf(value, sample) : fminf(value, sample);
                            }
                            o[((size_t)y * (size_t)width + (size_t)x) * 4 + (size_t)c] = value;
                        }
                memcpy(pass, o, bytes);
            }
            free(pass);
            break;
        }
        case SVG_TURBULENCE: {
            struct turbulence *noise = malloc(sizeof *noise);
            turbulence_init(noise, (long)lroundf(parameters[3]));
            bool fractal = parameters[5] != 0, stitching = parameters[4] != 0;
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++) {
                    float color[4];
                    for (int c = 0; c < 4; c++) {
                        double sum = turbulence_at(noise, c, x, y, parameters[0], parameters[1], (int)parameters[2], fractal, stitching,
                                                   width, height);
                        color[c] = clamp01((float)(fractal ? (sum + 1) / 2 : sum));
                    }
                    float *out_pixel = o + ((size_t)y * (size_t)width + (size_t)x) * 4;
                    for (int c = 0; c < 3; c++) out_pixel[c] = color[c] * color[3];
                    out_pixel[3] = color[3];
                }
            free(noise);
            break;
        }
        case SVG_DISPLACEMENT_MAP: { // in moves by the unpremultiplied channels of in2
            float scale = parameters[0];
            int channel_x = (int)parameters[1], channel_y = (int)parameters[2];
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++) {
                    const float *map = b + ((size_t)y * (size_t)width + (size_t)x) * 4;
                    float alpha = map[3];
                    float mx = channel_x == 3 ? alpha : alpha > 0 ? map[channel_x] / alpha : 0;
                    float my = channel_y == 3 ? alpha : alpha > 0 ? map[channel_y] / alpha : 0;
                    int sx = (int)floorf((float)x + scale * (mx - 0.5f) + 0.5f), sy = (int)floorf((float)y + scale * (my - 0.5f) + 0.5f);
                    if (sx >= 0 && sx < width && sy >= 0 && sy < height)
                        memcpy(o + ((size_t)y * (size_t)width + (size_t)x) * 4, a + ((size_t)sy * (size_t)width + (size_t)sx) * 4, 4 * sizeof(float));
                }
            break;
        }
        default: memcpy(o, a, bytes);
        }
        free(input[0].pixels);
        free(input[1].pixels);
        results[p] = out;
        last = &results[p];
    }
    convert(last, count, false);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            pixels[(size_t)y * (size_t)(stride / 4) + (size_t)x] = to_argb(last->pixels + ((size_t)y * (size_t)width + (size_t)x) * 4);
    for (int p = 0; p < primitive_count; p++) free(results[p].pixels);
    free(results);
    free(source.pixels);
    free(source_alpha.pixels);
}
