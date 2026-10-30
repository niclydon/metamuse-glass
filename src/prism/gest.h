/* prismd: the temple touchpad and the two buttons, decoded into gestures.
 *
 * The Glass touchpad is a Synaptics RMI4 reporting Protocol A multitouch over evdev:
 * ABS_MT_POSITION_X (and _Y) per contact, SYN_MT_REPORT after each contact, SYN_REPORT
 * after each frame; a frame with no contacts is the release. The decoder is a pure
 * state machine fed one evdev event at a time (so it is tested on the host with
 * synthetic events) plus a tick for time-based gestures. Thresholds are written down
 * here, in one place, and the first device session revises them (PLAN-P1 §3).
 *
 * The X thresholds come from glass-os's fb-hello.c, which drove the real touchpad:
 * a swipe is ≥ 180 raw units of travel, a tap is a release inside 550 ms. The Y
 * axis was not read there; its threshold is a guess until measured.
 */
#ifndef PRISM_GEST_H
#define PRISM_GEST_H
#include <stdint.h>

#define GEST_SWIPE_PX   180   /* raw X travel for a forward/back swipe (measured, glass-os) */
#define GEST_SWIPE_Y_PX 120   /* raw Y travel for a down swipe (unmeasured) */
#define GEST_TAP_MS     550   /* release before this with little travel: tap (measured, glass-os) */
#define GEST_LONG_MS    600   /* release after this and before HOLD: long press */
#define GEST_HOLD_MS   1500   /* still down at this: hold begins (capture lane, P4) */
#define GEST_JITTER_PX   40   /* travel below this counts as none */

typedef enum {
    GEST_NONE = 0,
    GEST_TAP,          /* one finger, short */
    GEST_TAP2,         /* two fingers, short */
    GEST_SWIPE_FWD,    /* toward the front of the frame (+X) */
    GEST_SWIPE_BACK,   /* toward the ear (-X) */
    GEST_SWIPE_DOWN,   /* +Y */
    GEST_SWIPE_UP,     /* -Y (unbound; logged) */
    GEST_LONG,         /* released between LONG and HOLD */
    GEST_HOLD_START,   /* down for HOLD_MS; released later as HOLD_END */
    GEST_HOLD_END,
    GEST_KEY_CAMERA,   /* KEY_CAMERA (212) released */
    GEST_KEY_POWER     /* KEY_POWER (116) released */
} gest_kind;

typedef struct {
    gest_kind kind;
    int dx, dy, fingers;
    int64_t held_ms;
} gest_event;

typedef struct {
    int down, seen_pos, frame_contacts, max_fingers, hold_sent;
    int cur_x, cur_y, x0, y0, x1, y1;
    int64_t t0;
} gest_t;

void gest_init(gest_t *g);
/* Feed one evdev event (type, code, value as in struct input_event). Returns 1 and fills
 * *out when a gesture completed on this event, else 0. */
int gest_feed(gest_t *g, uint16_t type, uint16_t code, int32_t value, int64_t now_ms, gest_event *out);
/* Call when idle (every poll timeout): emits HOLD_START once per press. */
int gest_tick(gest_t *g, int64_t now_ms, gest_event *out);
const char *gest_name(gest_kind k);

#endif
