#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "braille.h"
#include "input.h"
#include "layout.h"
#include "term.h"

typedef struct {
    BrailleRGB *px;
    int w, h;
} Image;

static int read_token(FILE *f, char *tok, size_t cap) {
    int c;
    do {
        c = fgetc(f);
        if (c == '#') {
            while (c != '\n' && c != EOF) c = fgetc(f);
        }
    } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
    if (c == EOF) return 0;

    size_t n = 0;
    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        if (n + 1 < cap) tok[n++] = (char)c;
        c = fgetc(f);
    }
    tok[n] = '\0';
    return 1;
}

static Image *read_pnm(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "braille-cli: cannot open %s\n", path);
        return NULL;
    }

    char magic[3], tok[32];
    if (!read_token(f, magic, sizeof(magic)) ||
        (strcmp(magic, "P5") != 0 && strcmp(magic, "P6") != 0)) {
        fprintf(stderr, "braille-cli: %s is not a binary PGM/PPM (P5/P6)\n", path);
        fclose(f);
        return NULL;
    }
    int channels = strcmp(magic, "P5") == 0 ? 1 : 3;

    if (!read_token(f, tok, sizeof(tok))) { fclose(f); return NULL; }
    int w = atoi(tok);
    if (!read_token(f, tok, sizeof(tok))) { fclose(f); return NULL; }
    int h = atoi(tok);
    if (!read_token(f, tok, sizeof(tok))) { fclose(f); return NULL; }
    /* maxval, assumed 255; single whitespace byte already consumed by read_token */

    if (w <= 0 || h <= 0) {
        fprintf(stderr, "braille-cli: %s has an invalid size\n", path);
        fclose(f);
        return NULL;
    }

    size_t count = (size_t)w * (size_t)h;
    unsigned char *raw = malloc(count * (size_t)channels);
    if (fread(raw, (size_t)channels, count, f) != count) {
        fprintf(stderr, "braille-cli: %s is truncated\n", path);
        free(raw);
        fclose(f);
        return NULL;
    }
    fclose(f);

    Image *img = malloc(sizeof(Image));
    img->w = w;
    img->h = h;
    img->px = malloc(sizeof(BrailleRGB) * count);
    for (size_t i = 0; i < count; i++) {
        if (channels == 1) {
            img->px[i].r = img->px[i].g = img->px[i].b = raw[i];
        } else {
            img->px[i].r = raw[i * 3 + 0];
            img->px[i].g = raw[i * 3 + 1];
            img->px[i].b = raw[i * 3 + 2];
        }
    }
    free(raw);
    return img;
}

static void print_usage(void) {
    fprintf(stderr,
            "usage: braille-cli [--no-invert] [--color] [--cols N --rows N] [--bench N] <input.pgm|input.ppm>\n"
            "       braille-cli --layout TERMCOLSxTERMROWS --display WxH\n"
            "       braille-cli --cols N --rows N --diff-demo A.pgm B.pgm\n"
            "       braille-cli --display WxH --live-demo <input.pgm|input.ppm>\n"
            "       braille-cli --tty-selftest exit|resize\n"
            "       braille-cli --keys legacy|kitty [--release-ms N]\n");
}

/* Parses a run of "XX" hex byte pairs (optionally space-separated), stopping
 * at the first non-hex character (typically the trailing newline). Returns
 * the number of bytes parsed. */
static int parse_hex_bytes(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*s == ' ') s++;
    while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1])) {
        if (n >= cap) return -1;
        unsigned int byte;
        sscanf(s, "%2x", &byte);
        out[n++] = (uint8_t)byte;
        s += 2;
        while (*s == ' ') s++;
    }
    return (int)n;
}

static void print_input_event(long ms, const InputEvent *ev) {
    switch (ev->kind) {
        case INPUT_KEY_DOWN: printf("%ld down %d\n", ms, ev->code); break;
        case INPUT_KEY_UP: printf("%ld up %d\n", ms, ev->code); break;
        case INPUT_TOGGLE_COLOR: printf("%ld toggle-color\n", ms); break;
        case INPUT_TOGGLE_INVERT: printf("%ld toggle-invert\n", ms); break;
        case INPUT_QUIT: printf("%ld quit\n", ms); break;
    }
}

/*
 * Drives the key decoder (src/input.c) from a scripted stdin instead of a
 * real terminal, so tests can control the virtual clock precisely (AC-022,
 * AC-023, AC-025). Each stdin line is either "@<ms> <hex bytes>" (feed those
 * bytes at that time) or "@<ms>" alone (only advance the clock). Decoded
 * events are printed one per line, timestamped with the line's ms.
 */
static int run_keys_mode(InputMode mode, int release_ms) {
    InputDecoder dec;
    input_init(&dec, mode, release_ms);

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        char *p = line;
        while (*p == ' ') p++;
        if (*p != '@') continue;
        p++;

        char *end;
        long ms = strtol(p, &end, 10);
        p = end;

        uint8_t bytes[2048];
        int n = parse_hex_bytes(p, bytes, sizeof(bytes));
        if (n > 0) input_feed(&dec, bytes, (size_t)n, ms);
        input_tick(&dec, ms);

        InputEvent ev;
        while (input_pop(&dec, &ev)) print_input_event(ms, &ev);
    }
    return 0;
}

static int parse_dims(const char *s, int *a, int *b) {
    char sep;
    return sscanf(s, "%d%c%d", a, &sep, b) == 3 && (sep == 'x' || sep == 'X');
}

/*
 * Drives term_enter()/term_leave()/term_resize_pending() against a real pty
 * so tests can verify AC-011 (clean exit) and AC-007 (SIGWINCH re-fit)
 * without depending on an external pty tool. A forked drainer process
 * continuously copies the pty master to our real stdout, so the (possibly
 * large) frames term_draw() writes to the slave never fill the kernel's
 * pty buffer and block. A control-character marker is written to the pty
 * between phases so the test can split the captured stdout back into
 * per-phase segments; the final termios flags go to stderr.
 */
#define TTY_SELFTEST_MARKER "\x04""BOUNDARY""\x04"

static void tty_write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += (size_t)n;
    }
}

static void tty_write_marker(int fd) {
    tty_write_all(fd, TTY_SELFTEST_MARKER, strlen(TTY_SELFTEST_MARKER));
}

static void tty_selftest_draw(TermScreen *scr, int term_cols, int term_rows, int fd) {
    BrailleRGB src[8 * 8];
    memset(src, 0, sizeof(src));

    BrailleLayout layout = braille_layout(term_cols, term_rows, 320, 240);
    BrailleRGB *resampled = braille_resample(src, 8, 8, layout.px_w, layout.px_h);
    TermCell *cells = term_cells_from_pixels(resampled, layout.px_w, layout.px_h, 1, 0);
    term_draw(scr, cells, layout.cols, layout.rows, layout.off_col, layout.off_row, fd);
    free(cells);
    free(resampled);
}

static int run_tty_selftest(const char *mode) {
    int is_resize = strcmp(mode, "resize") == 0;

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        perror("braille-cli: posix_openpt");
        return 1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        perror("braille-cli: grantpt/unlockpt");
        return 1;
    }
    char *slave_name_p = ptsname(master);
    if (!slave_name_p) {
        perror("braille-cli: ptsname");
        return 1;
    }
    char slave_name[256];
    strncpy(slave_name, slave_name_p, sizeof(slave_name) - 1);
    slave_name[sizeof(slave_name) - 1] = '\0';

    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        perror("braille-cli: open slave");
        return 1;
    }

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 70;
    ws.ws_col = 200;
    ioctl(slave, TIOCSWINSZ, &ws);

    pid_t drainer = fork();
    if (drainer < 0) {
        perror("braille-cli: fork");
        return 1;
    }
    if (drainer == 0) {
        close(slave);
        char buf[65536];
        for (;;) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            tty_write_all(STDOUT_FILENO, buf, (size_t)n);
        }
        close(master);
        _exit(0);
    }
    close(master);

    if (term_enter(slave) != 0) {
        perror("braille-cli: term_enter");
        return 1;
    }

    TermScreen scr;
    term_screen_init(&scr);
    tty_selftest_draw(&scr, 200, 70, slave);
    tty_write_marker(slave);

    if (is_resize) {
        term_resize_pending(); /* install the handler, discard any stray flag */

        ws.ws_row = 24;
        ws.ws_col = 80;
        ioctl(slave, TIOCSWINSZ, &ws);
        raise(SIGWINCH);

        if (term_resize_pending()) {
            int new_cols, new_rows;
            term_get_size(slave, &new_cols, &new_rows);
            term_clear(slave);
            term_screen_reset(&scr);
            tty_selftest_draw(&scr, new_cols, new_rows, slave);
        }
        tty_write_marker(slave);
    }

    term_leave();

    struct termios attrs;
    int have_attrs = (tcgetattr(slave, &attrs) == 0);

    term_screen_free(&scr);
    close(slave);
    waitpid(drainer, NULL, 0);

    if (have_attrs) {
        fprintf(stderr, "ICANON=%d\n", (attrs.c_lflag & ICANON) ? 1 : 0);
        fprintf(stderr, "ECHO=%d\n", (attrs.c_lflag & ECHO) ? 1 : 0);
    } else {
        fprintf(stderr, "ICANON=-1\nECHO=-1\n");
    }

    return 0;
}

int main(int argc, char **argv) {
    int invert = 1;
    int color = 0;
    int cols = 0, rows = 0;
    int bench = 0;
    const char *path = NULL;
    const char *layout_term = NULL;
    const char *layout_display = NULL;
    int diff_demo = 0;
    const char *diff_path_a = NULL;
    const char *diff_path_b = NULL;
    int live_demo = 0;
    const char *live_path = NULL;
    int tty_selftest = 0;
    const char *tty_mode = NULL;
    int keys_mode_flag = 0;
    const char *keys_mode_name = NULL;
    int release_ms = 100;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tty-selftest") == 0 && i + 1 < argc) {
            tty_selftest = 1;
            tty_mode = argv[++i];
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            keys_mode_flag = 1;
            keys_mode_name = argv[++i];
        } else if (strcmp(argv[i], "--release-ms") == 0 && i + 1 < argc) {
            release_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-invert") == 0) {
            invert = 0;
        } else if (strcmp(argv[i], "--color") == 0) {
            color = 1;
        } else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) {
            cols = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            rows = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            bench = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layout_term = argv[++i];
        } else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            layout_display = argv[++i];
        } else if (strcmp(argv[i], "--diff-demo") == 0 && i + 2 < argc) {
            diff_demo = 1;
            diff_path_a = argv[++i];
            diff_path_b = argv[++i];
        } else if (strcmp(argv[i], "--live-demo") == 0 && i + 1 < argc) {
            live_demo = 1;
            live_path = argv[++i];
        } else if (argv[i][0] != '-') {
            path = argv[i];
        } else {
            print_usage();
            return 1;
        }
    }

    if (tty_selftest) {
        if (!tty_mode || (strcmp(tty_mode, "exit") != 0 && strcmp(tty_mode, "resize") != 0)) {
            print_usage();
            return 1;
        }
        return run_tty_selftest(tty_mode);
    }

    if (keys_mode_flag) {
        InputMode mode;
        if (strcmp(keys_mode_name, "legacy") == 0) mode = INPUT_MODE_LEGACY;
        else if (strcmp(keys_mode_name, "kitty") == 0) mode = INPUT_MODE_KITTY;
        else {
            print_usage();
            return 1;
        }
        return run_keys_mode(mode, release_ms);
    }

    if (!live_demo && !diff_demo && (layout_term || layout_display)) {
        int term_cols, term_rows, disp_w, disp_h;
        if (!layout_term || !layout_display ||
            !parse_dims(layout_term, &term_cols, &term_rows) ||
            !parse_dims(layout_display, &disp_w, &disp_h)) {
            print_usage();
            return 1;
        }
        BrailleLayout layout = braille_layout(term_cols, term_rows, disp_w, disp_h);
        printf("%d %d %d %d %g\n", layout.cols, layout.rows, layout.off_col, layout.off_row, layout.scale);
        return 0;
    }

    if (diff_demo) {
        if (cols <= 0 || rows <= 0) {
            print_usage();
            return 1;
        }

        Image *a = read_pnm(diff_path_a);
        Image *b = read_pnm(diff_path_b);
        if (!a || !b) return 1;

        int dst_w = cols * 2, dst_h = rows * 4;

        BrailleRGB *pxa = a->px, *resampled_a = NULL;
        if (dst_w != a->w || dst_h != a->h) {
            resampled_a = braille_resample(a->px, a->w, a->h, dst_w, dst_h);
            pxa = resampled_a;
        }
        BrailleRGB *pxb = b->px, *resampled_b = NULL;
        if (dst_w != b->w || dst_h != b->h) {
            resampled_b = braille_resample(b->px, b->w, b->h, dst_w, dst_h);
            pxb = resampled_b;
        }

        TermCell *cells_a = term_cells_from_pixels(pxa, dst_w, dst_h, invert, color);
        TermCell *cells_b = term_cells_from_pixels(pxb, dst_w, dst_h, invert, color);

        TermScreen scr;
        term_screen_init(&scr);

        ssize_t n1 = term_draw(&scr, cells_a, cols, rows, 1, 1, STDOUT_FILENO);
        fflush(stdout);
        fprintf(stderr, "%zd\n", n1);
        fflush(stderr);

        ssize_t n2 = term_draw(&scr, cells_b, cols, rows, 1, 1, STDOUT_FILENO);
        fflush(stdout);
        fprintf(stderr, "%zd\n", n2);

        term_screen_free(&scr);
        free(cells_a);
        free(cells_b);
        free(resampled_a);
        free(resampled_b);
        free(a->px);
        free(a);
        free(b->px);
        free(b);
        return 0;
    }

    if (live_demo) {
        int disp_w, disp_h;
        if (!layout_display || !parse_dims(layout_display, &disp_w, &disp_h)) {
            print_usage();
            return 1;
        }

        Image *img = read_pnm(live_path);
        if (!img) return 1;

        if (term_enter(STDOUT_FILENO) != 0) {
            fprintf(stderr, "braille-cli: failed to enter raw mode: %s\n", strerror(errno));
            free(img->px);
            free(img);
            return 1;
        }

        TermScreen scr;
        term_screen_init(&scr);

        int term_cols, term_rows;
        if (term_get_size(STDOUT_FILENO, &term_cols, &term_rows) != 0) {
            term_cols = 80;
            term_rows = 24;
        }

        int should_quit = 0;
        while (!should_quit) {
            BrailleLayout layout = braille_layout(term_cols, term_rows, disp_w, disp_h);

            BrailleRGB *px = img->px;
            int px_w = img->w, px_h = img->h;
            BrailleRGB *resampled = NULL;
            if (layout.px_w != px_w || layout.px_h != px_h) {
                resampled = braille_resample(img->px, img->w, img->h, layout.px_w, layout.px_h);
                px = resampled;
                px_w = layout.px_w;
                px_h = layout.px_h;
            }

            TermCell *cells = term_cells_from_pixels(px, px_w, px_h, invert, color);
            term_draw(&scr, cells, layout.cols, layout.rows, layout.off_col, layout.off_row, STDOUT_FILENO);
            free(cells);
            free(resampled);

            for (;;) {
                if (term_resize_pending()) {
                    if (term_get_size(STDOUT_FILENO, &term_cols, &term_rows) == 0) {
                        term_clear(STDOUT_FILENO);
                        term_screen_reset(&scr);
                        break;
                    }
                }

                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(STDIN_FILENO, &fds);
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 50000;
                int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
                if (r > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
                    char c;
                    ssize_t n = read(STDIN_FILENO, &c, 1);
                    if (n <= 0 || c == 'q') {
                        should_quit = 1;
                        break;
                    }
                } else if (r < 0 && errno != EINTR) {
                    should_quit = 1;
                    break;
                }
            }
        }

        term_screen_free(&scr);
        term_leave();
        free(img->px);
        free(img);
        return 0;
    }

    if (!path) {
        print_usage();
        return 1;
    }

    Image *img = read_pnm(path);
    if (!img) return 1;

    BrailleRGB *px = img->px;
    int px_w = img->w, px_h = img->h;
    BrailleRGB *resampled = NULL;

    if (cols > 0 && rows > 0) {
        int dst_w = cols * 2, dst_h = rows * 4;
        if (dst_w != px_w || dst_h != px_h) {
            resampled = braille_resample(px, px_w, px_h, dst_w, dst_h);
            px = resampled;
            px_w = dst_w;
            px_h = dst_h;
        }
    }

    if (bench > 0) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < bench; i++) {
            size_t len;
            char *out = braille_render(px, px_w, px_h, invert, color, &len);
            free(out);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        printf("%f\n", elapsed_ms / bench);
    } else {
        size_t len;
        char *out = braille_render(px, px_w, px_h, invert, color, &len);
        fwrite(out, 1, len, stdout);
        free(out);
    }

    free(resampled);
    free(img->px);
    free(img);
    return 0;
}
