#include "gest.h"
#include <string.h>

/* evdev constants, spelled out so the host build needs no <linux/input.h> */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define SYN_REPORT 0
#define SYN_MT_REPORT 2
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define KEY_POWER 116
#define KEY_CAMERA 212

void gest_init(gest_t *g) { memset(g, 0, sizeof *g); }

const char *gest_name(gest_kind k) {
    static const char *const names[] = {"none", "tap", "tap2", "swipe.fwd", "swipe.back", "swipe.down",
                                        "swipe.up", "long", "hold.start", "hold.end", "key.camera", "key.power"};
    return (unsigned)k < sizeof names / sizeof *names ? names[k] : "?";
}

static int emit(gest_event *out, gest_kind k, const gest_t *g, int64_t now_ms) {
    out->kind = k;
    out->dx = g->x1 - g->x0;
    out->dy = g->y1 - g->y0;
    out->fingers = g->max_fingers;
    out->held_ms = g->down ? now_ms - g->t0 : 0;
    return 1;
}

static int iabs(int v) { return v < 0 ? -v : v; }

int gest_feed(gest_t *g, uint16_t type, uint16_t code, int32_t value, int64_t now_ms, gest_event *out) {
    if (type == EV_KEY) {
        if (value != 0) return 0;  /* act on release, as glass-os does */
        if (code == KEY_CAMERA) { memset(out, 0, sizeof *out); out->kind = GEST_KEY_CAMERA; return 1; }
        if (code == KEY_POWER) { memset(out, 0, sizeof *out); out->kind = GEST_KEY_POWER; return 1; }
        return 0;
    }
    if (type == EV_ABS) {
        if (code == ABS_MT_POSITION_X || code == ABS_X) { g->cur_x = value; g->seen_pos = 1; }
        else if (code == ABS_MT_POSITION_Y || code == ABS_Y) { g->cur_y = value; g->seen_pos = 1; }
        return 0;
    }
    if (type != EV_SYN) return 0;
    if (code == SYN_MT_REPORT) {
        if (g->seen_pos) { g->frame_contacts++; g->seen_pos = 0; }
        return 0;
    }
    if (code != SYN_REPORT) return 0;

    /* A single-touch frame (ABS_X without SYN_MT_REPORT) still counts as a contact. */
    if (g->seen_pos) { g->frame_contacts++; g->seen_pos = 0; }
    int r = 0;
    if (g->frame_contacts > 0) {
        if (!g->down) {
            g->down = 1;
            g->x0 = g->x1 = g->cur_x;
            g->y0 = g->y1 = g->cur_y;
            g->t0 = now_ms;
            g->max_fingers = g->frame_contacts;
            g->hold_sent = 0;
        } else {
            g->x1 = g->cur_x;
            g->y1 = g->cur_y;
            if (g->frame_contacts > g->max_fingers) g->max_fingers = g->frame_contacts;
        }
    } else if (g->down) {
        int dx = g->x1 - g->x0, dy = g->y1 - g->y0;
        int adx = iabs(dx), ady = iabs(dy);
        int64_t dt = now_ms - g->t0;
        gest_event e;
        memset(&e, 0, sizeof e);
        r = emit(&e, GEST_NONE, g, now_ms);
        e.held_ms = dt;
        if (g->hold_sent) e.kind = GEST_HOLD_END;
        else if (adx >= 150 && adx >= ady) e.kind = dx > 0 ? GEST_SWIPE_FWD : GEST_SWIPE_BACK;
        else if (ady >= GEST_SWIPE_Y_PX && ady > adx) e.kind = dy > 0 ? GEST_SWIPE_DOWN : GEST_SWIPE_UP;
        else if (dt < GEST_TAP_MS) e.kind = g->max_fingers >= 2 ? GEST_TAP2 : GEST_TAP;
        else if (dt >= GEST_LONG_MS) e.kind = GEST_LONG;
        else e.kind = g->max_fingers >= 2 ? GEST_TAP2 : GEST_TAP;
        g->down = 0;
        g->hold_sent = 0;
        if (e.kind == GEST_NONE) r = 0;
        else *out = e;
    }
    g->frame_contacts = 0;
    g->seen_pos = 0;
    return r;
}

int gest_tick(gest_t *g, int64_t now_ms, gest_event *out) {
    if (!g->down || g->hold_sent) return 0;
    if (now_ms - g->t0 < GEST_HOLD_MS) return 0;
    if (iabs(g->x1 - g->x0) >= GEST_JITTER_PX || iabs(g->y1 - g->y0) >= GEST_JITTER_PX) return 0;
    g->hold_sent = 1;
    return emit(out, GEST_HOLD_START, g, now_ms);
}
