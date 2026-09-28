#include "braille.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

const uint8_t BRAILLE_BAYER16[16][16] = {
    {0, 128, 32, 160, 8, 136, 40, 168, 2, 130, 34, 162, 10, 138, 42, 170},
    {192, 64, 224, 96, 200, 72, 232, 104, 194, 66, 226, 98, 202, 74, 234, 106},
    {48, 176, 16, 144, 56, 184, 24, 152, 50, 178, 18, 146, 58, 186, 26, 154},
    {240, 112, 208, 80, 248, 120, 216, 88, 242, 114, 210, 82, 250, 122, 218, 90},
    {12, 140, 44, 172, 4, 132, 36, 164, 14, 142, 46, 174, 6, 134, 38, 166},
    {204, 76, 236, 108, 196, 68, 228, 100, 206, 78, 238, 110, 198, 70, 230, 102},
    {60, 188, 28, 156, 52, 180, 20, 148, 62, 190, 30, 158, 54, 182, 22, 150},
    {252, 124, 220, 92, 244, 116, 212, 84, 254, 126, 222, 94, 246, 118, 214, 86},
    {3, 131, 35, 163, 11, 139, 43, 171, 1, 129, 33, 161, 9, 137, 41, 169},
    {195, 67, 227, 99, 203, 75, 235, 107, 193, 65, 225, 97, 201, 73, 233, 105},
    {51, 179, 19, 147, 59, 187, 27, 155, 49, 177, 17, 145, 57, 185, 25, 153},
    {243, 115, 211, 83, 251, 123, 219, 91, 241, 113, 209, 81, 249, 121, 217, 89},
    {15, 143, 47, 175, 7, 135, 39, 167, 13, 141, 45, 173, 5, 133, 37, 165},
    {207, 79, 239, 111, 199, 71, 231, 103, 205, 77, 237, 109, 197, 69, 229, 101},
    {63, 191, 31, 159, 55, 183, 23, 151, 61, 189, 29, 157, 53, 181, 21, 149},
    {255, 127, 223, 95, 247, 119, 215, 87, 253, 125, 221, 93, 245, 117, 213, 85},
};

/* dot 1=bit0, 2=bit1, 3=bit2, 7=bit6 (left column, top to bottom);
 * dot 4=bit3, 5=bit4, 6=bit5, 8=bit7 (right column, top to bottom). */
static const int BIT_FOR_POS[4][2] = {
    {0, 3},
    {1, 4},
    {2, 5},
    {6, 7},
};

int braille_luma(int r, int g, int b) {
    return (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
}

/* 0, 1, 2, 4, ..., 128, 256 for a target of 0..256 dots. */
static int pow2_ladder(double target) {
    if (target < 0.70710678118654752440 /* 2^-0.5 */) {
        return 0;
    }
    int level = (int)lround(log2(target));
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    return 1 << level;
}

int braille_density(int luminance) {
    double target = (double)luminance * 256.0 / 255.0;
    if (target <= 128.0) {
        return pow2_ladder(target);
    }
    /* Bright half mirrors the dark one: count unlit dots on the same ladder. */
    return 256 - pow2_ladder(256.0 - target);
}

static size_t utf8_encode(unsigned int codepoint, char *out) {
    /* All braille codepoints (U+2800..U+28FF) fit the 3-byte UTF-8 form. */
    out[0] = (char)(0xE0 | (codepoint >> 12));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = (char)(0x80 | (codepoint & 0x3F));
    return 3;
}

char *braille_render(const BrailleRGB *px, int px_w, int px_h, int invert, int color, size_t *out_len) {
    int cols = px_w / 2;
    int rows = px_h / 4;
    if (cols < 0) cols = 0;
    if (rows < 0) rows = 0;

    size_t cap = (size_t)cols * rows * (3 + 19) + (size_t)rows + 1;
    if (cap < 1) cap = 1;
    char *buf = malloc(cap);
    size_t len = 0;

    int have_last_color = 0;
    int last_r = -1, last_g = -1, last_b = -1;

    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            unsigned int bits = 0;
            long sum_r = 0, sum_g = 0, sum_b = 0;

            for (int ry = 0; ry < 4; ry++) {
                int y = cy * 4 + ry;
                int yy = y % 16;
                for (int rx = 0; rx < 2; rx++) {
                    int x = cx * 2 + rx;
                    int xx = x % 16;
                    const BrailleRGB *p = &px[(size_t)y * px_w + x];

                    if (color) {
                        sum_r += p->r;
                        sum_g += p->g;
                        sum_b += p->b;
                    }

                    int luminance = braille_luma(p->r, p->g, p->b);
                    if (!invert) luminance = 255 - luminance;
                    int density = braille_density(luminance);
                    if (BRAILLE_BAYER16[yy][xx] < density) {
                        bits |= 1u << BIT_FOR_POS[ry][rx];
                    }
                }
            }

            if (color) {
                int avg_r = (int)((sum_r + 4) / 8);
                int avg_g = (int)((sum_g + 4) / 8);
                int avg_b = (int)((sum_b + 4) / 8);
                if (!have_last_color || avg_r != last_r || avg_g != last_g || avg_b != last_b) {
                    len += (size_t)sprintf(buf + len, "\x1b[38;2;%d;%d;%dm", avg_r, avg_g, avg_b);
                    last_r = avg_r;
                    last_g = avg_g;
                    last_b = avg_b;
                    have_last_color = 1;
                }
            }

            len += utf8_encode(0x2800 + bits, buf + len);
        }
        buf[len++] = '\n';
    }
    buf[len] = '\0';

    if (out_len) *out_len = len;
    return buf;
}

BrailleRGB *braille_resample(const BrailleRGB *src, int src_w, int src_h, int dst_w, int dst_h) {
    BrailleRGB *dst = malloc(sizeof(BrailleRGB) * (size_t)dst_w * (size_t)dst_h);
    double scale_x = (double)src_w / (double)dst_w;
    double scale_y = (double)src_h / (double)dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        double sy0 = dy * scale_y;
        double sy1 = sy0 + scale_y;
        int y0 = (int)floor(sy0);
        int y1 = (int)ceil(sy1);
        if (y1 > src_h) y1 = src_h;

        for (int dx = 0; dx < dst_w; dx++) {
            double sx0 = dx * scale_x;
            double sx1 = sx0 + scale_x;
            int x0 = (int)floor(sx0);
            int x1 = (int)ceil(sx1);
            if (x1 > src_w) x1 = src_w;

            double sum_r = 0, sum_g = 0, sum_b = 0, weight = 0;
            for (int sy = y0; sy < y1; sy++) {
                double wy = fmin((double)(sy + 1), sy1) - fmax((double)sy, sy0);
                if (wy <= 0) continue;
                for (int sx = x0; sx < x1; sx++) {
                    double wx = fmin((double)(sx + 1), sx1) - fmax((double)sx, sx0);
                    if (wx <= 0) continue;
                    double w = wx * wy;
                    const BrailleRGB *p = &src[(size_t)sy * src_w + sx];
                    sum_r += p->r * w;
                    sum_g += p->g * w;
                    sum_b += p->b * w;
                    weight += w;
                }
            }

            BrailleRGB *out = &dst[(size_t)dy * dst_w + dx];
            if (weight > 0) {
                out->r = (uint8_t)(sum_r / weight + 0.5);
                out->g = (uint8_t)(sum_g / weight + 0.5);
                out->b = (uint8_t)(sum_b / weight + 0.5);
            } else {
                int cy = y0 < src_h ? y0 : src_h - 1;
                int cx = x0 < src_w ? x0 : src_w - 1;
                *out = src[(size_t)cy * src_w + cx];
            }
        }
    }

    return dst;
}
