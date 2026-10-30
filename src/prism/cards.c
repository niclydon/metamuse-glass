#include "cards.h"
#include <stdio.h>
#include <string.h>
#include "sprite_mark.h"

#define MARGIN 24
#define USABLE (FB_W - 2 * MARGIN)

/* NexusOS DESIGN §13 tokens (nexusos/shell/src/theme.css): graphite, cerulean, label
 * blue, amber, error red, text. One Prism departure: the background is true black,
 * because on the Glass prism an unlit pixel is see-through and a lit graphite field
 * would hang a grey rectangle in the wearer's eye. */
static uint16_t c_bg, c_text, c_dim, c_accent, c_label, c_amber, c_ok, c_warn, c_bad, c_white;
static void palette(void) {
    c_bg = 0x0000;
    c_white = fb_rgb(0xE9, 0xF2, 0xF7);   /* --nx-text */
    c_text = fb_rgb(0xE9, 0xF2, 0xF7);
    c_label = fb_rgb(0x8A, 0xBA, 0xD3);   /* --nx-label */
    c_dim = fb_rgb(0x6E, 0x86, 0x94);     /* label at ~60% on black */
    c_accent = fb_rgb(0x4A, 0x7F, 0xA5);  /* --nx-accent / --nx-border (cerulean) */
    c_ok = fb_rgb(0x8A, 0xBA, 0xD3);
    c_amber = fb_rgb(0xC9, 0x93, 0x3A);   /* --nx-amber: accents and section heads */
    c_warn = c_amber;
    c_bad = fb_rgb(0xD9, 0x6A, 0x5A);     /* --nx-error */
}

/* The shell's cut-corner pane (DESIGN §13, .nx-cut): a 1 px cerulean ring with 45°
 * cuts at the top-right and bottom-left corners. */
static void cut_frame(fb_t *fb, int x, int y, int w, int h, int cut, uint16_t color) {
    for (int i = 0; i < w - cut; i++) fb->px[y * FB_W + x + i] = color;                  /* top */
    for (int i = cut; i < w; i++) fb->px[(y + h - 1) * FB_W + x + i] = color;            /* bottom */
    for (int i = 0; i < h - cut; i++) fb->px[(y + i) * FB_W + x] = color;                /* left */
    for (int i = cut; i < h; i++) fb->px[(y + i) * FB_W + x + w - 1] = color;            /* right */
    for (int i = 0; i <= cut; i++) {
        fb->px[(y + i) * FB_W + x + w - 1 - cut + i] = color;                            /* top-right cut */
        fb->px[(y + h - 1 - cut + i) * FB_W + x + cut - i] = color;                      /* bottom-left cut */
    }
}

static void mark(fb_t *fb, int x, int y) {
    for (int yy = 0; yy < SPRITE_MARK_H; yy++)
        for (int xx = 0; xx < SPRITE_MARK_W; xx++) {
            uint16_t p = sprite_mark[yy][xx];
            if (p) fb->px[(y + yy) * FB_W + x + xx] = p;
        }
}

static uint16_t attention_color(const char *a) {
    if (!a) return c_dim;
    if (!strcmp(a, "interrupt")) return c_bad;
    if (!strcmp(a, "engaged")) return c_label;
    if (!strcmp(a, "glance")) return c_accent;
    return fb_rgb(0x2E, 0x4F, 0x66);  /* ambient: cerulean dimmed */
}

static void text_fit(fb_t *fb, const fb_font *f, int x, int y, const char *s, uint16_t color, int max_px) {
    char buf[256];
    fb_fit(f, s, 1, max_px, buf, sizeof buf);
    fb_text(fb, f, x, y, buf, color, 1);
}

/* Draw up to max_lines strings from a JSON array; returns lines drawn. */
static int draw_lines(fb_t *fb, const jnode *arr, int y, int pitch, int max_lines, uint16_t color) {
    int n = 0;
    for (const jnode *l = arr && arr->type == J_ARR ? arr->first : NULL; l && n < max_lines; l = l->next) {
        if (l->type != J_STR) continue;
        text_fit(fb, &FONT_BODY, MARGIN, y + n * pitch, l->v.s, color, USABLE);
        n++;
    }
    return n;
}

int cards_page_count(const jnode *card) {
    const jnode *pages = json_get(json_get(card, "action"), "pages");
    return pages && pages->type == J_ARR && pages->count > 0 ? (int)pages->count : 1;
}

int cards_option_count(const jnode *card) {
    const jnode *opts = json_get(json_get(card, "action"), "options");
    return opts && opts->type == J_ARR ? (int)opts->count : 0;
}

static void hint(fb_t *fb, int y, const char *text, uint16_t color) {
    fb_rect(fb, MARGIN, y + 9, 6, 6, color, 1);
    text_fit(fb, &FONT_KICKER, MARGIN + 16, y, text, color, USABLE - 16);
}

void cards_render(fb_t *fb, const jnode *card, const card_view *view, const char *attention) {
    palette();
    card_view v = view ? *view : (card_view){0, 0};
    fb_clear(fb, c_bg);
    fb_rect(fb, 0, 0, FB_W, 4, attention_color(attention), 1);
    cut_frame(fb, 8, 10, FB_W - 16, FB_H - 18, 14, c_accent);
    mark(fb, FB_W - MARGIN - SPRITE_MARK_W, 14);

    const char *type = json_str(card, "type", "inform");
    const char *kicker = json_str(card, "kicker", NULL);
    const char *title = json_str(card, "title", "");
    const jnode *action = json_get(card, "action");
    const jnode *source = json_get(card, "source");
    const char *id = json_str(card, "id", "");

    if (!strcmp(type, "approve")) {
        fb_rect(fb, 0, 0, FB_W, 4, c_bad, 1);
        fb_text(fb, &FONT_KICKER, MARGIN, 20, "REFUSED", c_bad, 1);
        text_fit(fb, &FONT_TITLE, MARGIN, 48, "approve card dropped", c_text, USABLE);
        text_fit(fb, &FONT_BODY, MARGIN, 96, "approve cards never travel the", c_dim, USABLE);
        text_fit(fb, &FONT_BODY, MARGIN, 130, "node socket (NEXUS-LINK §2).", c_dim, USABLE);
        text_fit(fb, &FONT_SMALL, MARGIN, 336, id, c_dim, USABLE);
        return;
    }

    /* kicker: the status level colours it on status cards */
    uint16_t kicker_color = c_amber;
    if (!strcmp(type, "status")) {
        const char *level = json_str(action, "level", "ok");
        kicker_color = !strcmp(level, "bad") ? c_bad : !strcmp(level, "warn") ? c_warn : c_ok;
    }
    if (kicker && *kicker) {
        char up[64];
        /* kickers read as labels: upper-case the ASCII letters */
        size_t i = 0;
        for (; kicker[i] && i < sizeof up - 1; i++) up[i] = (kicker[i] >= 'a' && kicker[i] <= 'z') ? (char)(kicker[i] - 32) : kicker[i];
        up[i] = 0;
        text_fit(fb, &FONT_KICKER, MARGIN, 20, up, kicker_color, USABLE - SPRITE_MARK_W - 12);
    }
    {
        const char *agent = json_str(source, "agent", NULL);
        if (agent) {
            char tag[24];
            size_t i = 0;
            for (; agent[i] && i < sizeof tag - 1; i++) tag[i] = (agent[i] >= 'a' && agent[i] <= 'z') ? (char)(agent[i] - 32) : agent[i];
            tag[i] = 0;
            int tw = fb_text_width(&FONT_KICKER, tag, 1);
            fb_text(fb, &FONT_KICKER, FB_W - MARGIN - SPRITE_MARK_W - 14 - tw, 24, tag, c_label, 1);
        }
    }
    text_fit(fb, &FONT_TITLE, MARGIN, 48, title, c_white, USABLE - SPRITE_MARK_W - 12);

    int y = 96;
    int lines = draw_lines(fb, json_get(card, "body"), y, 34, 3, c_text);
    y += lines * 34;
    if (lines) y += 10;
    if (y < 204) y = 204;

    if (!strcmp(type, "checkin")) {
        char h[96];
        snprintf(h, sizeof h, "tap to check in  ·  %s", json_str(action, "event", ""));
        hint(fb, y, h, c_label);
    } else if (!strcmp(type, "action")) {
        char h[96];
        snprintf(h, sizeof h, "tap to run  ·  %s", json_str(action, "invoke", ""));
        hint(fb, y, h, c_label);
    } else if (!strcmp(type, "choose")) {
        const jnode *opts = json_get(action, "options");
        int i = 0;
        for (const jnode *o = opts && opts->type == J_ARR ? opts->first : NULL; o && i < 5; o = o->next, i++) {
            int oy = y + i * 26;
            if (oy + 24 > FB_H - 24) break;
            const char *label = json_str(o, "label", "");
            if (i == v.selected) {
                fb_rect(fb, MARGIN - 8, oy - 2, USABLE + 16, 24, c_accent, 1);
                text_fit(fb, &FONT_KICKER, MARGIN + 16, oy, label, c_white, USABLE - 16);
                fb_rect(fb, MARGIN, oy + 7, 6, 6, c_white, 1);
            } else {
                text_fit(fb, &FONT_KICKER, MARGIN + 16, oy, label, c_text, USABLE - 16);
                fb_rect(fb, MARGIN, oy + 7, 6, 6, c_dim, 0);
            }
        }
    } else if (!strcmp(type, "page")) {
        const jnode *pages = json_get(action, "pages");
        int count = cards_page_count(card);
        if (v.page < 0) v.page = 0;
        if (v.page >= count) v.page = count - 1;
        const jnode *page = json_at(pages, (size_t)v.page);
        int room = (FB_H - 36 - y) / 34;
        if (room > 3) room = 3;
        draw_lines(fb, page, y, 34, room, c_text);
        char pager[32];
        snprintf(pager, sizeof pager, "%d / %d", v.page + 1, count);
        int pw = fb_text_width(&FONT_KICKER, pager, 1);
        fb_text(fb, &FONT_KICKER, FB_W - MARGIN - pw, 336, pager, c_dim, 1);
        if (v.page > 0) fb_text(fb, &FONT_KICKER, FB_W - MARGIN - pw - 30, 336, "\xE2\x97\x80", c_accent, 1);  /* ◀ */
        if (v.page + 1 < count) fb_text(fb, &FONT_KICKER, FB_W - MARGIN - pw - 30 + 14, 336, "\xE2\x96\xB6", c_accent, 1);  /* ▶ */
    } else if (!strcmp(type, "status")) {
        const char *level = json_str(action, "level", "ok");
        uint16_t lc = !strcmp(level, "bad") ? c_bad : !strcmp(level, "warn") ? c_warn : c_ok;
        fb_rect(fb, MARGIN, y + 10, 14, 14, lc, 1);
        text_fit(fb, &FONT_TITLE, MARGIN + 28, y, json_str(action, "value", ""), lc, USABLE - 28);
    }

    /* footer: source and id */
    char foot[128] = "";
    if (source) {
        const char *agent = json_str(source, "agent", NULL), *ref = json_str(source, "ref", NULL);
        (void)agent;
        if (ref) snprintf(foot, sizeof foot, "%s", ref);
    }
    if (strcmp(type, "page") != 0) {
        int iw = fb_text_width(&FONT_SMALL, id, 1);
        fb_text(fb, &FONT_SMALL, FB_W - MARGIN - iw, 338, id, c_dim, 1);
        text_fit(fb, &FONT_SMALL, MARGIN, 338, foot, c_dim, USABLE - iw - 16);
    } else text_fit(fb, &FONT_SMALL, MARGIN, 338, foot, c_dim, USABLE / 2);
}
