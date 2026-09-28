#ifndef TERM_H
#define TERM_H

#include <stdint.h>
#include <sys/types.h>

#include "braille.h"

/* One rendered braille character: dot bits (glyph = U+2800 + bits) plus an
 * optional 24-bit foreground color. */
typedef struct {
    uint8_t bits;
    int has_color;
    uint8_t r, g, b;
} TermCell;

/* Holds the previous frame's cells and the last color escape written, so
 * term_draw() can emit only what changed since the last call. Zero-init
 * (or term_screen_init()) before first use. */
typedef struct {
    TermCell *prev;
    int cols, rows;
    int have_last_color;
    uint8_t last_r, last_g, last_b;
} TermScreen;

void term_screen_init(TermScreen *scr);
void term_screen_free(TermScreen *scr);

/* Drops the stored previous frame so the next term_draw() call redraws
 * every cell. Call after a resize, once the new layout is known. */
void term_screen_reset(TermScreen *scr);

/* Converts a pixel buffer into a cols x rows (px_w/2 x px_h/4) grid of
 * TermCell using the same luma/density/Bayer pipeline as braille_render().
 * Returns a malloc'd array (caller frees). */
TermCell *term_cells_from_pixels(const BrailleRGB *px, int px_w, int px_h, int invert, int color);

/*
 * Diffs `cells` (cols x rows, positioned at 1-based off_col/off_row) against
 * the screen's previous frame and writes only the changed cells - each as a
 * cursor-move escape, an optional color escape (only when the color differs
 * from the last one written), and the glyph - to fd in a single write(2)
 * call. The first call after term_screen_init()/term_screen_reset(), or any
 * call where cols/rows differ from the stored frame, redraws every cell.
 *
 * Returns the number of bytes written (0 if nothing changed), or -1 on
 * write error (errno set).
 */
ssize_t term_draw(TermScreen *scr, const TermCell *cells, int cols, int rows, int off_col, int off_row, int fd);

/* Writes a clear-screen + cursor-home escape to fd. Use after a resize,
 * before redrawing at the new (possibly smaller) size, so no characters
 * from the previous, larger picture linger on screen. */
void term_clear(int fd);

/*
 * Puts the terminal behind fd into raw mode and switches it to the
 * alternate screen buffer, cleared, with the cursor hidden. Registers an
 * atexit handler plus SIGINT/SIGTERM handlers that call term_leave()
 * automatically, so the terminal is restored even on an unexpected exit.
 * Idempotent. Returns 0 on success, -1 on error (errno set).
 */
int term_enter(int fd);

/*
 * Restores cooked mode, shows the cursor and leaves the alternate screen
 * buffer. Safe to call more than once, and from a signal handler.
 */
void term_leave(void);

/*
 * Returns non-zero exactly once per SIGWINCH received since the last call
 * (edge-triggered), installing the SIGWINCH handler on first use.
 */
int term_resize_pending(void);

/* Queries the current terminal size via TIOCGWINSZ. Returns 0 on success,
 * -1 on error (errno set). */
int term_get_size(int fd, int *cols, int *rows);

/*
 * Probes whether the terminal on in_fd/out_fd supports the Kitty keyboard
 * protocol: writes `CSI ? u CSI c` to out_fd, then reads in_fd for up to
 * timeout_ms. Returns 1 if a `CSI ? <flags> u` reply arrives before the
 * Device Attributes (`CSI ? ... c`) reply, 0 otherwise (including on
 * timeout, so the caller never waits longer than timeout_ms). Any bytes
 * read that are not part of one of those two replies are kept (not lost)
 * and can be retrieved with term_take_pending().
 */
int term_detect_kitty(int in_fd, int out_fd, int timeout_ms);

/*
 * Enables the Kitty keyboard protocol on fd by writing `CSI > 11 u`
 * (report all key events, disambiguated), and marks it so term_leave()
 * writes `CSI < u` to pop it before leaving the alternate screen.
 */
void term_push_kitty(int fd);

/*
 * Copies up to cap bytes captured (but not consumed) by term_detect_kitty()
 * into buf and clears the internal buffer. Returns the number of bytes
 * copied, so a caller that fed real keystrokes into the probe doesn't lose
 * them.
 */
size_t term_take_pending(char *buf, size_t cap);

#endif
