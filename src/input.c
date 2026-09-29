#include "input.h"

#include <string.h>

#define INPUT_CODE_NONE 0
#define INPUT_CODE_HANDLED (-1)

static void push_event(InputDecoder *d, InputEventKind kind, int code) {
    if (d->queue_len >= INPUT_MAX_EVENTS) return; /* drop on overflow */
    int idx = (d->queue_head + d->queue_len) % INPUT_MAX_EVENTS;
    d->queue[idx].kind = kind;
    d->queue[idx].code = code;
    d->queue_len++;
}

static InputHeldKey *find_or_alloc_held(InputDecoder *d, int code) {
    int free_idx = -1;
    for (int i = 0; i < INPUT_MAX_HELD; i++) {
        if (d->held[i].active && d->held[i].code == code) return &d->held[i];
        if (free_idx < 0 && !d->held[i].active) free_idx = i;
    }
    if (free_idx < 0) return NULL; /* held table full: drop the event */
    return &d->held[free_idx];
}

/* Legacy keys are held (and later released by input_tick's timeout) since
 * the terminal never reports a release directly - see AC-022/AC-023. */
static void legacy_key_event(InputDecoder *d, int code, long now_ms) {
    InputHeldKey *slot = find_or_alloc_held(d, code);
    if (!slot) return;
    if (!slot->active) {
        slot->code = code;
        slot->active = 1;
        push_event(d, INPUT_KEY_DOWN, code);
    }
    slot->last_seen_ms = now_ms;
}

/* Kitty presses/releases are explicit (event type 1/3), so they bypass the
 * held table entirely. */
static void kitty_key_event(InputDecoder *d, int code, int is_down) {
    push_event(d, is_down ? INPUT_KEY_DOWN : INPUT_KEY_UP, code);
}

static void legacy_handle_alt(InputDecoder *d, uint8_t b, long now_ms) {
    (void)now_ms;
    if (b == 'c' || b == 'C') push_event(d, INPUT_TOGGLE_COLOR, 0);
    /* Any other Alt+letter combination is outside the controls table. */
}

static void legacy_handle_byte(InputDecoder *d, uint8_t b, long now_ms) {
    if (b == 0x03) { /* Ctrl+C */
        push_event(d, INPUT_QUIT, 0);
        return;
    }
    if (b >= 'A' && b <= 'Z') { /* uppercase = Shift + the lowercase letter */
        legacy_key_event(d, INPUT_KEY_RSHIFT, now_ms);
        legacy_key_event(d, (int)(b - 'A' + 'a'), now_ms);
        return;
    }
    if (b == '\r') { legacy_key_event(d, INPUT_KEY_ENTER, now_ms); return; }
    if (b == '\t') { legacy_key_event(d, INPUT_KEY_TAB, now_ms); return; }
    if (b == 0x7f || b == 0x08) { legacy_key_event(d, INPUT_KEY_BACKSPACE, now_ms); return; }
    if (b >= 0x20 && b < 0x7f) { /* space, digits, lowercase letters, punctuation */
        legacy_key_event(d, (int)b, now_ms);
        return;
    }
    /* Other control bytes are outside the controls table: ignored. */
}

static void ss3_final(InputDecoder *d, uint8_t final, long now_ms) {
    int code;
    switch (final) {
        case 'A': code = INPUT_KEY_UPARROW; break;
        case 'B': code = INPUT_KEY_DOWNARROW; break;
        case 'C': code = INPUT_KEY_RIGHTARROW; break;
        case 'D': code = INPUT_KEY_LEFTARROW; break;
        case 'P': code = INPUT_KEY_F1; break;
        case 'Q': code = INPUT_KEY_F2; break;
        case 'R': code = INPUT_KEY_F3; break;
        case 'S': code = INPUT_KEY_F4; break;
        default: return;
    }
    legacy_key_event(d, code, now_ms);
}

static int fkey_from_number(int num) {
    switch (num) {
        case 15: return INPUT_KEY_F5;
        case 17: return INPUT_KEY_F6;
        case 18: return INPUT_KEY_F7;
        case 19: return INPUT_KEY_F8;
        case 20: return INPUT_KEY_F9;
        case 21: return INPUT_KEY_F10;
        case 23: return INPUT_KEY_F11;
        case 24: return INPUT_KEY_F12;
        default: return INPUT_CODE_NONE;
    }
}

/* CSI ... u: general codepoint report (kitty's "report all keys as escape
 * codes"). mods follows the kitty encoding (raw value - 1 = bitmask, bit0
 * shift, bit1 alt, bit2 ctrl); event 1/2/3 = press/repeat/release, default
 * press when absent. Alt+C/Ctrl+C are one-shot actions, not held
 * keys, so they are pushed directly here instead of returning a code. */
static int codepoint_to_code(InputDecoder *d, int cp, int mods, int event) {
    int ev = (event < 0) ? 1 : event;
    int bits = (mods < 0) ? 0 : (mods - 1);
    int alt = bits & 0x2;
    int ctrl = bits & 0x4;

    if (ev == 1) {
        if (alt && (cp == 'c' || cp == 'C')) { push_event(d, INPUT_TOGGLE_COLOR, 0); return INPUT_CODE_HANDLED; }
        if (ctrl && (cp == 'c' || cp == 'C')) { push_event(d, INPUT_QUIT, 0); return INPUT_CODE_HANDLED; }
    }

    if (cp == 57441 || cp == 57447) return INPUT_KEY_RSHIFT; /* left/right Shift */
    if (cp == 57442 || cp == 57448) return INPUT_KEY_RCTRL;  /* left/right Ctrl */
    if (cp == 13) return INPUT_KEY_ENTER;
    if (cp == 9) return INPUT_KEY_TAB;
    if (cp == 27) return INPUT_KEY_ESCAPE;
    if (cp == 127) return INPUT_KEY_BACKSPACE;
    if (cp >= 32 && cp < 127) return cp; /* letters, digits, space, punctuation unchanged */
    return INPUT_CODE_NONE;
}

static void csi_final(InputDecoder *d, uint8_t final, long now_ms) {
    int num = d->csi_num;
    int mods = d->csi_mods;
    int event = d->csi_event;
    int code;

    switch (final) {
        case 'A': code = INPUT_KEY_UPARROW; break;
        case 'B': code = INPUT_KEY_DOWNARROW; break;
        case 'C': code = INPUT_KEY_RIGHTARROW; break;
        case 'D': code = INPUT_KEY_LEFTARROW; break;
        case 'P': code = INPUT_KEY_F1; break;
        case 'Q': code = INPUT_KEY_F2; break;
        case 'R': code = INPUT_KEY_F3; break;
        case 'S': code = INPUT_KEY_F4; break;
        case '~': code = fkey_from_number(num); break;
        case 'u': {
            int r = codepoint_to_code(d, num, mods, event);
            if (r == INPUT_CODE_HANDLED) return;
            code = r;
            break;
        }
        default: return; /* Home/End and anything else outside the controls table */
    }
    if (code <= 0) return;

    if (d->mode == INPUT_MODE_KITTY) {
        int ev = (event < 0) ? 1 : event;
        if (ev == 1) kitty_key_event(d, code, 1);
        else if (ev == 3) kitty_key_event(d, code, 0);
        /* ev == 2 (repeat): the key is already down, nothing to emit. */
    } else {
        legacy_key_event(d, code, now_ms);
    }
}

void input_init(InputDecoder *d, InputMode mode, int release_ms) {
    memset(d, 0, sizeof(*d));
    d->mode = mode;
    d->release_ms = release_ms;
    d->state = INPUT_ST_IDLE;
    d->csi_num = -1;
    d->csi_mods = -1;
    d->csi_event = -1;
}

void input_feed(InputDecoder *d, const uint8_t *bytes, size_t n, long now_ms) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        switch (d->state) {
            case INPUT_ST_IDLE:
                if (b == 0x1b) {
                    d->state = INPUT_ST_ESC;
                    d->esc_start_ms = now_ms;
                } else {
                    legacy_handle_byte(d, b, now_ms);
                }
                break;

            case INPUT_ST_ESC:
                if (b == '[') {
                    d->state = INPUT_ST_CSI;
                    d->csi_num = -1;
                    d->csi_mods = -1;
                    d->csi_event = -1;
                } else if (b == 'O') {
                    d->state = INPUT_ST_SS3;
                } else {
                    d->state = INPUT_ST_IDLE;
                    legacy_handle_alt(d, b, now_ms);
                }
                break;

            case INPUT_ST_CSI:
                if (b >= '0' && b <= '9') {
                    if (d->csi_num < 0) d->csi_num = 0;
                    d->csi_num = d->csi_num * 10 + (b - '0');
                } else if (b == ';') {
                    d->state = INPUT_ST_CSI_MODS;
                } else {
                    csi_final(d, b, now_ms);
                    d->state = INPUT_ST_IDLE;
                }
                break;

            case INPUT_ST_CSI_MODS:
                if (b >= '0' && b <= '9') {
                    if (d->csi_mods < 0) d->csi_mods = 0;
                    d->csi_mods = d->csi_mods * 10 + (b - '0');
                } else if (b == ':') {
                    d->state = INPUT_ST_CSI_EVENT;
                } else {
                    csi_final(d, b, now_ms);
                    d->state = INPUT_ST_IDLE;
                }
                break;

            case INPUT_ST_CSI_EVENT:
                if (b >= '0' && b <= '9') {
                    if (d->csi_event < 0) d->csi_event = 0;
                    d->csi_event = d->csi_event * 10 + (b - '0');
                } else {
                    csi_final(d, b, now_ms);
                    d->state = INPUT_ST_IDLE;
                }
                break;

            case INPUT_ST_SS3:
                d->state = INPUT_ST_IDLE;
                ss3_final(d, b, now_ms);
                break;
        }
    }
}

void input_tick(InputDecoder *d, long now_ms) {
    if (d->mode != INPUT_MODE_LEGACY) return;

    if (d->state == INPUT_ST_ESC && now_ms - d->esc_start_ms >= 25) {
        d->state = INPUT_ST_IDLE;
        legacy_key_event(d, INPUT_KEY_ESCAPE, d->esc_start_ms);
    }

    for (int i = 0; i < INPUT_MAX_HELD; i++) {
        if (d->held[i].active && now_ms - d->held[i].last_seen_ms >= d->release_ms) {
            push_event(d, INPUT_KEY_UP, d->held[i].code);
            d->held[i].active = 0;
        }
    }
}

int input_pop(InputDecoder *d, InputEvent *out) {
    if (d->queue_len == 0) return 0;
    *out = d->queue[d->queue_head];
    d->queue_head = (d->queue_head + 1) % INPUT_MAX_EVENTS;
    d->queue_len--;
    return 1;
}
