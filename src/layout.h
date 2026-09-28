#ifndef LAYOUT_H
#define LAYOUT_H

typedef struct {
    double scale;
    int px_w, px_h;     /* resampled picture size, in dots */
    int cols, rows;     /* resampled picture size, in braille characters */
    int off_col, off_row; /* 1-based top-left character position, centered */
} BrailleLayout;

/*
 * Fits a disp_w x disp_h picture (in pixels/dots) into a term_cols x
 * term_rows terminal (in characters, each holding a 2x4 dot cell).
 *
 * Scale is 1 (native size, centered) when the picture already fits the
 * available dot area (term_cols*2 x term_rows*4); otherwise scale is the
 * largest ratio that fits both dimensions, sizes are floored (minimum 1
 * dot), and the result is centered.
 */
BrailleLayout braille_layout(int term_cols, int term_rows, int disp_w, int disp_h);

#endif
