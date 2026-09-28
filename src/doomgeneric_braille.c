#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "braille.h"
#include "input.h"
#include "layout.h"
#include "term.h"

#include "doomgeneric.h"
#include "doomtype.h"
#include "i_joystick.h"
#include "i_system.h"
#include "m_controls.h"

/*
 * The engine's frame buffer is pinned to 320x200 by the Makefile's engine
 * build flags (-DDOOMGENERIC_RESX/RESY, spec.md's normative resolution).
 * This file is compiled by the plain src/%.c rule, without those defines,
 * so DOOMGENERIC_RESX/RESY would silently fall back to doomgeneric.h's
 * 640x400 header default; use our own constants instead of those macros.
 */
#define GAME_RESX 320
#define GAME_RESY 200
#define GAME_DISPLAY_H 240 /* 4:3 stretch: 200 * 1.2, see spec.md step 3 */

/*
 * The Makefile excludes i_joystick.c (it's the SDL joystick backend, and
 * we don't link SDL). Gameplay input is out of scope for this phase
 * (spec.md "Out of scope"), so these are no-op stand-ins for the two
 * symbols d_main.c calls unconditionally.
 */
void I_InitJoystick(void) {}
void I_BindJoystickVariables(void) {}

/* Neither header exports these globals (m_menu.c / am_map.c define them
 * with no matching extern in m_menu.h / am_map.h), so DG_GetKey's
 * menu-aware arrow mapping (spec.md "How the bindings reach the engine")
 * declares them itself instead of editing doomgeneric sources. */
extern boolean menuactive;
extern boolean automapactive;

static TermScreen g_screen;
static int g_term_cols = 80;
static int g_term_rows = 24;
static int g_color;
static int g_invert;

/* The real terminal. main() moves it off fd 1/2 before the engine starts,
 * because the engine printf()s its startup log (and I_Error messages) to
 * stdout/stderr; left there, that text lands on the raw-mode game screen.
 * The engine's output goes to g_log_fd instead and is replayed to the
 * terminal on exit, once it is back in normal mode. */
static int g_tty_fd = STDOUT_FILENO;
static int g_log_fd = -1;
static int g_err_fd = -1;

/* Glyph color while color mode is off. Always written explicitly, so the
 * picture looks the same before and after an Alt+C round trip (otherwise the
 * last truecolor escape from color mode would stay active). MONO_COLOR=RRGGBB
 * overrides it. */
static uint8_t g_mono_r = 0xD0, g_mono_g = 0xD0, g_mono_b = 0xD0;

static InputDecoder g_input;
static int g_kitty_mode;

/* Small FIFO between the decoder (drained once per frame in poll_input())
 * and DG_GetKey (drained one event at a time per tic by the engine's own
 * I_GetEvent loop). */
#define KEY_QUEUE_CAP 64
typedef struct {
    int pressed;
    unsigned char key;
} PendingKey;
static PendingKey g_key_queue[KEY_QUEUE_CAP];
static int g_key_queue_head, g_key_queue_len;

/* Remembers the engine key code actually sent for the up/down arrow's
 * press, so a later release uses the same code even if menuactive or
 * automapactive changes while the key is held (spec.md's "a release
 * always uses the same code as its press"). -1 means "not held". */
static int g_up_arrow_sent = -1;
static int g_down_arrow_sent = -1;

static int env_bool(const char *name, int def) {
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return atoi(v) != 0;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return atoi(v);
}

static void parse_mono_color(void) {
    const char *v = getenv("MONO_COLOR");
    if (!v || !v[0]) return;
    if (v[0] == '#') v++;
    char *end;
    unsigned long rgb = strtoul(v, &end, 16);
    if (end - v != 6 || *end) {
        fprintf(stderr, "braille-game: ignoring MONO_COLOR=%s (expected RRGGBB hex)\n", getenv("MONO_COLOR"));
        return;
    }
    g_mono_r = (uint8_t)(rgb >> 16);
    g_mono_g = (uint8_t)(rgb >> 8);
    g_mono_b = (uint8_t)rgb;
}

/* atexit, registered before term_enter()'s own handler, so it runs after the
 * terminal has been restored. */
static void replay_engine_log(void) {
    fflush(stdout);
    fflush(stderr);
    if (g_log_fd < 0 || g_err_fd < 0) return;
    if (lseek(g_log_fd, 0, SEEK_SET) != 0) return;
    char buf[4096];
    ssize_t n;
    while ((n = read(g_log_fd, buf, sizeof(buf))) > 0) {
        ssize_t unused = write(g_err_fd, buf, (size_t)n);
        (void)unused;
    }
}

/* Keeps the terminal on a private fd and points fd 1/2 at an unlinked
 * temp file. On any failure the engine simply keeps writing to the terminal. */
static void redirect_engine_output(void) {
    if (!isatty(STDOUT_FILENO)) return;
    char path[] = "/tmp/doom-braille-log-XXXXXX";
    int log_fd = mkstemp(path);
    if (log_fd < 0) return;
    unlink(path);

    int tty_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    int err_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    if (tty_fd < 0 || err_fd < 0) {
        if (tty_fd >= 0) close(tty_fd);
        if (err_fd >= 0) close(err_fd);
        close(log_fd);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);

    g_tty_fd = tty_fd;
    g_err_fd = err_fd;
    g_log_fd = log_fd;
    atexit(replay_engine_log);
}

static void push_key_event(int pressed, unsigned char key) {
    if (g_key_queue_len >= KEY_QUEUE_CAP) return;
    int idx = (g_key_queue_head + g_key_queue_len) % KEY_QUEUE_CAP;
    g_key_queue[idx].pressed = pressed;
    g_key_queue[idx].key = key;
    g_key_queue_len++;
}

/*
 * Registered with the engine's own I_AtExit (i_system.h), not a doomgeneric
 * edit: both I_Quit() (menu quit, F10+y) and I_Error() (the demo-recorded
 * message G_CheckDemoStatus raises once key_demo_quit - 'q' - is seen)
 * call every I_AtExit entry before this backend otherwise gets a chance to
 * act. This doomgeneric port compiles out I_Quit's own exit(0), and
 * I_Error always exits -1; without this hook neither path would end the
 * process with the terminal restored and a zero status (AC-011, AC-026).
 * Registered with run_on_error=1 so it also fires from the I_Error path.
 */
static void shutdown_hook(void) {
    exit(0);
}

/* Handles one decoded key/toggle/quit event: Alt+C/Alt+I/Ctrl+C locally,
 * everything else queued for DG_GetKey. See spec.md's "How the bindings
 * reach the engine". */
static void handle_input_event(const InputEvent *ev) {
    switch (ev->kind) {
        case INPUT_TOGGLE_COLOR:
            g_color = !g_color;
            return;
        case INPUT_TOGGLE_INVERT:
            g_invert = !g_invert;
            return;
        case INPUT_QUIT:
            exit(0);
            return;
        case INPUT_KEY_DOWN:
        case INPUT_KEY_UP: {
            int pressed = ev->kind == INPUT_KEY_DOWN;
            int code = ev->code;

            if (code == INPUT_KEY_RCTRL && g_kitty_mode) {
                /* "Ctrl fires too, in kitty mode only" - merge onto Space's
                 * own code so it sets the same gamekeydown slot as key_fire. */
                code = ' ';
            } else if (code == INPUT_KEY_UPARROW || code == INPUT_KEY_DOWNARROW) {
                int *sent = code == INPUT_KEY_UPARROW ? &g_up_arrow_sent : &g_down_arrow_sent;
                if (pressed) {
                    if (*sent == -1) {
                        int navigating = menuactive || automapactive;
                        *sent = navigating ? code : (code == INPUT_KEY_UPARROW ? 'w' : 's');
                    }
                    code = *sent;
                } else if (*sent != -1) {
                    code = *sent;
                    *sent = -1;
                }
            }

            push_key_event(pressed, (unsigned char)code);
            return;
        }
    }
}

/* Reads whatever stdin has buffered (never blocks), feeds it to the
 * decoder, ticks it and drains its queue. Called once per frame. */
static void poll_input(void) {
    long now_ms = (long)DG_GetTicksMs();

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) break;

        uint8_t buf[256];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        input_feed(&g_input, buf, (size_t)n, now_ms);
    }

    input_tick(&g_input, now_ms);

    InputEvent ev;
    while (input_pop(&g_input, &ev)) handle_input_event(&ev);
}

void DG_Init(void) {
    g_color = env_bool("COLOR", 0);
    g_invert = env_bool("INVERT", 1);
    parse_mono_color();
    const char *curve = getenv("CURVE");
    if (curve && strcmp(curve, "pow2") == 0) braille_curve = BRAILLE_CURVE_POW2;

    if (term_enter(g_tty_fd) != 0) {
        fprintf(stderr, "braille-game: failed to enter raw mode: %s\n", strerror(errno));
        exit(1);
    }
    term_screen_init(&g_screen);

    if (term_get_size(g_tty_fd, &g_term_cols, &g_term_rows) != 0) {
        g_term_cols = 80;
        g_term_rows = 24;
    }

    int release_ms = env_int("KEY_RELEASE_MS", 100);
    g_kitty_mode = term_detect_kitty(STDIN_FILENO, g_tty_fd, 200);
    if (g_kitty_mode) term_push_kitty(g_tty_fd);
    input_init(&g_input, g_kitty_mode ? INPUT_MODE_KITTY : INPUT_MODE_LEGACY, release_ms);

    char pending[256];
    size_t n = term_take_pending(pending, sizeof(pending));
    if (n > 0) input_feed(&g_input, (const uint8_t *)pending, n, (long)DG_GetTicksMs());

    I_AtExit(shutdown_hook, true);
}

void DG_DrawFrame(void) {
    poll_input();

    if (term_resize_pending()) {
        if (term_get_size(g_tty_fd, &g_term_cols, &g_term_rows) == 0) {
            term_clear(g_tty_fd);
            term_screen_reset(&g_screen);
        }
    }

    static BrailleRGB src[GAME_RESX * GAME_RESY];
    for (int i = 0; i < GAME_RESX * GAME_RESY; i++) {
        uint32_t p = (uint32_t)DG_ScreenBuffer[i];
        src[i].r = (uint8_t)(p >> 16);
        src[i].g = (uint8_t)(p >> 8);
        src[i].b = (uint8_t)p;
    }

    BrailleLayout layout = braille_layout(g_term_cols, g_term_rows, GAME_RESX, GAME_DISPLAY_H);

    BrailleRGB *resampled = braille_resample(src, GAME_RESX, GAME_RESY, layout.px_w, layout.px_h);
    TermCell *cells = term_cells_from_pixels(resampled, layout.px_w, layout.px_h, g_invert, g_color);
    free(resampled);
    if (!g_color) {
        for (int i = 0; i < layout.cols * layout.rows; i++) {
            cells[i].has_color = 1;
            cells[i].r = g_mono_r;
            cells[i].g = g_mono_g;
            cells[i].b = g_mono_b;
        }
    }

    term_draw(&g_screen, cells, layout.cols, layout.rows, layout.off_col, layout.off_row, g_tty_fd);
    free(cells);
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    static struct timespec start;
    static int have_start = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!have_start) {
        start = now;
        have_start = 1;
    }
    return (uint32_t)((now.tv_sec - start.tv_sec) * 1000 +
                       (now.tv_nsec - start.tv_nsec) / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (g_key_queue_len == 0) return 0;
    *pressed = g_key_queue[g_key_queue_head].pressed;
    *key = g_key_queue[g_key_queue_head].key;
    g_key_queue_head = (g_key_queue_head + 1) % KEY_QUEUE_CAP;
    g_key_queue_len--;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    char buf[300];
    int n = snprintf(buf, sizeof(buf), "\x1b]0;%s\x07", title);
    if (n > 0) {
        ssize_t unused = write(g_tty_fd, buf, (size_t)n);
        (void)unused;
    }
}

int main(int argc, char **argv) {
    redirect_engine_output();
    doomgeneric_Create(argc, argv);

    /* Set once the engine's own config is loaded, so its own default.cfg
     * can't overwrite them again (spec.md's "How the bindings reach the
     * engine"); doomgeneric sources are not edited. */
    key_up = 'w';
    key_down = 's';
    key_strafeleft = 'a';
    key_straferight = 'd';
    key_fire = ' ';
    key_use = 'e';

    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
