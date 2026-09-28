#ifndef INPUT_H
#define INPUT_H

#include <stddef.h>
#include <stdint.h>

/* Pure decoder: no I/O, no clock of its own. Callers feed raw bytes plus a
 * timestamp (input_feed), tick it with the current time so legacy releases
 * and the lone-Esc grace period can fire (input_tick), then drain decoded
 * events (input_pop). See spec.md's "Kitty detection" and "Controls" tables
 * and tasks.md T-006 for the encodings this implements. */

typedef enum {
    INPUT_MODE_LEGACY = 0,
    INPUT_MODE_KITTY = 1,
} InputMode;

typedef enum {
    INPUT_KEY_DOWN,
    INPUT_KEY_UP,
    INPUT_TOGGLE_COLOR,
    INPUT_TOGGLE_INVERT,
    INPUT_QUIT,
} InputEventKind;

typedef struct {
    InputEventKind kind;
    int code; /* engine key code; unused for TOGGLE_COLOR/TOGGLE_INVERT/QUIT */
} InputEvent;

/*
 * Engine key codes the decoder can emit. These mirror doomgeneric's
 * doomkeys.h values (numerically) so T-008 can hand them to the engine
 * unchanged, but this module does not include that header: third_party/
 * doomgeneric is not a build dependency of the decoder itself. Letters
 * (a-z) and digits (0-9) are passed through as their own ASCII codes and
 * need no constant here.
 */
#define INPUT_KEY_RIGHTARROW 0xae
#define INPUT_KEY_LEFTARROW  0xac
#define INPUT_KEY_UPARROW    0xad
#define INPUT_KEY_DOWNARROW  0xaf
#define INPUT_KEY_ESCAPE     27
#define INPUT_KEY_ENTER      13
#define INPUT_KEY_TAB        9
#define INPUT_KEY_BACKSPACE  0x7f
#define INPUT_KEY_MINUS      0x2d
#define INPUT_KEY_EQUALS     0x3d
#define INPUT_KEY_RSHIFT     (0x80 + 0x36)
#define INPUT_KEY_RCTRL      (0x80 + 0x1d)
#define INPUT_KEY_F1  (0x80 + 0x3b)
#define INPUT_KEY_F2  (0x80 + 0x3c)
#define INPUT_KEY_F3  (0x80 + 0x3d)
#define INPUT_KEY_F4  (0x80 + 0x3e)
#define INPUT_KEY_F5  (0x80 + 0x3f)
#define INPUT_KEY_F6  (0x80 + 0x40)
#define INPUT_KEY_F7  (0x80 + 0x41)
#define INPUT_KEY_F8  (0x80 + 0x42)
#define INPUT_KEY_F9  (0x80 + 0x43)
#define INPUT_KEY_F10 (0x80 + 0x44)
#define INPUT_KEY_F11 (0x80 + 0x57)
#define INPUT_KEY_F12 (0x80 + 0x58)

#define INPUT_MAX_EVENTS 64
#define INPUT_MAX_HELD 48

typedef struct {
    int code;
    int active;
    long last_seen_ms;
} InputHeldKey;

typedef enum {
    INPUT_ST_IDLE = 0,
    INPUT_ST_ESC,
    INPUT_ST_CSI,
    INPUT_ST_CSI_MODS,
    INPUT_ST_CSI_EVENT,
    INPUT_ST_SS3,
} InputParseState;

typedef struct {
    InputMode mode;
    int release_ms;

    InputParseState state;
    int csi_num;   /* -1 when absent */
    int csi_mods;  /* -1 when absent */
    int csi_event; /* -1 when absent */
    long esc_start_ms;

    InputHeldKey held[INPUT_MAX_HELD];

    InputEvent queue[INPUT_MAX_EVENTS];
    int queue_head, queue_len;
} InputDecoder;

void input_init(InputDecoder *dec, InputMode mode, int release_ms);

/* Decodes bytes arriving at time now_ms. May queue zero or more events. */
void input_feed(InputDecoder *dec, const uint8_t *bytes, size_t n, long now_ms);

/*
 * Advances the decoder's notion of "now" without new bytes: fires legacy
 * releases for keys whose last byte is older than release_ms, and resolves
 * a pending lone Esc once its 25 ms disambiguation grace has elapsed.
 * No-op in kitty mode (releases there are explicit, event type 3).
 */
void input_tick(InputDecoder *dec, long now_ms);

/* Pops the oldest queued event into *out. Returns 1 if one was popped, 0 if
 * the queue was empty. */
int input_pop(InputDecoder *dec, InputEvent *out);

#endif
