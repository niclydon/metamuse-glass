/* prismd-nexusos.c — Full NexusOS Shell & App Suite for Google Glass (640x360 BGRA32).
 *
 * Ports the canonical NexusOS visual aesthetic (DESIGN §13, shell/src/theme.css,
 * home.tsx, launcher.tsx, screensaver.tsx) and the 7 core nexusos-apps (nexus.ops,
 * nexus.brief, nexus.nova, nexus.focus-timer, nexus.aria, nexus.room, nexus.system)
 * natively to the Google Glass OMAP4430 prism framebuffer, while preserving full
 * NEXUS-LINK v1 WebSocket compatibility and the --choose dual-boot chooser.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cards.h"
#include "crypto.h"
#include "fb.h"
#include "gest.h"
#include "json.h"
#include "link.h"
#include "telem.h"
#include "util.h"
#include "ws.h"
#include "nexusos_assets.h"

#define HANDSHAKE_TIMEOUT_MS 2500
#define PING_SILENCE_MS 75000
#define VERSION_STR "0.4.0-nexusos-glass"
#define CHOOSE_VOX 10
#define CHOOSE_NEXUS 11

typedef struct {
    const char *host, *port, *path, *node, *firmware, *png_dir, *fb_path, *fb_format, *exit_after;
    const char *input_dir, *cap_dir, *wlan;
    int ack, timeout_ms, verbose, presence_s, choose_s;
    const char *preview_view;
} opts_t;

typedef enum {
    NX_VIEW_HOME = 0,
    NX_VIEW_LAUNCHER,
    NX_VIEW_OPS,
    NX_VIEW_BRIEF,
    NX_VIEW_NOVA,
    NX_VIEW_FOCUS,
    NX_VIEW_ARIA,
    NX_VIEW_SYSTEM,
    NX_VIEW_SAVER,
    NX_VIEW_REMOTE_CARD
} nx_view_id;

typedef struct {
    ws_t ws;
    uint8_t key[32];
    link_ring ring;
    int64_t clock_offset;
    int have_clock;
    int online;
    char current_card[40];
    card_view view;
    char attention[16];
    jnode *card;

    /* Interactive NexusOS Shell & App State */
    nx_view_id ui_view;
    int home_focus;       /* 0..3 tiles on Home */
    int launcher_focus;   /* 0..7 apps in Launcher */
    int ops_pane_idx;     /* 0..3 coder panes in Ops Room */
    int brief_page_idx;   /* 0..3 pages in ARIA Daily Brief */
    int nova_btn_idx;     /* 0..2 buttons in Nova app */
    int focus_btn_idx;    /* 0..2 buttons in Focus Timer */
    int focus_running;
    int focus_rem_s;
    int focus_total_s;
    int64_t focus_last_tick_ms;
    int aria_checked_in;
    int sys_btn_idx;      /* 0..2 buttons in System & Dock */
    int saver_tick;

    /* Cached live hardware & Nova telemetry */
    int bat_pct;
    int bat_mv;
    int bat_temp_c;
    int wifi_rssi;
    char wifi_ip[32];
    char nova_state[64];
    char nova_user[256];
    char nova_reply[512];
    int voice_active;
    int64_t last_telem_ms;
} session_t;

static fb_t canvas;
static const opts_t *g_opts;

/* ---- Panel Wake Safety (Prevents OMAP4 DISPC L3 SIGBUS when waking from sleep) ---- */
static void wake_glass_panel(void) {
#ifdef PRISM_DEVICE
    static int woken = 0;
    if (woken) return;
    const char *sys_paths[] = {
        "/sys/devices/omapdss/display0/enabled",
        "/sys/bus/omapdss/devices/panel-notle-dpi/enabled",
        NULL
    };
    for (int i = 0; sys_paths[i]; i++) {
        int fd = open(sys_paths[i], O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "1\n", 2) < 0) {}
            close(fd);
        }
    }
    int bfd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (bfd >= 0) {
        if (write(bfd, "0\n", 2) < 0) {}
        close(bfd);
    }
    woken = 1;
#endif
}

/* ---- Color & Blending Primitives (DESIGN §13) ------------------------------------ */
static inline uint16_t rgb565(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void unpack565(uint16_t c, int *r, int *g, int *b) {
    int rr = (c >> 11) & 31, gg = (c >> 5) & 63, bb = c & 31;
    *r = (rr << 3) | (rr >> 2);
    *g = (gg << 2) | (gg >> 4);
    *b = (bb << 3) | (bb >> 2);
}

static inline uint16_t blend565(uint16_t dst, int sr, int sg, int sb, int alpha255) {
    if (alpha255 <= 0) return dst;
    if (alpha255 >= 255) return rgb565(sr, sg, sb);
    int dr, dg, db;
    unpack565(dst, &dr, &dg, &db);
    int inv = 255 - alpha255;
    return rgb565((sr * alpha255 + dr * inv) / 255,
                  (sg * alpha255 + dg * inv) / 255,
                  (sb * alpha255 + db * inv) / 255);
}

static inline uint16_t blend565_color(uint16_t dst, uint16_t src, int alpha255) {
    int sr, sg, sb;
    unpack565(src, &sr, &sg, &sb);
    return blend565(dst, sr, sg, sb, alpha255);
}

static inline void put_px_alpha(fb_t *fb, int x, int y, int r, int g, int b, int a) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return;
    fb->px[y * FB_W + x] = blend565(fb->px[y * FB_W + x], r, g, b, a);
}

static inline void put_px(fb_t *fb, int x, int y, uint16_t c) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return;
    fb->px[y * FB_W + x] = c;
}

/* Colors from NexusOS shell/src/theme.css */
#define C_TEXT      fb_rgb(0xE9, 0xF2, 0xF7)  /* --nx-text */
#define C_LABEL     fb_rgb(0x8A, 0xBA, 0xD3)  /* --nx-label */
#define C_ACCENT    fb_rgb(0x4A, 0x7F, 0xA5)  /* --nx-accent / --nx-border */
#define C_AMBER     fb_rgb(0xC9, 0x93, 0x3A)  /* --nx-amber */
#define C_ERROR     fb_rgb(0xD9, 0x6A, 0x5A)  /* --nx-error */
#define C_DIM       fb_rgb(0x6E, 0x86, 0x94)  /* muted ice-blue */
#define C_HAIRLINE  fb_rgb(0x28, 0x38, 0x45)  /* --nx-hairline */

/* Check if (dx, dy) inside (w, h) is inside the .nx-cut polygon (45° top-right & bottom-left cuts) */
static inline int inside_nx_cut(int dx, int dy, int w, int h, int cut) {
    if (dx < 0 || dx >= w || dy < 0 || dy >= h) return 0;
    if ((w - 1 - dx) + dy < cut) return 0; /* top-right cut */
    if (dx + (h - 1 - dy) < cut) return 0; /* bottom-left cut */
    return 1;
}

/* Render the NexusOS liquid-glass backdrop:
 * - full_splash == 1: full splash.webp dark liquid-glass waves (for Screensaver & Chooser)
 * - full_splash == 0: --nx-bg (#181A1E) blended with the peripheral liquid-glass wave caustics */
static void draw_nx_backdrop(fb_t *fb, int full_splash) {
    for (int y = 0; y < FB_H; y++) {
        int sy = y / 4;
        if (!full_splash) {
            /* Sample the liquid-glass wave regions from top/bottom third so center ring doesn't intrude */
            sy = (y < FB_H / 2) ? (y * 26 / (FB_H / 2)) : (64 + (y - FB_H / 2) * 25 / (FB_H / 2));
        }
        if (sy >= NX_SPLASH_160X90_H) sy = NX_SPLASH_160X90_H - 1;
        const uint16_t *srow = &nx_splash_160x90[sy * NX_SPLASH_160X90_W];
        uint16_t *drow = &fb->px[y * FB_W];
        if (full_splash) {
            for (int x = 0; x < FB_W; x++) {
                drow[x] = srow[x / 4];
            }
        } else {
            /* Blend liquid-glass wave caustics at 48% over #181A1E graphite */
            for (int x = 0; x < FB_W; x++) {
                uint16_t sp = srow[x / 4];
                drow[x] = blend565_color(rgb565(22, 24, 28), sp, 122);
            }
        }
    }
}

/* Draw a signature NexusOS .nx-cut glass panel with:
 * - Soft drop shadow
 * - Translucent graphite glass fill (--nx-glass: rgba(28, 28, 30, 0.86))
 * - Top vertical sheen gradient (rgba(233, 242, 247, 0.09) -> 0)
 * - Top 1px inner specular highlight (rgba(233, 242, 247, 0.24))
 * - Continuous 1px #4A7FA5 Cerulean border (or 2px #4A7FA5 + #8ABAD3 focus ring when focused) */
static void draw_nx_cut_panel(fb_t *fb, int x, int y, int w, int h, int cut, int focused, int amber_ring) {
    if (w <= 2 * cut || h <= 2 * cut) return;

    /* 1. Soft drop shadow offset by (+2, +4) */
    for (int dy = 0; dy < h; dy++) {
        int py = y + 4 + dy;
        if (py < 0 || py >= FB_H) continue;
        for (int dx = 0; dx < w; dx++) {
            int px = x + 2 + dx;
            if (px < 0 || px >= FB_W) continue;
            if (inside_nx_cut(dx, dy, w, h, cut)) {
                fb->px[py * FB_W + px] = blend565(fb->px[py * FB_W + px], 0, 0, 0, 120);
            }
        }
    }

    /* 2. Translucent glass body + vertical top sheen gradient */
    int base_r = focused ? 32 : 26;
    int base_g = focused ? 43 : 28;
    int base_b = focused ? 54 : 32;
    int base_a = focused ? 232 : 220;
    int sheen_h = (h * 45) / 100;
    if (sheen_h < 8) sheen_h = 8;

    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= FB_H) continue;
        int sheen_a = (dy < sheen_h) ? ((sheen_h - dy) * (focused ? 30 : 22) / sheen_h) : 0;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= FB_W) continue;
            if (!inside_nx_cut(dx, dy, w, h, cut)) continue;
            uint16_t p = blend565(fb->px[py * FB_W + px], base_r, base_g, base_b, base_a);
            if (sheen_a > 0) {
                p = blend565(p, 0xE9, 0xF2, 0xF7, sheen_a);
            }
            fb->px[py * FB_W + px] = p;
        }
    }

    /* 3. Top inner specular rim highlight (y + 1) */
    if (y + 1 >= 0 && y + 1 < FB_H) {
        for (int dx = 1; dx < w - cut - 1; dx++) {
            put_px_alpha(fb, x + dx, y + 1, 0xE9, 0xF2, 0xF7, focused ? 76 : 54);
        }
    }

    /* 4. Outer focus ring if focused */
    if (focused) {
        uint16_t glow = amber_ring ? C_AMBER : C_ACCENT;
        int gx = x - 1, gy = y - 1, gw = w + 2, gh = h + 2, gcut = cut + 1;
        for (int i = 0; i < gw - gcut; i++) put_px(fb, gx + i, gy, glow);
        for (int i = gcut; i < gw; i++) put_px(fb, gx + i, gy + gh - 1, glow);
        for (int i = 0; i < gh - gcut; i++) put_px(fb, gx, gy + i, glow);
        for (int i = gcut; i < gh; i++) put_px(fb, gx + gw - 1, gy + i, glow);
        for (int i = 0; i <= gcut; i++) {
            put_px(fb, gx + gw - 1 - gcut + i, gy + i, glow);
            put_px(fb, gx + i, gy + gh - 1 - gcut + i, glow);
        }
    }

    /* 5. Continuous 1px .nx-cut polygon border with anti-aliased 45° bevels */
    uint16_t border = amber_ring ? C_AMBER : (focused ? C_LABEL : C_ACCENT);
    int br, bg, bb;
    unpack565(border, &br, &bg, &bb);

    for (int i = 0; i < w - cut; i++) put_px(fb, x + i, y, border);
    for (int i = cut; i < w; i++) put_px(fb, x + i, y + h - 1, border);
    for (int i = 0; i < h - cut; i++) put_px(fb, x, y + i, border);
    for (int i = cut; i < h; i++) put_px(fb, x + w - 1, y + i, border);
    for (int i = 0; i <= cut; i++) {
        /* Top-right 45° diagonal bevel + 1px inner AA smoothing */
        int tx = x + w - 1 - cut + i, ty = y + i;
        put_px(fb, tx, ty, border);
        if (i > 0 && i < cut) put_px_alpha(fb, tx - 1, ty, br, bg, bb, 110);
        /* Bottom-left 45° diagonal bevel + 1px inner AA smoothing */
        int bx = x + i, by = y + h - 1 - cut + i;
        put_px(fb, bx, by, border);
        if (i > 0 && i < cut) put_px_alpha(fb, bx + 1, by, br, bg, bb, 110);
    }
}

/* Draw an RGBA sprite from nexusos_assets.h */
static void draw_nx_sprite(fb_t *fb, int x, int y, int w, int h, const uint16_t *rgb, const uint8_t *alpha) {
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= FB_H) continue;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= FB_W) continue;
            uint8_t a = alpha[dy * w + dx];
            if (!a) continue;
            fb->px[py * FB_W + px] = blend565_color(fb->px[py * FB_W + px], rgb[dy * w + dx], a);
        }
    }
}

/* Draw a glowing status dot (6x6 with soft corner AA) */
static void draw_nx_dot(fb_t *fb, int x, int y, uint16_t color) {
    int r, g, b;
    unpack565(color, &r, &g, &b);
    for (int dy = 0; dy < 7; dy++) {
        for (int dx = 0; dx < 7; dx++) {
            int dist2 = (dx - 3) * (dx - 3) + (dy - 3) * (dy - 3);
            if (dist2 <= 6) put_px(fb, x + dx, y + dy, color);
            else if (dist2 <= 10) put_px_alpha(fb, x + dx, y + dy, r, g, b, 110);
        }
    }
}

/* Draw a NexusOS diamond indicator ◆ (7x7 rotated square) */
static void draw_nx_diamond(fb_t *fb, int x, int y, uint16_t color) {
    for (int dy = -3; dy <= 3; dy++) {
        int span = 3 - (dy < 0 ? -dy : dy);
        for (int dx = -span; dx <= span; dx++) {
            put_px(fb, x + 3 + dx, y + 3 + dy, color);
        }
    }
}

static void draw_hairline(fb_t *fb, int x, int y, int w) {
    for (int i = 0; i < w; i++) {
        put_px_alpha(fb, x + i, y, 0x8A, 0xBA, 0xD3, 42);
    }
}

static void text_fit(fb_t *fb, const fb_font *f, int x, int y, const char *s, uint16_t color, int max_px) {
    if (!s || !*s || max_px <= 0) return;
    char buf[256];
    fb_fit(f, s, 1, max_px, buf, sizeof buf);
    fb_text(fb, f, x, y, buf, color, 1);
}

/* Word-wrapped multi-line text renderer */
static int draw_wrapped_text(fb_t *fb, const fb_font *f, int x, int y, int max_w, int line_h, int max_lines, const char *text, uint16_t color) {
    if (!text || !*text) return 0;
    int max_chars = max_w / f->w;
    if (max_chars < 4) max_chars = 4;
    const char *p = text;
    int line = 0;
    while (*p && line < max_lines) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        int len = 0;
        int last_space = -1;
        while (p[len] && p[len] != '\n' && len < max_chars) {
            if (p[len] == ' ') last_space = len;
            len++;
        }
        int cut = len;
        if (p[len] && p[len] != '\n' && p[len] != ' ' && last_space > max_chars / 3) {
            cut = last_space;
        }
        char buf[160];
        if (cut >= (int)sizeof(buf)) cut = (int)sizeof(buf) - 1;
        memcpy(buf, p, (size_t)cut);
        buf[cut] = '\0';
        if (line == max_lines - 1 && p[cut] != '\0') {
            if (cut > 3) strcpy(buf + cut - 3, "...");
        }
        fb_text(fb, f, x, y + line * line_h, buf, color, 1);
        p += cut;
        line++;
    }
    return line;
}

/* ---- Live Hardware & Nova Telemetry Refresh --------------------------------------- */
static void read_small_text(const char *path, char *out, size_t cap, const char *fallback) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, cap, "%s", fallback);
        return;
    }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    if (!out[0]) snprintf(out, cap, "%s", fallback);
}

static int read_int_file(const char *path, int fallback) {
    char buf[64];
    read_small_text(path, buf, sizeof buf, "");
    return buf[0] ? atoi(buf) : fallback;
}

static void refresh_telemetry(session_t *s, int force) {
    int64_t now = mono_ms();
    if (!force && s->last_telem_ms && now - s->last_telem_ms < 750) return;
    s->last_telem_ms = now;

    s->bat_pct = read_int_file("/sys/class/power_supply/bq27520-0/capacity", 88);
    int uv = read_int_file("/sys/class/power_supply/bq27520-0/voltage_now", 4050000);
    s->bat_mv = uv > 10000 ? uv / 1000 : 4050;
    int t10 = read_int_file("/sys/class/power_supply/bq27520-0/temp", 310);
    s->bat_temp_c = t10 / 10;

    int rssi = -54;
    if (telem_rssi_dbm(g_opts && g_opts->wlan ? g_opts->wlan : "wlan0", &rssi) == 0) {
        s->wifi_rssi = rssi;
    } else {
        s->wifi_rssi = -54;
    }

    read_small_text("/run/glass/nova-state", s->nova_state, sizeof s->nova_state, "READY");
    read_small_text("/run/glass/nova-user", s->nova_user, sizeof s->nova_user,
                    "Tap 'Talk to Nova' or press Camera to snap & ask on your Google Glass thread.");
    read_small_text("/run/glass/nova-reply", s->nova_reply, sizeof s->nova_reply,
                    "Hey Nic! NexusOS Prism is live on your Google Glass. How can I help?");

    s->voice_active = (access("/run/glass/voice-turn.lock", F_OK) == 0);
    if (!s->wifi_ip[0] || (now / 5000) != ((now - 750) / 5000)) {
        FILE *fp = popen("ip -4 -o addr show dev wlan0 2>/dev/null | awk '{split($4,a,\"/\"); print a[1]}' | head -n1", "r");
        if (fp) {
            char ipbuf[32] = "";
            if (fgets(ipbuf, sizeof ipbuf, fp)) {
                size_t l = strlen(ipbuf);
                while (l && (ipbuf[l - 1] == '\n' || ipbuf[l - 1] == '\r')) ipbuf[--l] = 0;
            }
            pclose(fp);
            snprintf(s->wifi_ip, sizeof s->wifi_ip, "%s", ipbuf[0] ? ipbuf : "192.168.0.245");
        } else {
            snprintf(s->wifi_ip, sizeof s->wifi_ip, "192.168.0.245");
        }
    }
}

/* ---- Persistent NexusOS Shell Chrome (TopBar + PresenceStrip) --------------------- */
static void draw_nx_chrome(fb_t *fb, const session_t *s) {
    /* 1. TopBar (.nx-topbar.nx-cut): x=10, y=6, w=620, h=42, cut=10 */
    draw_nx_cut_panel(fb, 10, 6, 620, 42, 10, 0, 0);

    time_t now_t = time(NULL);
    struct tm tmv;
    localtime_r(&now_t, &tmv);
    int hr12 = tmv.tm_hour % 12;
    if (hr12 == 0) hr12 = 12;
    char clk[32], datebuf[40];
    snprintf(clk, sizeof clk, "%d:%02d %s", hr12, tmv.tm_min, tmv.tm_hour >= 12 ? "PM" : "AM");
    strftime(datebuf, sizeof datebuf, "%a, %b %d", &tmv);

    int cw = fb_text(fb, &FONT_BODY, 22, 13, clk, C_TEXT, 1);
    fb_text(fb, &FONT_SMALL, 22 + cw + 12, 20, datebuf, C_LABEL, 1);

    /* Right side of TopBar: Attention + Link + Battery + NexusOS Ring Mark */
    draw_nx_sprite(fb, 588, 11, NX_MARK_32_W, NX_MARK_32_H, nx_mark_32_rgb, nx_mark_32_a);

    char batbuf[32];
    snprintf(batbuf, sizeof batbuf, "%d%%", s->bat_pct);
    int bw = fb_text_width(&FONT_KICKER, batbuf, 1);
    int bx = 578 - bw;
    fb_text(fb, &FONT_KICKER, bx, 17, batbuf, s->bat_pct <= 20 ? C_ERROR : C_TEXT, 1);

    const char *link_lbl = s->online ? "Linked" : "Prism";
    int lw = fb_text_width(&FONT_KICKER, link_lbl, 1);
    int lx = bx - 14 - lw;
    draw_nx_dot(fb, lx - 12, 23, s->online ? C_LABEL : C_AMBER);
    fb_text(fb, &FONT_KICKER, lx, 17, link_lbl, C_LABEL, 1);

    const char *att_lbl = s->voice_active ? "Voice Call" : (s->ui_view == NX_VIEW_HOME ? "Engaged" : "Active");
    int aw = fb_text_width(&FONT_KICKER, att_lbl, 1);
    int ax = lx - 26 - aw;
    draw_nx_diamond(fb, ax - 12, 23, s->voice_active ? C_AMBER : C_ACCENT);
    fb_text(fb, &FONT_KICKER, ax, 17, att_lbl, s->voice_active ? C_AMBER : C_TEXT, 1);

    /* 2. PresenceStrip (.nx-presence.nx-cut): x=10, y=52, w=620, h=26, cut=7 */
    draw_nx_cut_panel(fb, 10, 52, 620, 26, 7, 0, 0);

    int px = 22;
    draw_nx_diamond(fb, px, 61, s->voice_active ? C_AMBER : C_LABEL);
    px += 12;
    px += fb_text(fb, &FONT_SMALL, px, 57, "Nova ", C_TEXT, 1);
    px += fb_text(fb, &FONT_SMALL, px, 57, s->voice_active ? "Live Call" : "Ready", s->voice_active ? C_AMBER : C_LABEL, 1);

    px += 16;
    draw_nx_diamond(fb, px, 61, C_LABEL);
    px += 12;
    px += fb_text(fb, &FONT_SMALL, px, 57, "ARIA ", C_TEXT, 1);
    px += fb_text(fb, &FONT_SMALL, px, 57, s->aria_checked_in ? "Checked In" : "Brief Ready", C_LABEL, 1);

    /* Right-aligned Nodes & Dock status */
    const char *nodes_r = "Nodes  prism  janus  glance   Dock  Glass";
    int nw = fb_text_width(&FONT_SMALL, nodes_r, 1);
    int nx = 618 - nw;
    fb_text(fb, &FONT_SMALL, nx, 57, nodes_r, C_LABEL, 1);
    draw_nx_dot(fb, nx + 6 * 8 - 2, 61, C_LABEL);
    draw_nx_dot(fb, nx + 13 * 8 - 2, 61, C_LABEL);
    draw_nx_dot(fb, nx + 20 * 8 - 2, 61, C_ACCENT);
    draw_nx_dot(fb, nx + 35 * 8 - 2, 61, C_AMBER);
}

/* ---- View 0: NexusOS Home Shell (shell/src/home.tsx) ------------------------------ */
static void render_view_home(fb_t *fb, const session_t *s) {
    /* Deck line (y=82..98) */
    fb_text(fb, &FONT_SMALL, 16, 82, "CARD SET R518  ·  4 TILES", C_LABEL, 1);
    const char *hint = "SWIPE: FOCUS  ·  TAP: OPEN  ·  2-TAP: APPS";
    fb_text(fb, &FONT_SMALL, 624 - fb_text_width(&FONT_SMALL, hint, 1), 82, hint, C_DIM, 1);

    /* 2x2 Cut-Corner Tile Grid:
     * Col 0: x=10, w=304 | Col 1: x=326, w=304
     * Row 0: y=102, h=122 | Row 1: y=232, h=122 */
    const int tx[4] = {10, 326, 10, 326};
    const int ty[4] = {102, 102, 232, 232};
    const int tw = 304, th = 122;

    for (int i = 0; i < 4; i++) {
        int foc = (s->home_focus == i);
        draw_nx_cut_panel(fb, tx[i], ty[i], tw, th, 12, foc, 0);
    }

    /* Tile 0: OPS ROOM (nexus.ops — Herdr Fleet & OB Lanes) */
    {
        int x = tx[0], y = ty[0];
        draw_nx_sprite(fb, x + tw - 40, y + 10, ICON_OPS_W, ICON_OPS_H, icon_ops_rgb, icon_ops_a);
        fb_text(fb, &FONT_KICKER, x + 14, y + 10, "OPS ROOM  ·  HERDR", C_AMBER, 1);
        fb_text(fb, &FONT_BODY, x + 14, y + 34, "4 Panes · 2 Working", C_TEXT, 1);
        draw_nx_dot(fb, x + 14, y + 70, C_LABEL);
        fb_text(fb, &FONT_SMALL, x + 26, y + 66, "muse-pool-1 · RUN · nexus", C_LABEL, 1);
        /* Mini OB queue depth bar */
        fb_text(fb, &FONT_SMALL, x + 14, y + 92, "OB Lanes", C_DIM, 1);
        fb_rect(fb, x + 88, y + 96, 140, 8, C_HAIRLINE, 1);
        fb_rect(fb, x + 88, y + 96, 84, 8, C_LABEL, 1);
        fb_rect(fb, x + 172, y + 96, 36, 8, C_AMBER, 1);
        fb_text(fb, &FONT_SMALL, x + 236, y + 92, "6 / 3", C_LABEL, 1);
    }

    /* Tile 1: ARIA DAILY BRIEF (nexus.brief) */
    {
        int x = tx[1], y = ty[1];
        draw_nx_sprite(fb, x + tw - 40, y + 10, ICON_BRIEF_W, ICON_BRIEF_H, icon_brief_rgb, icon_brief_a);
        fb_text(fb, &FONT_KICKER, x + 14, y + 10, "ARIA  ·  DAILY BRIEF", C_AMBER, 1);
        fb_text(fb, &FONT_BODY, x + 14, y + 34, "Morning Brief (4 p)", C_TEXT, 1);
        fb_text(fb, &FONT_SMALL, x + 14, y + 66, "• Dual-OS Glass port + Janus link", C_LABEL, 1);
        fb_text(fb, &FONT_SMALL, x + 14, y + 90, "• 3 open decisions  ·  Tap to read", C_DIM, 1);
    }

    /* Tile 2: NOVA VOICE & VISION (nexus.nova) */
    {
        int x = tx[2], y = ty[2];
        draw_nx_sprite(fb, x + tw - 40, y + 10, ICON_NOVA_W, ICON_NOVA_H, icon_nova_rgb, icon_nova_a);
        fb_text(fb, &FONT_KICKER, x + 14, y + 10, "NOVA  ·  GLASS THREAD", C_AMBER, 1);
        fb_text(fb, &FONT_BODY, x + 14, y + 34, s->voice_active ? "Live Call Active" : "Voice & 5MP Camera", s->voice_active ? C_AMBER : C_TEXT, 1);
        text_fit(fb, &FONT_SMALL, x + 14, y + 66, s->nova_reply, C_LABEL, tw - 28);
        fb_text(fb, &FONT_SMALL, x + 14, y + 90, "Tap: Open Nova  ·  Hold: Quick Call", C_DIM, 1);
    }

    /* Tile 3: APPS DRAWER & SYSTEM (Launcher / nexus.system) */
    {
        int x = tx[3], y = ty[3];
        draw_nx_sprite(fb, x + tw - 40, y + 10, ICON_SYSTEM_W, ICON_SYSTEM_H, icon_system_rgb, icon_system_a);
        fb_text(fb, &FONT_KICKER, x + 14, y + 10, "APPS & SYSTEM DOCK", C_AMBER, 1);
        fb_text(fb, &FONT_BODY, x + 14, y + 34, "8 Apps · Prism HUD", C_TEXT, 1);
        char sysline[64];
        snprintf(sysline, sizeof sysline, "%s · %d mV · %d C", s->wifi_ip, s->bat_mv, s->bat_temp_c);
        fb_text(fb, &FONT_SMALL, x + 14, y + 66, sysline, C_LABEL, 1);
        fb_text(fb, &FONT_SMALL, x + 14, y + 90, "Tap: Apps Drawer  ·  Pwr: System", C_DIM, 1);
    }
}

/* ---- View 1: NexusOS Apps Launcher Drawer (shell/src/launcher.tsx) ---------------- */
static void render_view_launcher(fb_t *fb, const session_t *s) {
    draw_nx_cut_panel(fb, 10, 84, 620, 270, 14, 0, 0);
    fb_text(fb, &FONT_KICKER, 24, 94, "APPS DRAWER (8)", C_AMBER, 1);
    const char *hint = "SWIPE: SELECT  ·  TAP: OPEN  ·  DOWN: HOME";
    fb_text(fb, &FONT_SMALL, 614 - fb_text_width(&FONT_SMALL, hint, 1), 96, hint, C_LABEL, 1);
    draw_hairline(fb, 22, 118, 596);

    static const struct {
        const char *title;
        const char *sub;
        const uint16_t *rgb;
        const uint8_t *a;
        int w, h;
    } apps[8] = {
        {"Ops Room",     "Herdr & Lanes", icon_ops_rgb,    icon_ops_a,    ICON_OPS_W,    ICON_OPS_H},
        {"Daily Brief",  "ARIA 4-Page",   icon_brief_rgb,  icon_brief_a,  ICON_BRIEF_W,  ICON_BRIEF_H},
        {"Nova Voice",   "Call & Camera", icon_nova_rgb,   icon_nova_a,   ICON_NOVA_W,   ICON_NOVA_H},
        {"Focus Timer",  "25:00 Sprint",  icon_focus_rgb,  icon_focus_a,  ICON_FOCUS_W,  ICON_FOCUS_H},
        {"ARIA & Room",  "Nodes & Check", icon_aria_rgb,   icon_aria_a,   ICON_ARIA_W,   ICON_ARIA_H},
        {"System Dock",  "Prism & Info",  icon_system_rgb, icon_system_a, ICON_SYSTEM_W, ICON_SYSTEM_H},
        {"Liquid Saver", "Wave & Emblem", nx_mark_32_rgb,  nx_mark_32_a,  NX_MARK_32_W,  NX_MARK_32_H},
        {"GlassOS Vox",  "Switch to Vox", icon_room_rgb,   icon_room_a,   ICON_ROOM_W,   ICON_ROOM_H},
    };

    for (int i = 0; i < 8; i++) {
        int col = i % 4;
        int row = i / 4;
        int cx = 22 + col * 149;
        int cy = 126 + row * 110;
        int cw = 141, ch = 100;
        int foc = (s->launcher_focus == i);
        draw_nx_cut_panel(fb, cx, cy, cw, ch, 10, foc, i == 7 && foc);
        draw_nx_sprite(fb, cx + 12, cy + 12, apps[i].w, apps[i].h, apps[i].rgb, apps[i].a);
        fb_text(fb, &FONT_KICKER, cx + 12, cy + 48, apps[i].title, foc ? C_TEXT : C_LABEL, 1);
        fb_text(fb, &FONT_SMALL, cx + 12, cy + 72, apps[i].sub, C_DIM, 1);
    }
}

/* ---- View 2: Ops Room App (nexusos-apps/apps/nexus.ops) --------------------------- */
static void render_view_ops(fb_t *fb, const session_t *s) {
    static const struct {
        const char *name;
        const char *state;
        const char *repo;
        const char *age;
        const char *line;
        int state_kind; /* 0=working, 1=waiting, 2=done */
    } panes[4] = {
        {"muse-pool-1",   "WORKING", "glass-os / prismd-nexusos", "1m", "Rendering .nx-cut liquid-glass shell & 7 apps on /dev/fb0", 0},
        {"asclaude-q-as", "WORKING", "nexusos / shell-r518",      "4m", "Syncing presence strip & USB dock module descriptors",      0},
        {"herdr-triage",  "WAITING", "nexus / ob-queue",          "9m", "Awaiting operator decision on 3 requires_decision cards",   1},
        {"forge-eval-2",  "DONE",    "nexusos-apps /nexus.ops",   "18m","Verified coder-pane flip stack and OB queue depth bars",    2},
    };

    int idx = s->ops_pane_idx % 4;
    if (idx < 0) idx += 4;

    /* Top-left: Coder Pane Flip Stack Card (x=10, y=84, w=396, h=168) */
    draw_nx_cut_panel(fb, 10, 84, 396, 168, 12, 1, 0);
    char pkicker[64];
    snprintf(pkicker, sizeof pkicker, "CODER PANE %d OF 4  ·  SWIPE TO FLIP", idx + 1);
    fb_text(fb, &FONT_KICKER, 24, 96, pkicker, C_AMBER, 1);

    uint16_t st_col = panes[idx].state_kind == 0 ? C_LABEL : (panes[idx].state_kind == 1 ? C_AMBER : C_DIM);
    draw_nx_dot(fb, 24, 132, st_col);
    fb_text(fb, &FONT_TITLE, 38, 120, panes[idx].name, C_TEXT, 1);
    int stw = fb_text_width(&FONT_KICKER, panes[idx].state, 1);
    fb_text(fb, &FONT_KICKER, 392 - stw, 126, panes[idx].state, st_col, 1);

    char meta[96];
    snprintf(meta, sizeof meta, "%s  ·  %s ago", panes[idx].repo, panes[idx].age);
    fb_text(fb, &FONT_SMALL, 24, 158, meta, C_LABEL, 1);
    draw_hairline(fb, 24, 180, 368);
    draw_wrapped_text(fb, &FONT_SMALL, 24, 190, 368, 20, 2, panes[idx].line, C_TEXT);

    /* Top-right: Fleet Totals Card (x=416, y=84, w=214, h=168) */
    draw_nx_cut_panel(fb, 416, 84, 214, 168, 12, 0, 0);
    fb_text(fb, &FONT_KICKER, 430, 96, "FLEET TOTALS", C_AMBER, 1);
    fb_text(fb, &FONT_SMALL, 430, 126, "PANES", C_LABEL, 1);
    fb_text(fb, &FONT_BODY,  430, 144, "4", C_TEXT, 1);
    fb_text(fb, &FONT_SMALL, 530, 126, "WORKING", C_LABEL, 1);
    fb_text(fb, &FONT_BODY,  530, 144, "2", C_LABEL, 1);
    fb_text(fb, &FONT_SMALL, 430, 182, "WAITING", C_LABEL, 1);
    fb_text(fb, &FONT_BODY,  430, 200, "1", C_AMBER, 1);
    fb_text(fb, &FONT_SMALL, 530, 182, "DONE", C_LABEL, 1);
    fb_text(fb, &FONT_BODY,  530, 200, "1", C_DIM, 1);

    /* Bottom: OB Queue Lanes Card (x=10, y=260, w=620, h=94) */
    draw_nx_cut_panel(fb, 10, 260, 620, 94, 10, 0, 0);
    fb_text(fb, &FONT_KICKER, 24, 268, "OB QUEUE LANES  ·  TAP: NOVA BRIEF  ·  DOWN: HOME", C_AMBER, 1);

    const struct { const char *name; int count; int max_w; uint16_t col; } lanes[3] = {
        {"autonomous_safe",        6, 210, C_LABEL},
        {"requires_decision",      3, 110, C_AMBER},
        {"requires_investigation", 1,  44, C_ACCENT},
    };
    for (int i = 0; i < 3; i++) {
        int ly = 292 + i * 20;
        fb_text(fb, &FONT_SMALL, 24, ly, lanes[i].name, C_LABEL, 1);
        fb_rect(fb, 226, ly + 4, 310, 10, C_HAIRLINE, 1);
        fb_rect(fb, 226, ly + 4, lanes[i].max_w, 10, lanes[i].col, 1);
        char cbuf[24];
        snprintf(cbuf, sizeof cbuf, "%d queued", lanes[i].count);
        fb_text(fb, &FONT_SMALL, 548, ly, cbuf, C_TEXT, 1);
    }
}

/* ---- View 3: ARIA Daily Brief App (nexusos-apps/apps/nexus.brief) ----------------- */
static void render_view_brief(fb_t *fb, const session_t *s) {
    int p = s->brief_page_idx % 4;
    if (p < 0) p += 4;

    /* Left Page Rail (.nx-cut): x=10, y=84, w=156, h=270 */
    draw_nx_cut_panel(fb, 10, 84, 156, 270, 10, 0, 0);
    fb_text(fb, &FONT_KICKER, 22, 96, "ARIA BRIEF", C_AMBER, 1);
    static const char *const nav_titles[4] = {
        "1  Today",
        "2  Priority",
        "3  Estate",
        "4  Decide"
    };
    for (int i = 0; i < 4; i++) {
        int ry = 128 + i * 44;
        int sel = (i == p);
        draw_nx_cut_panel(fb, 18, ry, 140, 36, 7, sel, 0);
        fb_text(fb, &FONT_KICKER, 28, ry + 8, nav_titles[i], sel ? C_TEXT : C_LABEL, 1);
    }
    fb_text(fb, &FONT_SMALL, 22, 326, "Swipe: Turn Page", C_DIM, 1);

    /* Right Brief Page Card (.nx-cut): x=176, y=84, w=454, h=270 */
    draw_nx_cut_panel(fb, 176, 84, 454, 270, 12, 1, 0);

    static const struct {
        const char *kicker;
        const char *title;
        const char *b1, *m1;
        const char *b2, *m2;
        const char *b3, *m3;
        const char *foot;
    } pages[4] = {
        {
            "PAGE 1 OF 4  ·  MORNING SYNTHESIS",
            "Today's Brief",
            "• NexusOS Prism Shell live on Glass", "640x360",
            "• Voice & 5MP camera on Glass thread", "ONLINE",
            "• Herdr running 4 panes across 2 repos", "2 RUN",
            "Tap: Open Nova Voice  ·  Down-swipe: Home Shell"
        },
        {
            "PAGE 2 OF 4  ·  ACTIVE TRACKS",
            "Top Priorities",
            "1. Visual parity with Janus (DESIGN §13)", "SHIPPED",
            "2. Dedicated Glass voice + camera thread", "SEEDED",
            "3. Zero-stutter 48kHz bone-conduction", "VERIFIED",
            "Swipe Forward/Back to flip pages (2 / 4)"
        },
        {
            "PAGE 3 OF 4  ·  ESTATE & HEARTH",
            "Nodes & Surfaces",
            "• Janus Desk (100.64.241.52) :7870", "ONLINE",
            "• Furnace Host (192.168.0.233) LAN", "READY",
            "• Google Glass (192.168.0.245) Prism", "LINKED",
            "All estate surfaces healthy · 0 active alerts"
        },
        {
            "PAGE 4 OF 4  ·  OPERATOR QUEUE",
            "Open Decisions (3)",
            "• [OB-412] Promote Prism app pack default", "DECIDE",
            "• [OB-415] Enable Ops Room flip on idle", "DECIDE",
            "• [OB-419] Sync ARIA brief on morning wake", "DECIDE",
            "Tap to discuss decisions with Nova on Voice Call"
        }
    };

    fb_text(fb, &FONT_KICKER, 194, 96, pages[p].kicker, C_AMBER, 1);
    fb_text(fb, &FONT_TITLE,  194, 120, pages[p].title, C_TEXT, 1);
    draw_hairline(fb, 194, 160, 418);

    const char *bullets[3] = {pages[p].b1, pages[p].b2, pages[p].b3};
    const char *metas[3]   = {pages[p].m1, pages[p].m2, pages[p].m3};
    for (int i = 0; i < 3; i++) {
        int by = 174 + i * 44;
        text_fit(fb, &FONT_SMALL, 194, by, bullets[i], C_TEXT, 340);
        int mw = fb_text_width(&FONT_SMALL, metas[i], 1);
        draw_nx_cut_panel(fb, 610 - mw - 16, by - 4, mw + 16, 24, 5, 0, p == 3);
        fb_text(fb, &FONT_SMALL, 610 - mw - 8, by, metas[i], p == 3 ? C_AMBER : C_LABEL, 1);
    }

    draw_hairline(fb, 194, 316, 418);
    fb_text(fb, &FONT_SMALL, 194, 326, pages[p].foot, C_DIM, 1);
}

/* ---- View 4: Nova Voice & Vision App (nexusos-apps/apps/nexus.nova) --------------- */
static void render_view_nova(fb_t *fb, const session_t *s) {
    /* Top Conversation Card (.nx-cut): x=10, y=84, w=620, h=206 */
    draw_nx_cut_panel(fb, 10, 84, 620, 206, 12, 0, s->voice_active);
    draw_nx_sprite(fb, 588, 94, ICON_NOVA_W, ICON_NOVA_H, icon_nova_rgb, icon_nova_a);

    fb_text(fb, &FONT_KICKER, 24, 94, "NOVA  ·  GLASS WEARABLE THREAD", C_AMBER, 1);
    int stw = fb_text_width(&FONT_KICKER, s->nova_state, 1);
    fb_text(fb, &FONT_KICKER, 576 - stw, 94, s->nova_state, s->voice_active ? C_AMBER : C_LABEL, 1);
    draw_hairline(fb, 24, 118, 592);

    /* YOU turn */
    fb_text(fb, &FONT_KICKER, 24, 126, "YOU", C_LABEL, 1);
    draw_wrapped_text(fb, &FONT_SMALL, 76, 128, 536, 18, 2, s->nova_user, C_LABEL);

    draw_hairline(fb, 24, 170, 592);

    /* NOVA turn */
    fb_text(fb, &FONT_KICKER, 24, 178, "NOVA", C_AMBER, 1);
    draw_wrapped_text(fb, &FONT_BODY, 24, 202, 592, 28, 3, s->nova_reply, C_TEXT);

    /* Bottom Action Strip: 3 .nx-cut interactive buttons (y=298..354) */
    const char *btns[3] = {
        s->voice_active ? "● End Voice Call" : "▶ Talk to Nova (Voice)",
        "● Snap 5MP Camera",
        "◀ Home Shell"
    };
    const int bx[3] = {10, 246, 466};
    const int bw[3] = {226, 210, 164};
    for (int i = 0; i < 3; i++) {
        int foc = (s->nova_btn_idx == i);
        draw_nx_cut_panel(fb, bx[i], 298, bw[i], 54, 10, foc, i == 0 && s->voice_active);
        int tw = fb_text_width(&FONT_KICKER, btns[i], 1);
        fb_text(fb, &FONT_KICKER, bx[i] + (bw[i] - tw) / 2, 315, btns[i], foc ? C_TEXT : C_LABEL, 1);
    }
}

/* ---- View 5: Focus Timer App (nexusos-apps/apps/nexus.focus-timer) ---------------- */
static void render_view_focus(fb_t *fb, const session_t *s) {
    draw_nx_cut_panel(fb, 10, 84, 620, 204, 14, 0, s->focus_running);
    draw_nx_sprite(fb, 588, 94, ICON_FOCUS_W, ICON_FOCUS_H, icon_focus_rgb, icon_focus_a);
    fb_text(fb, &FONT_KICKER, 24, 94, "FOCUS TIMER  ·  DEEP WORK SPRINT", C_AMBER, 1);
    fb_text(fb, &FONT_KICKER, 440, 94, s->focus_running ? "● RUNNING" : "● PAUSED", s->focus_running ? C_AMBER : C_LABEL, 1);

    int mins = s->focus_rem_s / 60;
    int secs = s->focus_rem_s % 60;
    char tbuf[32];
    snprintf(tbuf, sizeof tbuf, "%02d:%02d", mins, secs);
    int tw = fb_text_width(&FONT_TITLE, tbuf, 2);
    fb_text(fb, &FONT_TITLE, (FB_W - tw) / 2, 134, tbuf, C_TEXT, 2);

    /* Progress bar */
    int bar_x = 50, bar_y = 222, bar_w = 540, bar_h = 16;
    draw_nx_cut_panel(fb, bar_x, bar_y, bar_w, bar_h, 4, 0, 0);
    int elapsed = s->focus_total_s - s->focus_rem_s;
    if (elapsed < 0) elapsed = 0;
    int fill_w = s->focus_total_s > 0 ? (elapsed * (bar_w - 6)) / s->focus_total_s : 0;
    if (fill_w > 0) {
        fb_rect(fb, bar_x + 3, bar_y + 3, fill_w, bar_h - 6, s->focus_running ? C_AMBER : C_ACCENT, 1);
    }
    fb_text(fb, &FONT_SMALL, bar_x, 250, "Swipe: Select Action  ·  Tap: Trigger  ·  Down-swipe: Home", C_DIM, 1);

    const char *btns[3] = {
        s->focus_running ? "● Pause Timer" : "▶ Start Timer",
        "● Reset 25:00",
        "◀ Home Shell"
    };
    const int bx[3] = {10, 230, 440};
    const int bw[3] = {210, 200, 190};
    for (int i = 0; i < 3; i++) {
        int foc = (s->focus_btn_idx == i);
        draw_nx_cut_panel(fb, bx[i], 298, bw[i], 54, 10, foc, i == 0 && s->focus_running);
        int btw = fb_text_width(&FONT_KICKER, btns[i], 1);
        fb_text(fb, &FONT_KICKER, bx[i] + (bw[i] - btw) / 2, 315, btns[i], foc ? C_TEXT : C_LABEL, 1);
    }
}

/* ---- View 6: ARIA & Room App (nexusos-apps/apps/nexus.aria + nexus.room) ---------- */
static void render_view_aria(fb_t *fb, const session_t *s) {
    /* Left Card: ARIA Presence & Check-in */
    draw_nx_cut_panel(fb, 10, 84, 304, 270, 12, 1, 0);
    draw_nx_sprite(fb, 272, 94, ICON_ARIA_W, ICON_ARIA_H, icon_aria_rgb, icon_aria_a);
    fb_text(fb, &FONT_KICKER, 24, 94, "ARIA  ·  AMBIENT PARTNER", C_AMBER, 1);
    fb_text(fb, &FONT_TITLE,  24, 122, s->aria_checked_in ? "Checked In" : "Morning Check-In", C_TEXT, 1);
    draw_hairline(fb, 24, 162, 276);
    draw_wrapped_text(fb, &FONT_SMALL, 24, 174, 276, 20, 4,
                      s->aria_checked_in
                          ? "Check-in logged from Google Glass Prism. Focus blocks & estate routing aligned."
                          : "Ready for your wearable check-in. Tap to confirm posture and sync with Janus.",
                      C_LABEL);
    draw_nx_cut_panel(fb, 24, 292, 276, 46, 8, 1, s->aria_checked_in);
    draw_nx_diamond(fb, 46, 311, C_AMBER);
    fb_text(fb, &FONT_KICKER, 62, 305, s->aria_checked_in ? "Posture Confirmed" : "Tap to Check In", C_TEXT, 1);

    /* Right Card: Room & Linked Surfaces (nexus.room) */
    draw_nx_cut_panel(fb, 326, 84, 304, 270, 12, 0, 0);
    draw_nx_sprite(fb, 588, 94, ICON_ROOM_W, ICON_ROOM_H, icon_room_rgb, icon_room_a);
    fb_text(fb, &FONT_KICKER, 340, 94, "ROOM  ·  LINKED SURFACES", C_AMBER, 1);
    fb_text(fb, &FONT_BODY,   340, 122, "4 Estate Nodes", C_TEXT, 1);
    draw_hairline(fb, 340, 162, 276);

    static const struct { const char *node; const char *desc; uint16_t col; } nodes[4] = {
        {"prism",  "Glass XE · 640x360",     0},
        {"slate",  "Janus Desk · 1280x800",  0},
        {"glance", "Ambient HUD · Active",   0},
        {"hearth", "AppleTV + Hue Lighting", 0},
    };
    for (int i = 0; i < 4; i++) {
        int ny = 176 + i * 40;
        draw_nx_dot(fb, 340, ny + 5, i == 0 ? C_AMBER : C_LABEL);
        fb_text(fb, &FONT_KICKER, 354, ny - 2, nodes[i].node, C_TEXT, 1);
        fb_text(fb, &FONT_SMALL,  422, ny, nodes[i].desc, C_LABEL, 1);
    }
}

/* ---- View 7: System & USB Dock App (shell/src/tiles/system.tsx + dock.tsx) -------- */
static void render_view_system(fb_t *fb, const session_t *s) {
    /* Left Card: System Facts (dl.nx-facts) */
    draw_nx_cut_panel(fb, 10, 84, 386, 270, 12, 0, 0);
    draw_nx_sprite(fb, 354, 94, ICON_SYSTEM_W, ICON_SYSTEM_H, icon_system_rgb, icon_system_a);
    fb_text(fb, &FONT_KICKER, 24, 94, "SYSTEM  ·  NEXUSOS PRISM", C_AMBER, 1);
    draw_hairline(fb, 24, 120, 358);

    char bat_val[64], net_val[64], up_val[64];
    snprintf(bat_val, sizeof bat_val, "%d%%  ·  %d mV  ·  %d C", s->bat_pct, s->bat_mv, s->bat_temp_c);
    snprintf(net_val, sizeof net_val, "%s (%d dBm)", s->wifi_ip, s->wifi_rssi);
    long up_s = (long)(mono_ms() / 1000);
    snprintf(up_val, sizeof up_val, "%ldm %lds  ·  OMAP4430", up_s / 60, up_s % 60);

    const struct { const char *k; const char *v; } facts[6] = {
        {"Profile",  "prism  (Google Glass XE)"},
        {"Density",  "compact  (640x360 BGRA32)"},
        {"Version",  VERSION_STR},
        {"Battery",  bat_val},
        {"Wi-Fi",    net_val},
        {"Uptime",   up_val},
    };
    for (int i = 0; i < 6; i++) {
        int fy = 132 + i * 35;
        fb_text(fb, &FONT_SMALL, 24, fy, facts[i].k, C_LABEL, 1);
        fb_text(fb, &FONT_SMALL, 128, fy, facts[i].v, C_TEXT, 1);
        if (i < 5) draw_hairline(fb, 24, fy + 24, 358);
    }

    /* Right Card: USB Dock & OS Switcher */
    draw_nx_cut_panel(fb, 406, 84, 224, 270, 12, 0, 0);
    fb_text(fb, &FONT_KICKER, 420, 94, "USB DOCK & OS", C_AMBER, 1);
    fb_text(fb, &FONT_SMALL,  420, 122, "Audio: TWL6040 McPDM", C_LABEL, 1);
    fb_text(fb, &FONT_SMALL,  420, 144, "Cam:   OV5680 Ducati", C_LABEL, 1);
    draw_hairline(fb, 420, 170, 196);

    const char *btns[3] = {
        "Liquid Screensaver",
        "Switch to GlassOS",
        "◀ Back to Home"
    };
    for (int i = 0; i < 3; i++) {
        int by = 182 + i * 54;
        int foc = (s->sys_btn_idx == i);
        draw_nx_cut_panel(fb, 418, by, 200, 44, 8, foc, i == 1 && foc);
        int btw = fb_text_width(&FONT_KICKER, btns[i], 1);
        fb_text(fb, &FONT_KICKER, 418 + (200 - btw) / 2, by + 12, btns[i], foc ? C_TEXT : C_LABEL, 1);
    }
}

/* ---- View 8: Liquid-Glass Screensaver (shell/src/screensaver.tsx) ----------------- */
static void render_view_saver(fb_t *fb, const session_t *s) {
    draw_nx_backdrop(fb, 1);

    /* Gently drifting 3D NexusOS Ring Emblem around the center */
    static const int dx_tab[8] = {0, 14, 22, 14, 0, -14, -22, -14};
    static const int dy_tab[8] = {-10, -4, 6, 12, 10, 4, -6, -12};
    int t = s->saver_tick & 7;
    int mx = (FB_W - NX_MARK_96_W) / 2 + dx_tab[t];
    int my = (FB_H - NX_MARK_96_H) / 2 - 14 + dy_tab[t];
    draw_nx_sprite(fb, mx, my, NX_MARK_96_W, NX_MARK_96_H, nx_mark_96_rgb, nx_mark_96_a);

    /* Bottom cut-corner HUD bar on screensaver */
    draw_nx_cut_panel(fb, 140, 296, 360, 48, 10, 1, 0);
    time_t now_t = time(NULL);
    struct tm tmv;
    localtime_r(&now_t, &tmv);
    int hr12 = tmv.tm_hour % 12;
    if (hr12 == 0) hr12 = 12;
    char sline[64];
    snprintf(sline, sizeof sline, "NEXUSOS PRISM  ·  %d:%02d %s  ·  %d%%",
             hr12, tmv.tm_min, tmv.tm_hour >= 12 ? "PM" : "AM", s->bat_pct);
    int sw = fb_text_width(&FONT_KICKER, sline, 1);
    fb_text(fb, &FONT_KICKER, (FB_W - sw) / 2, 304, sline, C_TEXT, 1);
    const char *wake_hint = "Tap or Swipe to return to Home Shell";
    fb_text(fb, &FONT_SMALL, (FB_W - fb_text_width(&FONT_SMALL, wake_hint, 1)) / 2, 324, wake_hint, C_LABEL, 1);
}

/* ---- View 9: Remote Pushed Card Modal inside NexusOS Chrome ----------------------- */
static void render_view_remote_card(fb_t *fb, const session_t *s) {
    if (!s->card) {
        render_view_home(fb, s);
        return;
    }
    draw_nx_cut_panel(fb, 10, 84, 620, 270, 14, 1, 0);
    const char *kicker = json_str(s->card, "kicker", "NEXUSD CARD");
    const char *title = json_str(s->card, "title", "");
    const char *type = json_str(s->card, "type", "inform");
    const jnode *action = json_get(s->card, "action");

    fb_text(fb, &FONT_KICKER, 26, 96, kicker, C_AMBER, 1);
    fb_text(fb, &FONT_SMALL, 450, 98, "2-TAP / DOWN: HOME SHELL", C_LABEL, 1);
    text_fit(fb, &FONT_TITLE, 26, 122, title, C_TEXT, 580);
    draw_hairline(fb, 26, 160, 588);

    int y = 172;
    const jnode *arr = json_get(s->card, "body");
    int n = 0;
    for (const jnode *l = arr && arr->type == J_ARR ? arr->first : NULL; l && n < 3; l = l->next) {
        if (l->type != J_STR) continue;
        text_fit(fb, &FONT_BODY, 26, y + n * 32, l->v.s, C_TEXT, 588);
        n++;
    }
    y += n * 32 + 8;

    if (!strcmp(type, "choose")) {
        const jnode *opts = json_get(action, "options");
        int i = 0;
        for (const jnode *o = opts && opts->type == J_ARR ? opts->first : NULL; o && i < 4; o = o->next, i++) {
            int ox = 26 + (i % 2) * 296;
            int oy = 252 + (i / 2) * 46;
            int sel = (i == s->view.selected);
            draw_nx_cut_panel(fb, ox, oy, 284, 38, 8, sel, 0);
            text_fit(fb, &FONT_KICKER, ox + 14, oy + 9, json_str(o, "label", ""), sel ? C_TEXT : C_LABEL, 256);
        }
    } else if (!strcmp(type, "status")) {
        const char *val = json_str(action, "value", "");
        draw_nx_dot(fb, 26, y + 10, C_LABEL);
        text_fit(fb, &FONT_BODY, 42, y, val, C_LABEL, 560);
    }
}

/* ---- Master NexusOS Frame Render & Present ---------------------------------------- */
static int write_fb(const fb_t *fb_canvas) {
    wake_glass_panel();
    if (g_opts->fb_format && !strcmp(g_opts->fb_format, "rgb565"))
        return fb_write_raw(fb_canvas, g_opts->fb_path);
    return fb_present(fb_canvas, g_opts->fb_path);
}

static void render_nexusos(session_t *s) {
    refresh_telemetry(s, 0);
    if (s->ui_view == NX_VIEW_SAVER) {
        render_view_saver(&canvas, s);
    } else {
        draw_nx_backdrop(&canvas, 0);
        draw_nx_chrome(&canvas, s);
        switch (s->ui_view) {
        case NX_VIEW_HOME:        render_view_home(&canvas, s); break;
        case NX_VIEW_LAUNCHER:    render_view_launcher(&canvas, s); break;
        case NX_VIEW_OPS:         render_view_ops(&canvas, s); break;
        case NX_VIEW_BRIEF:       render_view_brief(&canvas, s); break;
        case NX_VIEW_NOVA:        render_view_nova(&canvas, s); break;
        case NX_VIEW_FOCUS:       render_view_focus(&canvas, s); break;
        case NX_VIEW_ARIA:        render_view_aria(&canvas, s); break;
        case NX_VIEW_SYSTEM:      render_view_system(&canvas, s); break;
        case NX_VIEW_REMOTE_CARD: render_view_remote_card(&canvas, s); break;
        default:                  render_view_home(&canvas, s); break;
        }
    }
    if (g_opts && g_opts->png_dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/nexusos-live.png", g_opts->png_dir);
        fb_write_png(&canvas, path);
    }
    if (g_opts && g_opts->fb_path) {
        write_fb(&canvas);
    }
}

/* ---- Background Helpers to Trigger Voice / Camera / OS Switch --------------------- */
static void trigger_glass_voice(session_t *s) {
    if (system("/usr/sbin/glass-muse-voice --tap >/dev/null 2>&1 &") < 0) {}
    usleep(120000);
    refresh_telemetry(s, 1);
    render_nexusos(s);
}

static void trigger_glass_camera(session_t *s) {
    s->ui_view = NX_VIEW_NOVA;
    snprintf(s->nova_state, sizeof s->nova_state, "SNAPPING 5MP...");
    render_nexusos(s);
    if (system("/usr/sbin/glass-muse-camera >/dev/null 2>&1 &") < 0) {}
}

static void trigger_switch_to_vox(void) {
    plog("switching OS from NexusOS to GlassOS (Vox)");
    if (system("(/etc/glass/hud switch vox >/dev/null 2>&1) &") < 0) {}
    usleep(200000);
    exit(0);
}

/* ---- NEXUS-LINK v1 WebSocket Framing Helpers -------------------------------------- */
static int64_t host_now(const session_t *s) { return mono_ms() + s->clock_offset; }

static int send_op(session_t *s, const char *op, const char *body_json, int64_t ts) {
    if (!s->online) return -1;
    char id[33];
    if (make_hex32(id)) return -1;
    char head[LINK_MAX_FRAME], text[LINK_MAX_FRAME + 1];
    jwriter w;
    jw_init(&w, head, sizeof head);
    jw_obj_begin(&w);
    jw_key(&w, "v"); jw_int(&w, 1);
    jw_key(&w, "op"); jw_str(&w, op);
    jw_key(&w, "id"); jw_str(&w, id);
    jw_key(&w, "ts"); jw_int(&w, ts);
    jw_key(&w, "from"); jw_str(&w, g_opts->node);
    jw_key(&w, "body"); jw_raw(&w, body_json);
    jw_obj_end(&w);
    if (!jw_ok(&w)) return -1;
    int n = link_encode(s->key, head, w.len, text, sizeof text);
    if (n < 0) return -1;
    return ws_send_text(&s->ws, text, (size_t)n);
}

static int send_card_action(session_t *s, const char *card_id, const char *event) {
    char body[256], esc_id[80];
    jwriter w;
    jw_init(&w, esc_id, sizeof esc_id);
    jw_str(&w, card_id);
    snprintf(body, sizeof body, "{\"card_id\":%s,\"event\":\"%s\",\"hold_ms\":0}", esc_id, event);
    return send_op(s, "card.action", body, host_now(s));
}

static int send_intent(session_t *s, const char *intent, const char *source) {
    char body[128];
    snprintf(body, sizeof body, "{\"intent\":\"%s\",\"source\":\"%s\"}", intent, source);
    return send_op(s, "input.intent", body, host_now(s));
}

/* ---- Interactive Touchpad & Button Dispatcher for NexusOS ------------------------- */
static void on_gesture(void *ctx, const gest_event *e) {
    session_t *s = ctx;

    /* Any gesture on Screensaver wakes to Home Shell */
    if (s->ui_view == NX_VIEW_SAVER) {
        s->ui_view = NX_VIEW_HOME;
        render_nexusos(s);
        return;
    }

    /* Two-finger tap toggles the Apps Launcher Drawer from anywhere */
    if (e->kind == GEST_TAP2) {
        s->ui_view = (s->ui_view == NX_VIEW_LAUNCHER) ? NX_VIEW_HOME : NX_VIEW_LAUNCHER;
        render_nexusos(s);
        return;
    }

    /* Down-swipe returns to Home Shell (or enters Liquid Screensaver from Home) */
    if (e->kind == GEST_SWIPE_DOWN) {
        if (s->ui_view == NX_VIEW_HOME) s->ui_view = NX_VIEW_SAVER;
        else s->ui_view = NX_VIEW_HOME;
        render_nexusos(s);
        return;
    }

    /* Long press jumps to Nova Voice & starts/toggles live call */
    if (e->kind == GEST_LONG || e->kind == GEST_HOLD_START) {
        s->ui_view = NX_VIEW_NOVA;
        s->nova_btn_idx = 0;
        trigger_glass_voice(s);
        return;
    }

    /* Physical Camera Button snaps 5MP photo & opens Nova Vision */
    if (e->kind == GEST_KEY_CAMERA) {
        trigger_glass_camera(s);
        return;
    }

    /* Physical Back Power Button:
     * - First press from any view -> opens System & OS Switcher with 'Switch to GlassOS' pre-selected
     * - Second press (when already on System view) -> immediately switches to GlassOS (Vox)! */
    if (e->kind == GEST_KEY_POWER) {
        if (s->ui_view != NX_VIEW_SYSTEM) {
            s->ui_view = NX_VIEW_SYSTEM;
            s->sys_btn_idx = 1; /* Pre-focus 'Switch to GlassOS' for 1-tap or 2nd-power-press OS switch */
            render_nexusos(s);
        } else {
            trigger_switch_to_vox();
        }
        return;
    }

    int dir = (e->kind == GEST_SWIPE_FWD) ? 1 : (e->kind == GEST_SWIPE_BACK ? -1 : 0);

    switch (s->ui_view) {
    case NX_VIEW_HOME:
        if (dir != 0) {
            s->home_focus = (s->home_focus + dir + 4) % 4;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            if (s->home_focus == 0) s->ui_view = NX_VIEW_OPS;
            else if (s->home_focus == 1) s->ui_view = NX_VIEW_BRIEF;
            else if (s->home_focus == 2) s->ui_view = NX_VIEW_NOVA;
            else s->ui_view = NX_VIEW_LAUNCHER;
            render_nexusos(s);
        }
        break;

    case NX_VIEW_LAUNCHER:
        if (dir != 0) {
            s->launcher_focus = (s->launcher_focus + dir + 8) % 8;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            switch (s->launcher_focus) {
            case 0: s->ui_view = NX_VIEW_OPS; break;
            case 1: s->ui_view = NX_VIEW_BRIEF; break;
            case 2: s->ui_view = NX_VIEW_NOVA; break;
            case 3: s->ui_view = NX_VIEW_FOCUS; break;
            case 4: s->ui_view = NX_VIEW_ARIA; break;
            case 5: s->ui_view = NX_VIEW_SYSTEM; break;
            case 6: s->ui_view = NX_VIEW_SAVER; break;
            case 7: trigger_switch_to_vox(); return;
            }
            render_nexusos(s);
        }
        break;

    case NX_VIEW_OPS:
        if (dir != 0) {
            s->ops_pane_idx = (s->ops_pane_idx + dir + 4) % 4;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            s->ui_view = NX_VIEW_BRIEF;
            render_nexusos(s);
        }
        break;

    case NX_VIEW_BRIEF:
        if (dir != 0) {
            s->brief_page_idx = (s->brief_page_idx + dir + 4) % 4;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            s->ui_view = NX_VIEW_NOVA;
            render_nexusos(s);
        }
        break;

    case NX_VIEW_NOVA:
        if (dir != 0) {
            s->nova_btn_idx = (s->nova_btn_idx + dir + 3) % 3;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            if (s->nova_btn_idx == 0) trigger_glass_voice(s);
            else if (s->nova_btn_idx == 1) trigger_glass_camera(s);
            else { s->ui_view = NX_VIEW_HOME; render_nexusos(s); }
        }
        break;

    case NX_VIEW_FOCUS:
        if (dir != 0) {
            s->focus_btn_idx = (s->focus_btn_idx + dir + 3) % 3;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            if (s->focus_btn_idx == 0) {
                s->focus_running = !s->focus_running;
                s->focus_last_tick_ms = mono_ms();
            } else if (s->focus_btn_idx == 1) {
                s->focus_running = 0;
                s->focus_rem_s = s->focus_total_s = 25 * 60;
            } else {
                s->ui_view = NX_VIEW_HOME;
            }
            render_nexusos(s);
        }
        break;

    case NX_VIEW_ARIA:
        if (dir != 0) {
            s->ui_view = NX_VIEW_HOME;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            s->aria_checked_in = !s->aria_checked_in;
            if (s->online) send_intent(s, "checkin", "touch");
            render_nexusos(s);
        }
        break;

    case NX_VIEW_SYSTEM:
        if (dir != 0) {
            s->sys_btn_idx = (s->sys_btn_idx + dir + 3) % 3;
            render_nexusos(s);
        } else if (e->kind == GEST_TAP) {
            if (s->sys_btn_idx == 0) { s->ui_view = NX_VIEW_SAVER; render_nexusos(s); }
            else if (s->sys_btn_idx == 1) { trigger_switch_to_vox(); return; }
            else { s->ui_view = NX_VIEW_HOME; render_nexusos(s); }
        }
        break;

    case NX_VIEW_REMOTE_CARD: {
        int options = s->card ? cards_option_count(s->card) : 0;
        if (dir != 0 && options > 1) {
            int n = s->view.selected + dir;
            if (n >= 0 && n < options) { s->view.selected = n; render_nexusos(s); }
        } else if (e->kind == GEST_TAP) {
            if (options > 0 && s->online) {
                const jnode *opt = json_at(json_get(json_get(s->card, "action"), "options"), (size_t)s->view.selected);
                char ev[72];
                snprintf(ev, sizeof ev, "choose.%s", json_str(opt, "id", "?"));
                send_card_action(s, s->current_card, ev);
            }
            s->ui_view = NX_VIEW_HOME;
            render_nexusos(s);
        }
        break;
    }
    default:
        s->ui_view = NX_VIEW_HOME;
        render_nexusos(s);
        break;
    }
}

/* ---- Input Polling ---------------------------------------------------------------- */
static struct pollfd g_in[8];
static int g_nin;
static gest_t g_gest;

static void open_inputs(void) {
    if (!g_opts->input_dir) return;
    DIR *d = opendir(g_opts->input_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_nin < 8) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", g_opts->input_dir, de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        g_in[g_nin].fd = fd;
        g_in[g_nin].events = POLLIN;
        g_nin++;
    }
    closedir(d);
    gest_init(&g_gest);
}

struct raw_input_event { struct timeval time; uint16_t type, code; int32_t value; };
typedef void (*gest_cb)(void *ctx, const gest_event *e);

static void pump_inputs(int wait_ms, gest_cb cb, void *ctx) {
    if (!g_nin) { if (wait_ms > 0) usleep((useconds_t)wait_ms * 1000); return; }
    int r = poll(g_in, (nfds_t)g_nin, wait_ms);
    int64_t now = mono_ms();
    gest_event e;
    if (r > 0) {
        for (int i = 0; i < g_nin; i++) {
            if (!(g_in[i].revents & POLLIN)) continue;
            struct raw_input_event ev;
            while (read(g_in[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
                if (gest_feed(&g_gest, ev.type, ev.code, ev.value, now, &e)) cb(ctx, &e);
        }
    }
    if (gest_tick(&g_gest, now, &e)) cb(ctx, &e);
}

/* ---- Periodic UI Tick (Focus Timer countdown, Live Nova state, Screensaver drift) -- */
static void tick_ui(session_t *s) {
    int64_t now = mono_ms();
    int dirty = 0;

    if (access("/run/glass/nexus-view", F_OK) == 0) {
        char vbuf[32] = "";
        read_small_text("/run/glass/nexus-view", vbuf, sizeof vbuf, "");
        unlink("/run/glass/nexus-view");
        if (!strcmp(vbuf, "home")) { s->ui_view = NX_VIEW_HOME; dirty = 1; }
        else if (!strcmp(vbuf, "launcher")) { s->ui_view = NX_VIEW_LAUNCHER; dirty = 1; }
        else if (!strcmp(vbuf, "ops")) { s->ui_view = NX_VIEW_OPS; dirty = 1; }
        else if (!strcmp(vbuf, "brief")) { s->ui_view = NX_VIEW_BRIEF; dirty = 1; }
        else if (!strcmp(vbuf, "nova")) { s->ui_view = NX_VIEW_NOVA; dirty = 1; }
        else if (!strcmp(vbuf, "focus")) { s->ui_view = NX_VIEW_FOCUS; dirty = 1; }
        else if (!strcmp(vbuf, "aria")) { s->ui_view = NX_VIEW_ARIA; dirty = 1; }
        else if (!strcmp(vbuf, "system")) { s->ui_view = NX_VIEW_SYSTEM; s->sys_btn_idx = 1; dirty = 1; }
        else if (!strcmp(vbuf, "saver")) { s->ui_view = NX_VIEW_SAVER; dirty = 1; }
        else if (!strcmp(vbuf, "vox")) { trigger_switch_to_vox(); return; }
    }

    if (s->focus_running && s->focus_rem_s > 0) {
        if (now - s->focus_last_tick_ms >= 1000) {
            int step = (int)((now - s->focus_last_tick_ms) / 1000);
            s->focus_last_tick_ms += (int64_t)step * 1000;
            s->focus_rem_s -= step;
            if (s->focus_rem_s <= 0) {
                s->focus_rem_s = 0;
                s->focus_running = 0;
            }
            if (s->ui_view == NX_VIEW_FOCUS) dirty = 1;
        }
    }

    static int64_t last_periodic_ms = 0;
    int interval = (s->ui_view == NX_VIEW_NOVA || s->voice_active) ? 350 : 2000;
    if (now - last_periodic_ms >= interval) {
        last_periodic_ms = now;
        s->saver_tick++;
        char prev_state[64], prev_reply[512];
        snprintf(prev_state, sizeof prev_state, "%s", s->nova_state);
        snprintf(prev_reply, sizeof prev_reply, "%s", s->nova_reply);
        refresh_telemetry(s, 1);
        if (strcmp(prev_state, s->nova_state) != 0 || strcmp(prev_reply, s->nova_reply) != 0 ||
            s->ui_view == NX_VIEW_SAVER) {
            dirty = 1;
        }
    }

    static int last_min = -1;
    time_t tnow = time(NULL);
    struct tm tmv;
    localtime_r(&tnow, &tmv);
    if (tmv.tm_min != last_min) {
        last_min = tmv.tm_min;
        dirty = 1;
    }

    if (dirty) render_nexusos(s);
}

/* ---- Stage-2 Dual-Boot Chooser (--choose <secs>) Styled in Authentic NexusOS ------ */
static int g_choice;
static int g_chooser_sel = 0; /* 0 = NexusOS, 1 = GlassOS Vox */

static void on_choose(void *ctx, const gest_event *e) {
    (void)ctx;
    switch (e->kind) {
    case GEST_TAP:
    case GEST_TAP2:
    case GEST_LONG:
        g_choice = CHOOSE_VOX;
        break;
    case GEST_SWIPE_FWD:
    case GEST_SWIPE_BACK:
    case GEST_SWIPE_DOWN:
    case GEST_SWIPE_UP:
        g_choice = CHOOSE_NEXUS;
        break;
    default:
        break;
    }
}

static void render_chooser(int rem_s) {
    draw_nx_backdrop(&canvas, 1);
    draw_nx_cut_panel(&canvas, 16, 18, 608, 324, 16, 1, 0);
    draw_nx_sprite(&canvas, 44, 96, NX_MARK_96_W, NX_MARK_96_H, nx_mark_96_rgb, nx_mark_96_a);
    fb_text(&canvas, &FONT_SMALL, 36, 210, "GOOGLE GLASS XE", C_LABEL, 1);
    fb_text(&canvas, &FONT_SMALL, 36, 230, "DUAL-BOOT STAGE 2", C_DIM, 1);

    fb_text(&canvas, &FONT_KICKER, 174, 34, "PRISM BOOT CHOOSER  ·  SELECT OS", C_AMBER, 1);
    fb_text(&canvas, &FONT_TITLE,  174, 62, "Which System?", C_TEXT, 1);
    draw_hairline(&canvas, 174, 102, 426);

    /* Option 1: NexusOS (Prism Shell & Apps) */
    draw_nx_cut_panel(&canvas, 174, 116, 426, 76, 10, g_chooser_sel == 0, 0);
    draw_nx_diamond(&canvas, 192, 136, C_AMBER);
    fb_text(&canvas, &FONT_BODY,  208, 126, "NexusOS  (Prism Shell)", C_TEXT, 1);
    char nsub[80];
    snprintf(nsub, sizeof nsub, "Swipe or wait %d s  ·  Cut-corner glass shell", rem_s);
    fb_text(&canvas, &FONT_SMALL, 208, 160, nsub, C_LABEL, 1);

    /* Option 2: GlassOS (Vox Chibi HUD) */
    draw_nx_cut_panel(&canvas, 174, 204, 426, 76, 10, g_chooser_sel == 1, 1);
    draw_nx_dot(&canvas, 192, 224, C_LABEL);
    fb_text(&canvas, &FONT_BODY,  208, 214, "GlassOS  (Vox Muse HUD)", C_TEXT, 1);
    fb_text(&canvas, &FONT_SMALL, 208, 248, "Tap touchpad now  ·  Chibi Nova & voice HUD", C_LABEL, 1);

    fb_text(&canvas, &FONT_SMALL, 174, 300, "Power button switches OS anytime after boot.", C_DIM, 1);

    if (g_opts->png_dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/local-choose.png", g_opts->png_dir);
        fb_write_png(&canvas, path);
    }
    if (g_opts->fb_path) write_fb(&canvas);
}

static int run_chooser(int secs) {
    open_inputs();
    int64_t end = mono_ms() + (int64_t)secs * 1000;
    int last_rem = -1;
    while (!g_choice && mono_ms() < end) {
        int rem = (int)((end - mono_ms() + 999) / 1000);
        if (rem != last_rem) {
            last_rem = rem;
            render_chooser(rem);
        }
        pump_inputs(50, on_choose, NULL);
    }
    if (!g_choice) g_choice = CHOOSE_NEXUS;
    printf("%s\n", g_choice == CHOOSE_VOX ? "vox" : "nexus");
    fflush(stdout);
    return g_choice;
}

/* ---- Optional Non-Blocking NEXUS-LINK v1 Session ---------------------------------- */
static int send_hello(session_t *s, const char *nonce) {
    char body[1024];
    jwriter w;
    jw_init(&w, body, sizeof body);
    jw_obj_begin(&w);
    jw_key(&w, "node"); jw_str(&w, g_opts->node);
    jw_key(&w, "firmware"); jw_str(&w, g_opts->firmware);
    jw_key(&w, "link"); jw_int(&w, 1);
    jw_key(&w, "nonce"); jw_str(&w, nonce);
    jw_key(&w, "profile");
    jw_obj_begin(&w);
    jw_key(&w, "screen"); jw_obj_begin(&w); jw_key(&w, "w"); jw_int(&w, FB_W); jw_key(&w, "h"); jw_int(&w, FB_H); jw_obj_end(&w);
    jw_key(&w, "inputs"); jw_arr_begin(&w); jw_str(&w, "touch"); jw_str(&w, "press"); jw_arr_end(&w);
    jw_key(&w, "tiers"); jw_arr_begin(&w); jw_str(&w, "lua"); jw_arr_end(&w);
    jw_key(&w, "audio"); jw_bool(&w, 1);
    jw_key(&w, "glow"); jw_bool(&w, 0);
    jw_key(&w, "link"); jw_int(&w, 1);
    jw_key(&w, "modes"); jw_arr_begin(&w); jw_str(&w, "remote"); jw_str(&w, "mirror"); jw_str(&w, "ambient"); jw_arr_end(&w);
    jw_obj_end(&w);
    jw_key(&w, "modes"); jw_arr_begin(&w); jw_str(&w, "remote"); jw_str(&w, "mirror"); jw_str(&w, "ambient"); jw_arr_end(&w);
    jw_obj_end(&w);
    return send_op(s, "hello", body, 0);
}

static int send_presence(session_t *s) {
    char body[200];
    snprintf(body, sizeof body, "{\"state\":\"online\",\"uptime_s\":%lld,\"battery_mv\":%d,\"rssi\":%d}",
             (long long)(mono_ms() / 1000), s->bat_mv, s->wifi_rssi);
    return send_op(s, "presence", body, host_now(s));
}

static jnode *recv_frame(session_t *s, int timeout_ms, int *why) {
    static char buf[LINK_MAX_FRAME + 1];
    for (;;) {
        int n = ws_recv_text(&s->ws, buf, sizeof buf, timeout_ms);
        if (n == -2) { *why = -2; return NULL; }
        if (n == 0) { *why = 0; return NULL; }
        if (n < 0) { *why = -1; return NULL; }
        jnode *f = NULL;
        int64_t now = s->have_clock ? host_now(s) : -1;
        link_err e = link_decode(buf, (size_t)n, s->key, now, &s->ring, "host", LINK_FROM_HOST, &f);
        if (e != LINK_OK) continue;
        return f;
    }
}

static int handle_host_frame(session_t *s, const jnode *f) {
    const char *op = json_str(f, "op", "");
    const jnode *body = json_get(f, "body");
    if (!strcmp(op, "ping")) {
        send_op(s, "pong", "{}", host_now(s));
    } else if (!strcmp(op, "state.patch")) {
        const char *att = json_str(body, "attention", NULL);
        if (att) snprintf(s->attention, sizeof s->attention, "%s", att);
        render_nexusos(s);
    } else if (!strcmp(op, "card.push")) {
        const jnode *card = json_get(body, "card");
        const char *type = json_str(card, "type", "");
        const char *id = json_str(card, "id", "");
        if (!card || card->type != J_OBJ || !*id || !strcmp(type, "approve")) return 0;
        json_free(s->card);
        char tmp[LINK_MAX_FRAME];
        jwriter w;
        jw_init(&w, tmp, sizeof tmp);
        jw_node(&w, card);
        s->card = jw_ok(&w) ? json_parse(tmp, w.len) : NULL;
        snprintf(s->current_card, sizeof s->current_card, "%s", id);
        s->view = (card_view){0, 0};
        s->ui_view = NX_VIEW_REMOTE_CARD;
        render_nexusos(s);
        if (g_opts->ack) send_card_action(s, id, "shown");
    } else if (!strcmp(op, "card.clear")) {
        const char *id = json_str(body, "card_id", "");
        if (!strcmp(id, s->current_card)) {
            json_free(s->card);
            s->card = NULL;
            s->current_card[0] = 0;
            if (s->ui_view == NX_VIEW_REMOTE_CARD) s->ui_view = NX_VIEW_HOME;
            render_nexusos(s);
        }
    }
    return 0;
}

static int try_connect_link(session_t *s) {
    if (!g_opts->host || !g_opts->host[0]) return 0;
    if (ws_connect(&s->ws, g_opts->host, g_opts->port, g_opts->path, 1200)) return 0;
    link_ring_init(&s->ring);
    s->have_clock = 0;
    s->online = 1; /* allow send_hello */
    char nonce[33];
    if (make_hex32(nonce) || send_hello(s, nonce)) { s->online = 0; ws_close(&s->ws, 1011); return 0; }
    int why;
    jnode *welcome = recv_frame(s, HANDSHAKE_TIMEOUT_MS, &why);
    if (!welcome) { s->online = 0; ws_close(&s->ws, 1002); return 0; }
    const jnode *wb = json_get(welcome, "body");
    const char *echo = json_str(wb, "echo", "");
    const char *host_nonce = json_str(wb, "nonce", "");
    int64_t time_ms = json_int(wb, "time_ms", -1);
    if (strcmp(json_str(welcome, "op", ""), "welcome") || strcmp(echo, nonce) || strlen(host_nonce) != 32 || time_ms < 0) {
        json_free(welcome); s->online = 0; ws_close(&s->ws, 1002); return 0;
    }
    s->clock_offset = time_ms - mono_ms();
    s->have_clock = 1;
    char ready_body[80];
    snprintf(ready_body, sizeof ready_body, "{\"nonce\":\"%s\"}", host_nonce);
    json_free(welcome);
    if (send_op(s, "ready", ready_body, host_now(s))) { s->online = 0; ws_close(&s->ws, 1011); return 0; }
    jnode *patch = recv_frame(s, HANDSHAKE_TIMEOUT_MS, &why);
    if (!patch || strcmp(json_str(patch, "op", ""), "state.patch")) {
        if (patch) json_free(patch);
        s->online = 0; ws_close(&s->ws, 1002); return 0;
    }
    handle_host_frame(s, patch);
    json_free(patch);
    send_presence(s);
    render_nexusos(s);
    return 1;
}

int main(int argc, char **argv) {
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 0);
    tzset();
    static opts_t o = {
        .host = "192.168.0.233",
        .port = "7872",
        .path = "/link",
        .node = "prism",
        .firmware = VERSION_STR,
        .wlan = "wlan0",
        .timeout_ms = 2500,
        .presence_s = 60
    };
    const char *token_file = NULL, *token_env = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-v")) o.verbose = 1;
        else if (!strcmp(a, "--ack")) o.ack = 1;
        else if (!v) break;
        else if (!strcmp(a, "--host")) o.host = v, i++;
        else if (!strcmp(a, "--port")) o.port = v, i++;
        else if (!strcmp(a, "--path")) o.path = v, i++;
        else if (!strcmp(a, "--node")) o.node = v, i++;
        else if (!strcmp(a, "--firmware")) o.firmware = v, i++;
        else if (!strcmp(a, "--png-dir")) o.png_dir = v, i++;
        else if (!strcmp(a, "--fb")) o.fb_path = v, i++;
        else if (!strcmp(a, "--fb-format")) o.fb_format = v, i++;
        else if (!strcmp(a, "--exit-after")) o.exit_after = v, i++;
        else if (!strcmp(a, "--timeout-ms")) o.timeout_ms = atoi(v), i++;
        else if (!strcmp(a, "--token-file")) token_file = v, i++;
        else if (!strcmp(a, "--token-env")) token_env = v, i++;
        else if (!strcmp(a, "--input-dir")) o.input_dir = v, i++;
        else if (!strcmp(a, "--cap-dir")) o.cap_dir = v, i++;
        else if (!strcmp(a, "--wlan")) o.wlan = v, i++;
        else if (!strcmp(a, "--presence-s")) o.presence_s = atoi(v), i++;
        else if (!strcmp(a, "--choose")) o.choose_s = atoi(v), i++;
        else if (!strcmp(a, "--view")) o.preview_view = v, i++;
    }
    g_opts = &o;

    if (o.choose_s > 0) {
        return run_chooser(o.choose_s);
    }

    static session_t s;
    memset(&s, 0, sizeof s);
    s.ui_view = NX_VIEW_HOME;
    s.focus_rem_s = s.focus_total_s = 25 * 60;

    if (o.preview_view) {
        if (!strcmp(o.preview_view, "home")) s.ui_view = NX_VIEW_HOME;
        else if (!strcmp(o.preview_view, "launcher")) s.ui_view = NX_VIEW_LAUNCHER;
        else if (!strcmp(o.preview_view, "ops")) s.ui_view = NX_VIEW_OPS;
        else if (!strcmp(o.preview_view, "brief")) s.ui_view = NX_VIEW_BRIEF;
        else if (!strcmp(o.preview_view, "nova")) s.ui_view = NX_VIEW_NOVA;
        else if (!strcmp(o.preview_view, "focus")) s.ui_view = NX_VIEW_FOCUS;
        else if (!strcmp(o.preview_view, "aria")) s.ui_view = NX_VIEW_ARIA;
        else if (!strcmp(o.preview_view, "system")) s.ui_view = NX_VIEW_SYSTEM;
        else if (!strcmp(o.preview_view, "saver")) s.ui_view = NX_VIEW_SAVER;
    }

    int have_token = 0;
    if (token_file || token_env) {
        char *tok = NULL;
        size_t tlen = 0;
        if (token_file) tok = read_file(token_file, 4096, &tlen);
        else if (token_env) {
            const char *e = getenv(token_env);
            if (e) { tok = strdup(e); tlen = strlen(tok); }
        }
        if (tok) {
            while (tlen && (tok[tlen - 1] == '\n' || tok[tlen - 1] == '\r' || tok[tlen - 1] == ' ')) tlen--;
            if (tlen > 0) {
                link_derive_key(tok, tlen, s.key);
                have_token = 1;
            }
            memset(tok, 0, tlen);
            free(tok);
        }
    }

    open_inputs();
    refresh_telemetry(&s, 1);
    render_nexusos(&s);

    if (o.exit_after && !strcmp(o.exit_after, "render")) {
        return 0;
    }

    int64_t last_conn_attempt_ms = 0;
    int64_t last_presence_ms = mono_ms();
    int64_t last_rx_ms = mono_ms();

    for (;;) {
        int64_t now = mono_ms();

        /* Background link retry only when /run/glass/enable-link is present so UI is 100% lag-free */
        if (have_token && !s.online && access("/run/glass/enable-link", F_OK) == 0 &&
            (last_conn_attempt_ms == 0 || now - last_conn_attempt_ms >= 15000)) {
            last_conn_attempt_ms = now;
            if (try_connect_link(&s)) {
                last_rx_ms = mono_ms();
                last_presence_ms = mono_ms();
            }
        }

        if (s.online) {
            int why = 0;
            jnode *f = recv_frame(&s, 25, &why);
            if (f) {
                last_rx_ms = mono_ms();
                handle_host_frame(&s, f);
                json_free(f);
            } else if (why == 0 || why == -1 || (mono_ms() - last_rx_ms > PING_SILENCE_MS)) {
                s.online = 0;
                ws_close(&s.ws, 1001);
                render_nexusos(&s);
            }
            if (s.online && o.presence_s > 0 && mono_ms() - last_presence_ms >= (int64_t)o.presence_s * 1000) {
                last_presence_ms = mono_ms();
                send_presence(&s);
            }
            pump_inputs(20, on_gesture, &s);
        } else {
            pump_inputs(45, on_gesture, &s);
        }

        tick_ui(&s);
    }
}
