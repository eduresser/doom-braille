#include "layout.h"

BrailleLayout braille_layout(int term_cols, int term_rows, int disp_w, int disp_h) {
    int avail_w = term_cols * 2;
    int avail_h = term_rows * 4;

    double scale = 1.0;
    if (disp_w > avail_w || disp_h > avail_h) {
        double sw = (double)avail_w / (double)disp_w;
        double sh = (double)avail_h / (double)disp_h;
        scale = sw < sh ? sw : sh;
    }

    int px_w = (int)(disp_w * scale);
    if (px_w < 1) px_w = 1;
    int px_h = (int)(disp_h * scale);
    if (px_h < 1) px_h = 1;

    BrailleLayout layout;
    layout.scale = scale;
    layout.px_w = px_w;
    layout.px_h = px_h;
    layout.cols = px_w / 2;
    layout.rows = px_h / 4;
    layout.off_col = (avail_w - px_w) / 2 / 2 + 1;
    layout.off_row = (avail_h - px_h) / 2 / 4 + 1;
    return layout;
}
