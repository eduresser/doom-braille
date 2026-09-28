#ifndef BRAILLE_H
#define BRAILLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} BrailleRGB;

/* The 16x16 recursive Bayer matrix used to spread lit dots inside a tile. */
extern const uint8_t BRAILLE_BAYER16[16][16];

/* ITU-R 601 luma, 16.16 fixed point. */
int braille_luma(int r, int g, int b);

/* Logarithmic dot-density ladder: 0, 1, 2, 4, ..., 128, 256 lit dots out of 256. */
int braille_density(int luminance);

/*
 * Render a pixel buffer into braille text. cols = px_w / 2, rows = px_h / 4;
 * trailing pixels that do not fill a whole 2x4 block are dropped. Each row
 * ends with '\n'. In color mode, a cell is preceded by a 24-bit color escape
 * (ESC[38;2;R;G;Bm) only when it differs from the previous cell's color.
 *
 * Returns a malloc'd, NUL-terminated UTF-8 buffer (caller must free()); if
 * out_len is non-NULL, it receives the length excluding the NUL terminator.
 */
char *braille_render(const BrailleRGB *px, int px_w, int px_h, int invert, int color, size_t *out_len);

/* Area-weighted (box filter) resample of src (src_w x src_h) into a new
 * dst_w x dst_h buffer (caller must free()). Works for both up- and
 * downsampling. */
BrailleRGB *braille_resample(const BrailleRGB *src, int src_w, int src_h, int dst_w, int dst_h);

#endif
