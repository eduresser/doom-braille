#include "term.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Same dot numbering as braille.c: dot 1=bit0, 2=bit1, 3=bit2, 7=bit6 (left
 * column, top to bottom); dot 4=bit3, 5=bit4, 6=bit5, 8=bit7 (right column,
 * top to bottom) - normative, see spec.md step 7. */
static const int TERM_BIT_FOR_POS[4][2] = {
    {0, 3},
    {1, 4},
    {2, 5},
    {6, 7},
};

/* The game always runs on a black background, whatever the terminal theme:
 * the background is set to true black before every erase (erase fills with
 * the current background) and nothing but LEAVE_SEQ resets it. */
#define BG_BLACK "\x1b[48;2;0;0;0m"
#define ENTER_SEQ "\x1b[?1049h" BG_BLACK "\x1b[2J\x1b[H\x1b[?25l"
#define LEAVE_SEQ "\x1b[0m\x1b[?25h\x1b[?1049l"
#define CLEAR_SEQ BG_BLACK "\x1b[2J\x1b[H"

#define KITTY_QUERY_SEQ "\x1b[?u\x1b[c"
#define KITTY_PUSH_SEQ "\x1b[>11u"
#define KITTY_POP_SEQ "\x1b[<u"

static struct termios g_orig_termios;
static volatile sig_atomic_t g_have_orig_termios = 0;
static volatile sig_atomic_t g_entered = 0;
static int g_term_fd = -1;
static int g_kitty_pushed = 0;

static volatile sig_atomic_t g_resize_flag = 0;
static volatile sig_atomic_t g_resize_installed = 0;

/* Bytes read by term_detect_kitty() that were not part of the recognized
 * query reply - real keystrokes the user typed during the startup probe,
 * handed to the decoder afterwards via term_take_pending(). */
#define TERM_PENDING_CAP 256
static char g_pending[TERM_PENDING_CAP];
static size_t g_pending_len = 0;

static void term_pending_append(const char *data, size_t n) {
    if (n > (size_t)(TERM_PENDING_CAP - g_pending_len)) {
        n = (size_t)(TERM_PENDING_CAP - g_pending_len);
    }
    if (n == 0) return;
    memcpy(g_pending + g_pending_len, data, n);
    g_pending_len += n;
}

static int full_write(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

void term_screen_init(TermScreen *scr) {
    memset(scr, 0, sizeof(*scr));
}

void term_screen_free(TermScreen *scr) {
    free(scr->prev);
    memset(scr, 0, sizeof(*scr));
}

void term_screen_reset(TermScreen *scr) {
    free(scr->prev);
    scr->prev = NULL;
    scr->cols = 0;
    scr->rows = 0;
}

TermCell *term_cells_from_pixels(const BrailleRGB *px, int px_w, int px_h, int invert, int color) {
    int cols = px_w / 2;
    int rows = px_h / 4;
    if (cols < 0) cols = 0;
    if (rows < 0) rows = 0;

    TermCell *cells = malloc(sizeof(TermCell) * (size_t)cols * (size_t)rows);

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
                        bits |= 1u << TERM_BIT_FOR_POS[ry][rx];
                    }
                }
            }

            TermCell *cell = &cells[(size_t)cy * cols + cx];
            cell->bits = (uint8_t)bits;
            if (color) {
                cell->has_color = 1;
                cell->r = (uint8_t)((sum_r + 4) / 8);
                cell->g = (uint8_t)((sum_g + 4) / 8);
                cell->b = (uint8_t)((sum_b + 4) / 8);
            } else {
                cell->has_color = 0;
                cell->r = cell->g = cell->b = 0;
            }
        }
    }

    return cells;
}

ssize_t term_draw(TermScreen *scr, const TermCell *cells, int cols, int rows, int off_col, int off_row, int fd) {
    int full_redraw = 0;
    if (!scr->prev || scr->cols != cols || scr->rows != rows) {
        free(scr->prev);
        scr->prev = malloc(sizeof(TermCell) * (size_t)cols * (size_t)rows);
        scr->cols = cols;
        scr->rows = rows;
        full_redraw = 1;
    }

    size_t cap = (size_t)cols * (size_t)rows * 40 + 64;
    char *buf = malloc(cap);
    size_t len = 0;

    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            const TermCell *cell = &cells[(size_t)cy * cols + cx];
            TermCell *prev = &scr->prev[(size_t)cy * cols + cx];

            int changed = full_redraw ||
                          prev->bits != cell->bits ||
                          prev->has_color != cell->has_color ||
                          (cell->has_color && (prev->r != cell->r || prev->g != cell->g || prev->b != cell->b));

            if (changed) {
                len += (size_t)sprintf(buf + len, "\x1b[%d;%dH", off_row + cy, off_col + cx);

                if (cell->has_color &&
                    (!scr->have_last_color || scr->last_r != cell->r || scr->last_g != cell->g ||
                     scr->last_b != cell->b)) {
                    len += (size_t)sprintf(buf + len, "\x1b[38;2;%d;%d;%dm", cell->r, cell->g, cell->b);
                    scr->have_last_color = 1;
                    scr->last_r = cell->r;
                    scr->last_g = cell->g;
                    scr->last_b = cell->b;
                }

                unsigned int codepoint = 0x2800u + cell->bits;
                buf[len++] = (char)(0xE0 | (codepoint >> 12));
                buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                buf[len++] = (char)(0x80 | (codepoint & 0x3F));

                *prev = *cell;
            }
        }
    }

    ssize_t result;
    if (len == 0) {
        result = 0;
    } else if (full_write(fd, buf, len) != 0) {
        result = -1;
    } else {
        result = (ssize_t)len;
    }
    free(buf);
    return result;
}

void term_clear(int fd) {
    full_write(fd, CLEAR_SEQ, strlen(CLEAR_SEQ));
}

static void term_restore_termios(void) {
    if (g_have_orig_termios && g_term_fd >= 0) {
        tcsetattr(g_term_fd, TCSAFLUSH, &g_orig_termios);
    }
}

void term_leave(void) {
    if (!g_entered) return;
    g_entered = 0;
    if (g_term_fd >= 0) {
        if (g_kitty_pushed) {
            full_write(g_term_fd, KITTY_POP_SEQ, strlen(KITTY_POP_SEQ));
        }
        full_write(g_term_fd, LEAVE_SEQ, strlen(LEAVE_SEQ));
    }
    g_kitty_pushed = 0;
    term_restore_termios();
}

static void term_atexit_handler(void) {
    term_leave();
}

static void term_signal_handler(int signum) {
    term_leave();
    signal(signum, SIG_DFL);
    raise(signum);
}

static void term_install_exit_handlers(void) {
    atexit(term_atexit_handler);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = term_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int term_enter(int fd) {
    if (g_entered) return 0;

    struct termios raw;
    if (tcgetattr(fd, &raw) != 0) return -1;
    g_orig_termios = raw;
    g_have_orig_termios = 1;

    cfmakeraw(&raw);
    if (tcsetattr(fd, TCSAFLUSH, &raw) != 0) return -1;

    g_term_fd = fd;
    g_entered = 1;

    if (full_write(fd, ENTER_SEQ, strlen(ENTER_SEQ)) != 0) {
        term_leave();
        return -1;
    }

    term_install_exit_handlers();
    return 0;
}

static void term_winch_handler(int signum) {
    (void)signum;
    g_resize_flag = 1;
}

int term_resize_pending(void) {
    if (!g_resize_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = term_winch_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, NULL);
        g_resize_installed = 1;
    }

    if (g_resize_flag) {
        g_resize_flag = 0;
        return 1;
    }
    return 0;
}

int term_get_size(int fd, int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

static long term_elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* Scans buf[0..len) for the first complete CSI reply (ESC '[' ... final
 * letter). Returns the index of the final byte, or -1 if none is complete
 * yet (caller should keep reading). Bytes before the CSI that aren't part
 * of it are junk-in-front and get folded into the pending buffer by the
 * caller alongside it. */
static int term_find_csi_end(const char *buf, size_t len, size_t start_at) {
    for (size_t i = start_at; i + 1 < len; i++) {
        if (buf[i] == 0x1b && buf[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && !isalpha((unsigned char)buf[j])) j++;
            if (j >= len) return -1; /* incomplete: keep waiting */
            return (int)j;
        }
    }
    return -1;
}

int term_detect_kitty(int in_fd, int out_fd, int timeout_ms) {
    if (full_write(out_fd, KITTY_QUERY_SEQ, strlen(KITTY_QUERY_SEQ)) != 0) return 0;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    char buf[TERM_PENDING_CAP];
    size_t len = 0;
    size_t scan_from = 0;

    int result = 0;
    int have_result = 0;
    int done = 0;

    /* The two replies we're waiting for (the Kitty flags query and the DA
     * query we always send alongside it), so both can be excluded from
     * what's handed back by term_take_pending(). */
    size_t match_start[2], match_end[2];
    int match_count = 0;

    while (!done) {
        for (;;) {
            int end = term_find_csi_end(buf, len, scan_from);
            if (end < 0) break; /* incomplete reply: wait for more bytes */

            char final = buf[end];
            if (final == 'u' || final == 'c') {
                if (!have_result) {
                    have_result = 1;
                    result = (final == 'u');
                }
                if (match_count < 2) {
                    size_t esc_start = (size_t)end;
                    while (esc_start > 0 && buf[esc_start] != 0x1b) esc_start--;
                    match_start[match_count] = esc_start;
                    match_end[match_count] = (size_t)end;
                    match_count++;
                }
                /* A DA reply with no Kitty reply before it means the
                 * terminal doesn't understand CSI ? u at all - it will
                 * never send one, so stop waiting. Otherwise, once both
                 * replies (Kitty flags, then DA) have been seen, we're
                 * done too. */
                if (!result || final == 'c') {
                    done = 1;
                    break;
                }
            }
            scan_from = (size_t)end + 1;
        }
        if (done) break;

        long remaining = timeout_ms - term_elapsed_ms(&start);
        if (remaining <= 0 || len >= sizeof(buf)) break;

        struct pollfd pfd = { .fd = in_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, (int)remaining);
        if (pr <= 0) break; /* timeout or error: assume no Kitty support */

        ssize_t n = read(in_fd, buf + len, sizeof(buf) - len);
        if (n <= 0) break;
        len += (size_t)n;
    }

    size_t pos = 0;
    for (int m = 0; m < match_count; m++) {
        term_pending_append(buf + pos, match_start[m] - pos);
        pos = match_end[m] + 1;
    }
    term_pending_append(buf + pos, len - pos);

    return result;
}

void term_push_kitty(int fd) {
    if (full_write(fd, KITTY_PUSH_SEQ, strlen(KITTY_PUSH_SEQ)) == 0) {
        g_kitty_pushed = 1;
    }
}

size_t term_take_pending(char *buf, size_t cap) {
    size_t n = g_pending_len < cap ? g_pending_len : cap;
    memcpy(buf, g_pending, n);
    g_pending_len = 0;
    return n;
}
