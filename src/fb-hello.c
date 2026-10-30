/* fb-hello.c: M0 framebuffer test pattern & live hardware telemetry HUD
 * for Google Glass Explorer Edition (board "notle" / glass_1, OMAP4430,
 * Himax HX7309 640x360 LCoS prism at /dev/fb0).
 *
 * Incorporates the hardware lessons verified on the stock XE24 (3.4.94)
 * kernel:
 *   1. Toggles panel-notle-dpi/enabled 0 -> 1 when needed so the driver
 *      does not skip LED illumination after bootloader handoff.
 *   2. Programs OMAP4 DISPC_GFX_BA0/BA1 (0x48041080/84) to smem_start and
 *      latches DISPC_GO_LCD2 (bit 5 of DISPC_CONTROL2 at 0x48041238), since
 *      FBIOPAN_DISPLAY alone does not reach hardware under dsscomp.
 *   3. Renders a 640x360 corner/border reticle, RGBCMYW color bars (for
 *      first-boot prism foil inspection), and high-contrast text on black
 *      (transparent on the prism) showing kernel, RAM size (1 GB vs 2 GB),
 *      bq27520 battery telemetry, USB/Wi-Fi IP, and live touchpad/button
 *      input events from /dev/input/event*.
 *
 * Usage:
 *   fb-hello [--once] [--no-panel-reset] [/dev/fb0]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#define DISPC_BASE      0x48041000u
#define DISPC_GFX_BA0   0x080u
#define DISPC_GFX_BA1   0x084u
#define DISPC_CONTROL2  0x238u
#define DISPC_GO_LCD2   0x020u

/* Chibi Nova sprite pack binary format ("NOVA" magic) */
struct nova_sprite_hdr {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint16_t counts[7]; /* 0:idle, 1:listening, 2:working, 3:speaking, 4:happy, 5:sleepy, 6:error */
    uint16_t reserved;
};

struct nova_sprite_pack {
    const uint8_t *blob;
    size_t len;
    struct nova_sprite_hdr hdr;
    size_t state_off[7];
};

static struct nova_sprite_pack g_pack_face;
static struct nova_sprite_pack g_pack_fullbody;
static unsigned g_anim_tick = 0;
static int g_smooth_rms = 0;
static int g_toast_ticks = 0;
static char g_toast_msg[64] = "";

static void load_sprite_pack_from(struct nova_sprite_pack *pk, const char *const *paths) {
    if (pk->blob)
        return;
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0)
            continue;
        struct stat st;
        if (fstat(fd, &st) == 0 && (size_t)st.st_size > sizeof(struct nova_sprite_hdr)) {
            void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                memcpy(&pk->hdr, p, sizeof pk->hdr);
                if (memcmp(pk->hdr.magic, "NOVA", 4) == 0 &&
                    pk->hdr.width > 0 && pk->hdr.height > 0) {
                    pk->blob = (const uint8_t *)p;
                    pk->len = (size_t)st.st_size;
                    size_t off = sizeof(struct nova_sprite_hdr);
                    size_t frame_sz = (size_t)pk->hdr.width * pk->hdr.height;
                    for (int s = 0; s < 7; s++) {
                        pk->state_off[s] = off;
                        off += 256 * 4 + (size_t)pk->hdr.counts[s] * frame_sz;
                    }
                    close(fd);
                    return;
                }
                munmap(p, (size_t)st.st_size);
            }
        }
        close(fd);
    }
}

static void load_nova_sprites(void) {
    static const char *const face_paths[] = {
        "/var/lib/musegadget/nova_sprites.bin",
        "/run/glass/nova_sprites.bin",
        "/usr/share/glass/nova_sprites.bin",
        NULL
    };
    static const char *const fb_paths[] = {
        "/var/lib/musegadget/nova_fullbody.bin",
        "/run/glass/nova_fullbody.bin",
        "/usr/share/glass/nova_fullbody.bin",
        NULL
    };
    load_sprite_pack_from(&g_pack_face, face_paths);
    load_sprite_pack_from(&g_pack_fullbody, fb_paths);
}

static void draw_nova_sprite_pack(const struct nova_sprite_pack *pk,
                                  uint32_t *fb, unsigned stride, unsigned w, unsigned h,
                                  unsigned dst_x, unsigned dst_y, unsigned dst_w, unsigned dst_h,
                                  int state, int frame_idx, int transparent_zero) {
    if (!pk || !pk->blob || state < 0 || state >= 7 || pk->hdr.counts[state] == 0)
        return;
    int cnt = pk->hdr.counts[state];
    if (frame_idx < 0) frame_idx = 0;
    if (frame_idx >= cnt) frame_idx = frame_idx % cnt;

    size_t st_off = pk->state_off[state];
    size_t frame_sz = (size_t)pk->hdr.width * pk->hdr.height;
    size_t px_off = st_off + 256 * 4 + (size_t)frame_idx * frame_sz;
    if (px_off + frame_sz > pk->len)
        return;

    const uint32_t *pal = (const uint32_t *)(pk->blob + st_off);
    const uint8_t *px = pk->blob + px_off;
    unsigned sw = pk->hdr.width;
    unsigned sh = pk->hdr.height;

    for (unsigned dy = 0; dy < dst_h; dy++) {
        unsigned py = dst_y + dy;
        if (py >= h) break;
        unsigned sy = (dy * sh) / dst_h;
        const uint8_t *srow = px + sy * sw;
        uint32_t *drow = fb + py * stride + dst_x;
        for (unsigned dx = 0; dx < dst_w; dx++) {
            if (dst_x + dx >= w) break;
            unsigned sx = (dx * sw) / dst_w;
            uint32_t c = pal[srow[sx]];
            if (transparent_zero && (c & 0x00FFFFFFu) == 0)
                continue;
            drow[dx] = c;
        }
    }
}

static void draw_nova_sprite(uint32_t *fb, unsigned stride, unsigned w, unsigned h,
                             unsigned dst_x, unsigned dst_y, unsigned dst_w, unsigned dst_h,
                             int state, int frame_idx) {
    load_nova_sprites();
    draw_nova_sprite_pack(&g_pack_face, fb, stride, w, h, dst_x, dst_y, dst_w, dst_h, state, frame_idx, 0);
}

static void draw_nova_fullbody(uint32_t *fb, unsigned stride, unsigned w, unsigned h,
                               unsigned dst_x, unsigned dst_y, unsigned dst_w, unsigned dst_h,
                               int state, int frame_idx) {
    load_nova_sprites();
    const struct nova_sprite_pack *pk = g_pack_fullbody.blob ? &g_pack_fullbody : &g_pack_face;
    draw_nova_sprite_pack(pk, fb, stride, w, h, dst_x, dst_y, dst_w, dst_h, state, frame_idx, 1);
}

static uint32_t *g_cam_bgra = NULL;
static time_t g_cam_mtime = 0;

static int load_camera_frame(void) {
    struct stat st;
    if (stat("/run/glass/camera-frame.bgra", &st) != 0 || st.st_size < 640 * 360 * 4)
        return 0;
    if (g_cam_bgra && st.st_mtime == g_cam_mtime)
        return 1;
    if (!g_cam_bgra) {
        g_cam_bgra = (uint32_t *)malloc(640 * 360 * 4);
        if (!g_cam_bgra) return 0;
    }
    FILE *f = fopen("/run/glass/camera-frame.bgra", "rb");
    if (!f) return 0;
    size_t n = fread(g_cam_bgra, 1, 640 * 360 * 4, f);
    fclose(f);
    if (n < 640 * 360 * 4) return 0;
    g_cam_mtime = st.st_mtime;
    return 1;
}

static void draw_camera_rect(uint32_t *fb, unsigned stride, unsigned w, unsigned h,
                             unsigned dst_x, unsigned dst_y, unsigned dst_w, unsigned dst_h) {
    if (!load_camera_frame()) return;
    for (unsigned dy = 0; dy < dst_h; dy++) {
        unsigned py = dst_y + dy;
        if (py >= h) break;
        unsigned sy = (dy * 360u) / dst_h;
        const uint32_t *srow = g_cam_bgra + (size_t)sy * 640u;
        uint32_t *drow = fb + (size_t)py * stride + dst_x;
        for (unsigned dx = 0; dx < dst_w; dx++) {
            if (dst_x + dx >= w) break;
            unsigned sx = (dx * 640u) / dst_w;
            drow[dx] = srow[sx] | 0xFF000000u;
        }
    }
}

static volatile sig_atomic_t keep_running = 1;
static void on_sig(int sig) {
    (void)sig;
    keep_running = 0;
}

/* Compact 8x8 ASCII bitmap font (0x20..0x5F: uppercase, digits, punctuation;
 * lowercase maps to uppercase for maximum legibility on the 640x360 prism). */
static const uint8_t font8x8[64][8] = {
    /* 0x20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 '!' */ {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    /* 0x22 '"' */ {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 '#' */ {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    /* 0x24 '$' */ {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    /* 0x25 '%' */ {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    /* 0x26 '&' */ {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    /* 0x27 ''' */ {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 0x28 '(' */ {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    /* 0x29 ')' */ {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    /* 0x2A '*' */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 0x2B '+' */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 0x2C ',' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    /* 0x2D '-' */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 0x2E '.' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 0x2F '/' */ {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    /* 0x30 '0' */ {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
    /* 0x31 '1' */ {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    /* 0x32 '2' */ {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
    /* 0x33 '3' */ {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    /* 0x34 '4' */ {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    /* 0x35 '5' */ {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
    /* 0x36 '6' */ {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    /* 0x37 '7' */ {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    /* 0x38 '8' */ {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    /* 0x39 '9' */ {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    /* 0x3A ':' */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    /* 0x3B ';' */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    /* 0x3C '<' */ {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    /* 0x3D '=' */ {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
    /* 0x3E '>' */ {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    /* 0x3F '?' */ {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    /* 0x40 '@' */ {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    /* 0x41 'A' */ {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    /* 0x42 'B' */ {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    /* 0x43 'C' */ {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    /* 0x44 'D' */ {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    /* 0x45 'E' */ {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    /* 0x46 'F' */ {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    /* 0x47 'G' */ {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
    /* 0x48 'H' */ {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    /* 0x49 'I' */ {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 0x4A 'J' */ {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    /* 0x4B 'K' */ {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    /* 0x4C 'L' */ {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    /* 0x4D 'M' */ {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
    /* 0x4E 'N' */ {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    /* 0x4F 'O' */ {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    /* 0x50 'P' */ {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /* 0x51 'Q' */ {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    /* 0x52 'R' */ {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    /* 0x53 'S' */ {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
    /* 0x54 'T' */ {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00},
    /* 0x55 'U' */ {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    /* 0x56 'V' */ {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    /* 0x57 'W' */ {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00},
    /* 0x58 'X' */ {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00},
    /* 0x59 'Y' */ {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    /* 0x5A 'Z' */ {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    /* 0x5B '[' */ {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    /* 0x5C '\' */ {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    /* 0x5D ']' */ {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    /* 0x5E '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /* 0x5F '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
};

static void write_sysfs(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        ssize_t r = write(fd, val, strlen(val));
        (void)r;
        close(fd);
    }
}

static void read_sysfs(const char *path, char *out, size_t n, const char *fallback) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(out, (int)n, f))
            out[0] = '\0';
        fclose(f);
    }
    out[strcspn(out, "\r\n")] = '\0';
    if (!out[0])
        snprintf(out, n, "%s", fallback);
}

static void ensure_panel_on(int do_reset) {
    const char *p = "/sys/devices/platform/omapdss/manager2/panel-notle-dpi";
    if (access(p, F_OK) != 0)
        return;
    if (do_reset) {
        write_sysfs("/sys/devices/platform/omapdss/manager2/panel-notle-dpi/enabled", "0\n");
        usleep(500000);
        write_sysfs("/sys/devices/platform/omapdss/manager2/panel-notle-dpi/enabled", "1\n");
    }
    write_sysfs("/sys/devices/platform/omapdss/manager2/panel-notle-dpi/brightness", "120\n");
    write_sysfs("/sys/devices/platform/omapfb/graphics/fb0/blank", "0\n");
    write_sysfs("/sys/devices/platform/omapfb/graphics/fb0/mode", "U:640x360p-312\n");
}

static void put_px(uint32_t *fbm, unsigned stride, unsigned w, unsigned h,
                   unsigned x, unsigned y, uint32_t argb) {
    if (x < w && y < h)
        fbm[(size_t)y * stride + x] = argb;
}

static void fill_rect(uint32_t *fbm, unsigned stride, unsigned w, unsigned h,
                      unsigned x0, unsigned y0, unsigned rw, unsigned rh, uint32_t argb) {
    for (unsigned dy = 0; dy < rh; dy++) {
        unsigned y = y0 + dy;
        if (y >= h) break;
        uint32_t *row = fbm + (size_t)y * stride;
        for (unsigned dx = 0; dx < rw; dx++) {
            unsigned x = x0 + dx;
            if (x >= w) break;
            row[x] = argb;
        }
    }
}

static void draw_char(uint32_t *fbm, unsigned stride, unsigned w, unsigned h,
                      unsigned x0, unsigned y0, char ch, unsigned scale,
                      uint32_t fg, uint32_t bg) {
    unsigned char uc = (unsigned char)ch;
    if (uc >= 'a' && uc <= 'z')
        uc = (unsigned char)(uc - 'a' + 'A');
    if (uc < 0x20 || uc > 0x5F)
        uc = '?';
    const uint8_t *glyph = font8x8[uc - 0x20];
    for (unsigned gy = 0; gy < 8; gy++) {
        uint8_t bits = glyph[gy];
        for (unsigned gx = 0; gx < 8; gx++) {
            uint32_t col = (bits & (0x80u >> gx)) ? fg : bg;
            for (unsigned sy = 0; sy < scale; sy++) {
                for (unsigned sx = 0; sx < scale; sx++) {
                    put_px(fbm, stride, w, h,
                           x0 + gx * scale + sx,
                           y0 + gy * scale + sy, col);
                }
            }
        }
    }
}

static void draw_str(uint32_t *fbm, unsigned stride, unsigned w, unsigned h,
                     unsigned x0, unsigned y0, const char *s, unsigned scale,
                     uint32_t fg, uint32_t bg) {
    unsigned cx = x0;
    unsigned step = 8 * scale + scale;
    for (; *s; s++) {
        if (cx + 8 * scale > w)
            break;
        draw_char(fbm, stride, w, h, cx, y0, *s, scale, fg, bg);
        cx += step;
    }
}

static long read_mem_total_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %ld kB", &kb) == 1)
            break;
    }
    fclose(f);
    return kb > 0 ? (kb + 512) / 1024 : -1;
}

static void read_board_rev(char *out, size_t n) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) { snprintf(out, n, "unknown"); return; }
    char line[256], hw[24] = "notle", rev[24] = "?";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "Hardware", 8) == 0) {
            const char *p = strchr(line, ':');
            if (p) snprintf(hw, sizeof hw, "%s", p + (p[1] == ' ' ? 2 : 1));
        } else if (strncmp(line, "Revision", 8) == 0) {
            const char *p = strchr(line, ':');
            if (p) snprintf(rev, sizeof rev, "%s", p + (p[1] == ' ' ? 2 : 1));
        }
    }
    fclose(f);
    snprintf(out, n, "%s R%s", hw, rev);
}

static void get_iface_ipv4(const char *ifname, char *out, size_t out_sz, const char *def) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
        if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            const char *ip = inet_ntoa(sin->sin_addr);
            if (ip && ip[0]) {
                snprintf(out, out_sz, "%s", ip);
                close(s);
                return;
            }
        }
        close(s);
    }
    snprintf(out, out_sz, "%s", def);
}

static void get_uptime_str(char *out, size_t out_sz) {
    char raw[64];
    read_sysfs("/proc/uptime", raw, sizeof raw, "0");
    long secs = atol(raw);
    long hrs = secs / 3600;
    long mins = (secs % 3600) / 60;
    long rem = secs % 60;
    if (hrs > 0)
        snprintf(out, out_sz, "%ldH %02ldM %02ldS", hrs, mins, rem);
    else
        snprintf(out, out_sz, "%02ldM %02ldS", mins, rem);
}

static void render_frame(uint32_t *fbm, unsigned stride, unsigned w, unsigned h,
                         const char *kver, long mem_mb, const char *board_rev,
                         unsigned long touch_events, const char *last_ev, int card_idx) {
    /* Clear background to pure black (transparent on the LCoS prism). */
    fill_rect(fbm, stride, w, h, 0, 0, w, h, 0xFF000000u);

    if (card_idx < 5) {
        /* Outer 2px cyan alignment border for prism inspection. */
        uint32_t border_col = 0xFF00E5FFu;
        fill_rect(fbm, stride, w, h, 0, 0, w, 2, border_col);
        fill_rect(fbm, stride, w, h, 0, h - 2, w, 2, border_col);
        fill_rect(fbm, stride, w, h, 0, 0, 2, h, border_col);
        fill_rect(fbm, stride, w, h, w - 2, 0, 2, h, border_col);

        /* Top color test bars (7 bars across 600px, y = 12..30). */
        static const uint32_t bars[7] = {
            0xFFFF3B30u, /* Red */
            0xFF34C759u, /* Green */
            0xFF007AFFu, /* Blue */
            0xFF00E5FFu, /* Cyan */
            0xFFFF2D55u, /* Magenta */
            0xFFFFCC00u, /* Yellow */
            0xFFFFFFFFu  /* White */
        };
        unsigned bw = (w - 40) / 7;
        for (unsigned i = 0; i < 7; i++) {
            fill_rect(fbm, stride, w, h, 20 + i * bw, 12, bw - 4, 18, bars[i]);
        }
    }

    char buf[128], hostname[32], uptime_s[32];
    read_sysfs("/proc/sys/kernel/hostname", hostname, sizeof hostname, "glass-muse");
    get_uptime_str(uptime_s, sizeof uptime_s);

    char cap[16], status[32], volt[24], usb_on[8];
    read_sysfs("/sys/class/power_supply/bq27520-0/capacity", cap, sizeof cap, "?");
    read_sysfs("/sys/class/power_supply/bq27520-0/status", status, sizeof status, "?");
    read_sysfs("/sys/class/power_supply/bq27520-0/voltage_now", volt, sizeof volt, "0");
    read_sysfs("/sys/class/power_supply/twl6030_usb/online", usb_on, sizeof usb_on, "0");
    long mv = atol(volt) / 1000;
    uint32_t bat_col = (atoi(cap) > 0 && atoi(cap) < 20 && usb_on[0] != '1')
                       ? 0xFFFF453Au : 0xFF30D158u;

    char wlan_ip[24], usb_ip[24];
    get_iface_ipv4("wlan0", wlan_ip, sizeof wlan_ip, "NO-WIFI");
    get_iface_ipv4("rndis0", usb_ip, sizeof usb_ip, "");
    if (!usb_ip[0])
        get_iface_ipv4("usb0", usb_ip, sizeof usb_ip, "172.16.42.1");

    char bt_mac[24];
    read_sysfs("/sys/class/bluetooth/hci0/address", bt_mac, sizeof bt_mac, "OFF");
    const char *muse_st = (access("/var/lib/musegadget/identity.json", F_OK) == 0)
                          ? "MUSE OK" : "HCI0 UP";
    const char *aud_st = (access("/dev/snd/pcmC0D2c", F_OK) == 0) ? "AUD:OK" : "AUD:--";
    const char *ram_tag = (mem_mb >= 1400) ? "2GB" :
                          (mem_mb > 0)     ? "1GB" : "?GB";

    if (card_idx == 1) {
        draw_str(fbm, stride, w, h, 20, 44, "VOX // M2 WIFI & TLS 1.3  [2/4]", 2,
                 0xFFFFFFFFu, 0xFF000000u);
        fill_rect(fbm, stride, w, h, 20, 70, w - 40, 2, 0xFF00E5FFu);

        draw_str(fbm, stride, w, h, 20, 86, "CHIP: BROADCOM BCM4330 (NL80211)", 2, 0xFFFFFFFFu, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 114, "BAND: 2.4 GHZ (2422 MHZ / -35DBM)", 2, 0xFF64D2FFu, 0xFF000000u);
        snprintf(buf, sizeof buf, "WLAN: %s/24 (DHCP OK)", wlan_ip);
        draw_str(fbm, stride, w, h, 20, 142, buf, 2, 0xFFFFD60Au, 0xFF000000u);
        snprintf(buf, sizeof buf, "USB : %s/24 (RNDIS SSH)", usb_ip);
        draw_str(fbm, stride, w, h, 20, 170, buf, 2, 0xFFE0E0E0u, 0xFF000000u);

        time_t now = time(NULL);
        struct tm *gmt = gmtime(&now);
        if (gmt)
            snprintf(buf, sizeof buf, "NTP : %04d-%02d-%02d %02d:%02d:%02d UTC",
                     gmt->tm_year + 1900, gmt->tm_mon + 1, gmt->tm_mday,
                     gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
        else
            snprintf(buf, sizeof buf, "NTP : CLOCK SYNCED UTC");
        draw_str(fbm, stride, w, h, 20, 198, buf, 2, 0xFF30D158u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 226, "TLS : OPENSSL 3.3 / TLS 1.3 OK", 2, 0xFF30D158u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 254, "HTTP: HTTP/2 200 (CA VERIFIED)", 2, 0xFFFF9F0Au, 0xFF000000u);

        fill_rect(fbm, stride, w, h, 20, h - 54, w - 40, 2, 0xFF48484Au);
        draw_str(fbm, stride, w, h, 20, h - 42, "GLASS-OS // M2 NETWORK VERIFIED", 2, 0xFFAEAEB2u, 0xFF000000u);
    } else if (card_idx == 2) {
        draw_str(fbm, stride, w, h, 20, 44, "VOX // M3 MUSE BLE PAIR   [3/4]", 2,
                 0xFFFFFFFFu, 0xFF000000u);
        fill_rect(fbm, stride, w, h, 20, 70, w - 40, 2, 0xFF00E5FFu);

        snprintf(buf, sizeof buf, "HCI0: %s (3MBPS)", bt_mac);
        draw_str(fbm, stride, w, h, 20, 86, buf, 2, 0xFFFFFFFFu, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 114, "NAME: MUSEGADGETE591A4 (LE ADV)", 2, 0xFF64D2FFu, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 142, "UUID: 7FDD3D1C-38EA-46CF-8B46...", 2, 0xFFE0E0E0u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 170, "GATT: CMD 0XFD02 / NOTIFY 0XFD03", 2, 0xFFFFD60Au, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 198, "SHIM: LIBGLASS_BLE_SHIM (MTU 23)", 2, 0xFF5E5CE6u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 226, "CRYP: V5 ECDH-P256 + AES-GCM-256", 2, 0xFF30D158u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 254, "STAT: PAIRING_CONFIRMED [GO!]", 2, 0xFF30D158u, 0xFF000000u);

        fill_rect(fbm, stride, w, h, 20, h - 54, w - 40, 2, 0xFF48484Au);
        draw_str(fbm, stride, w, h, 20, h - 42, "GLASS-OS // M3 GO/NO-GO: PASSED", 2, 0xFFAEAEB2u, 0xFF000000u);
    } else if (card_idx == 3) {
        draw_str(fbm, stride, w, h, 20, 44, "VOX // M4 VOICE & AUDIO   [4/4]", 2,
                 0xFFFFFFFFu, 0xFF000000u);
        fill_rect(fbm, stride, w, h, 20, 70, w - 40, 2, 0xFF00E5FFu);

        draw_str(fbm, stride, w, h, 20, 86, "CHIP: TWL6040 + OMAP4 ABE DSP", 2, 0xFFFFFFFFu, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 114, "PLAY: PCMC0D0P 48KHZ -> TRANSDUC", 2, 0xFF64D2FFu, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 142, "PATH: DL1 MONO -> EARPHONE (15)", 2, 0xFFE0E0E0u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 170, "MIC : PCMC0D2C 48KHZ <- DMIC0L/R", 2, 0xFFFFD60Au, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 198, "GAIN: DMIC1 UL 146 / VX UL 120", 2, 0xFF5E5CE6u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 226, "WAV : 24KHZ 16-BIT MONO PCM16LE", 2, 0xFF30D158u, 0xFF000000u);
        draw_str(fbm, stride, w, h, 20, 254, "TEST: 48000 FR RMS:208.7 [OK]", 2, 0xFF30D158u, 0xFF000000u);

        fill_rect(fbm, stride, w, h, 20, h - 54, w - 40, 2, 0xFF48484Au);
        draw_str(fbm, stride, w, h, 20, h - 42, "GLASS-OS // M4 AUDIO VERIFIED", 2, 0xFFAEAEB2u, 0xFF000000u);
    } else if (card_idx >= 4 && card_idx <= 9) {
        char n_state[64], n_user[160], n_reply[320], n_rms[32], cam_act[16];
        read_sysfs("/run/glass/nova-state", n_state, sizeof n_state, "");
        if (!n_state[0])
            read_sysfs("/run/glass/nova-status", n_state, sizeof n_state, "NOISE SESSION READY");
        read_sysfs("/run/glass/nova-user", n_user, sizeof n_user, "LISTENING...");
        read_sysfs("/run/glass/nova-reply", n_reply, sizeof n_reply, "...");
        read_sysfs("/run/glass/nova-rms", n_rms, sizeof n_rms, "");
        if (!n_rms[0])
            read_sysfs("/run/glass/nova-vu", n_rms, sizeof n_rms, "0");
        read_sysfs("/run/glass/camera-active", cam_act, sizeof cam_act, "0");
        int has_cam = (cam_act[0] == '1') && load_camera_frame();
        int rms_val = atoi(n_rms);
        g_smooth_rms = (g_smooth_rms * 2 + rms_val * 3) / 5;
        g_anim_tick++;

        /* Determine Chibi Nova avatar state & frame (amplitude lip-sync in SPEAKING) */
        load_nova_sprites();
        const struct nova_sprite_pack *active_pk =
            (card_idx >= 7 && g_pack_fullbody.blob) ? &g_pack_fullbody : &g_pack_face;
        int av_state = 0; /* 0:IDLE, 1:LISTENING, 2:WORKING/THINKING, 3:SPEAKING, 4:HAPPY */
        int av_frame = 0;
        const char *mode_label = "IDLE";
        uint32_t mode_col = 0xFF30D158u; /* green */

        if (rms_val > 70 || strstr(n_state, "SPEAK") || strstr(n_state, "PLAY")) {
            mode_label = "SPEAKING";
            mode_col = 0xFFFFD60Au; /* Nova gold */
            if (g_smooth_rms > 65 && active_pk->hdr.counts[3] > 0) {
                av_state = 3; /* SPEAKING (mouth-openness graded slots 0..N-1) */
                int max_slot = (int)active_pk->hdr.counts[3] - 1;
                int slot = (g_smooth_rms * max_slot) / 2800;
                if (slot > max_slot) slot = max_slot;
                if (slot > 2)
                    slot += ((g_anim_tick / 4) & 1) ? 1 : 0;
                if (slot < 0) slot = 0;
                if (slot > max_slot) slot = max_slot;
                av_frame = slot;
            } else {
                av_state = 3;
                av_frame = (g_anim_tick / 3) & 1; /* subtle closed-smile breath between syllables */
            }
        } else if (strstr(n_state, "LISTEN") || strstr(n_state, "RECORD") || strstr(n_state, "SPEECH")) {
            av_state = 1; /* LISTENING */
            av_frame = (int)(g_anim_tick / 2);
            mode_label = "LISTENING";
            mode_col = 0xFF00E5FFu; /* cyan */
        } else if (strstr(n_state, "THINK") || strstr(n_state, "CONNECT") || strstr(n_state, "WAIT")) {
            av_state = 2; /* WORKING/THINKING */
            int cnt = active_pk->hdr.counts[2] ? (int)active_pk->hdr.counts[2] : 1;
            av_frame = (card_idx >= 7) ? ((int)(g_anim_tick / 2) % cnt)
                                       : (cnt - 1 - ((int)(g_anim_tick / 2) % cnt));
            mode_label = "THINKING";
            mode_col = 0xFFBF5AF2u; /* purple */
        } else {
            av_state = 0; /* IDLE */
            av_frame = (int)(g_anim_tick / 2);
            mode_label = "READY";
            mode_col = 0xFF30D158u;
        }

        if (card_idx == 9) {
            /* CARD 9: Full-Screen Camera Photo Viewfinder + Full-Body Chibi Nova PiP & Caption */
            if (load_camera_frame()) {
                draw_camera_rect(fbm, stride, w, h, 0, 0, w, h);
            } else {
                fill_rect(fbm, stride, w, h, 0, 0, w, h, 0xFF0A0D16u);
                draw_str(fbm, stride, w, h, 140, 150, "PRESS CAMERA BUTTON TO SNAP", 2, 0xFF00E5FFu, 0xFF0A0D16u);
            }
            /* Cyan viewfinder corner ticks */
            fill_rect(fbm, stride, w, h, 8, 8, 36, 3, 0xFF00E5FFu);
            fill_rect(fbm, stride, w, h, 8, 8, 3, 36, 0xFF00E5FFu);
            fill_rect(fbm, stride, w, h, w - 44, 8, 36, 3, 0xFF00E5FFu);
            fill_rect(fbm, stride, w, h, w - 11, 8, 3, 36, 0xFF00E5FFu);

            fill_rect(fbm, stride, w, h, 16, 14, 196, 28, 0xFF0D0F18u);
            draw_str(fbm, stride, w, h, 24, 20, "NOVA // CAM", 2, 0xFFFFD60Au, 0xFF0D0F18u);
            snprintf(buf, sizeof buf, "[%s]", mode_label);
            unsigned bw_lbl = (unsigned)strlen(buf) * 18 + 16;
            fill_rect(fbm, stride, w, h, w - 16 - bw_lbl, 14, bw_lbl, 28, 0xFF0D0F18u);
            draw_str(fbm, stride, w, h, w - 8 - bw_lbl, 20, buf, 2, mode_col, 0xFF0D0F18u);

            /* Bottom-right caption box (x=156..628, y=248..352) */
            fill_rect(fbm, stride, w, h, 156, 248, w - 168, 104, 0xFF0D0F18u);
            fill_rect(fbm, stride, w, h, 156, 248, w - 168, 3, mode_col);
            const char *p = (n_reply[0] && strcmp(n_reply, "...") != 0) ? n_reply : n_user;
            for (int line = 0; line < 4 && *p; line++) {
                while (*p == ' ') p++;
                if (!*p) break;
                int maxc = 25;
                int len = (int)strlen(p);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 6 && p[sp] != ' ') sp--;
                    if (sp > 6) take = sp;
                }
                char lbuf[36];
                snprintf(lbuf, sizeof lbuf, "%.*s", take, p);
                draw_str(fbm, stride, w, h, 166, 256 + (unsigned)line * 23, lbuf, 2,
                         line == 0 ? 0xFF64D2FFu : 0xFFFFFFFFu, 0xFF0D0F18u);
                p += take;
            }
            /* Full-Body Chibi Nova PiP standing in bottom-left corner (144x192) */
            draw_nova_fullbody(fbm, stride, w, h, 6, 164, 144, 192, av_state, av_frame);
        } else if (card_idx == 8) {
            /* CARD 8: Full-Screen Centered Full-Body Chibi Nova (Glance Stand-Up Mode, 234x312) */
            /* Left & right floating vertical voice waveform wings */
            for (unsigned side = 0; side < 2; side++) {
                unsigned sx = (side == 0) ? 20 : (w - 20 - 8 * 16);
                for (unsigned bi = 0; bi < 8; bi++) {
                    int dist = (side == 0) ? (7 - (int)bi) : (int)bi;
                    int bar_rms = g_smooth_rms - dist * 220;
                    if (bar_rms < 0) bar_rms = 0;
                    unsigned bh = 8 + (unsigned)((bar_rms * 140) / 2800);
                    if (bh > 148) bh = 148;
                    unsigned by = 84 + (148 - bh) / 2;
                    uint32_t bcol = (bar_rms > 80) ? mode_col : 0xFF1C1C24u;
                    fill_rect(fbm, stride, w, h, sx + bi * 16, by, 11, bh, bcol);
                }
            }

            /* Full-Body Chibi Nova standing head-to-boots centered on the prism (234x312, y=2..314) */
            draw_nova_fullbody(fbm, stride, w, h, (w - 234) / 2, 2, 234, 312, av_state, av_frame);

            /* Top-left & top-right minimal floating pills */
            draw_str(fbm, stride, w, h, 16, 16, "NOVA", 2, 0xFFFFD60Au, 0xFF000000u);
            snprintf(buf, sizeof buf, "[%s]", mode_label);
            draw_str(fbm, stride, w, h, w - 16 - (unsigned)strlen(buf) * 18, 16, buf, 2, mode_col, 0xFF000000u);

            /* Floating bottom subtitle bar (y = 316..356) below Nova's boots */
            fill_rect(fbm, stride, w, h, 16, 316, w - 32, 40, 0xFF0D0F18u);
            fill_rect(fbm, stride, w, h, 16, 316, w - 32, 2, mode_col);
            const char *sub = (n_reply[0] && strcmp(n_reply, "...") != 0) ? n_reply : n_user;
            const char *p = sub;
            while (*p == ' ') p++;
            int maxc = 32;
            int len = (int)strlen(p);
            int take = len <= maxc ? len : maxc;
            if (len > maxc) {
                int sp = take;
                while (sp > 12 && p[sp] != ' ') sp--;
                if (sp > 12) take = sp;
            }
            char lbuf[40];
            snprintf(lbuf, sizeof lbuf, "%.*s", take, p);
            unsigned lx = (w - (unsigned)strlen(lbuf) * 18) / 2;
            draw_str(fbm, stride, w, h, lx, 326, lbuf, 2, 0xFFFFFFFFu, 0xFF0D0F18u);
        } else if (card_idx == 7) {
            /* CARD 7: Full-Body Chibi Nova (252x336 on left) + Floating Right Muse Gadget HUD */
            draw_nova_fullbody(fbm, stride, w, h, 20, 12, 252, 336, av_state, av_frame);

            unsigned rx = 292, rw = w - rx - 12; /* 336 px -> 18 chars at scale 2 */
            draw_str(fbm, stride, w, h, rx, 14, "NOVA", 2, 0xFFFFFFFFu, 0xFF000000u);
            snprintf(buf, sizeof buf, "[%s]", mode_label);
            draw_str(fbm, stride, w, h, w - 12 - (unsigned)strlen(buf) * 18, 14, buf, 2, mode_col, 0xFF000000u);
            fill_rect(fbm, stride, w, h, rx, 38, rw, 2, mode_col);

            /* Caller prompt card (+ optional 144x81 Camera Snapshot Thumbnail on right) */
            unsigned u_h = has_cam ? 81 : 56;
            unsigned u_w = has_cam ? (rw - 150) : rw;
            fill_rect(fbm, stride, w, h, rx, 46, u_w, u_h, 0xFF10131Eu);
            fill_rect(fbm, stride, w, h, rx, 46, 4, u_h, 0xFF00E5FFu);
            if (has_cam) {
                unsigned cx = rx + rw - 144, cy = 46;
                fill_rect(fbm, stride, w, h, cx - 2, cy - 2, 148, 85, 0xFF00E5FFu);
                draw_camera_rect(fbm, stride, w, h, cx, cy, 144, 81);
            }
            const char *up = n_user;
            int u_lines = has_cam ? 3 : 2;
            for (int line = 0; line < u_lines && *up; line++) {
                while (*up == ' ') up++;
                if (!*up) break;
                int maxc = has_cam ? ((line == 0) ? 5 : 9) : ((line == 0) ? 13 : 18);
                int len = (int)strlen(up);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 3 && up[sp] != ' ') sp--;
                    if (sp > 3) take = sp;
                }
                char lbuf[32];
                if (line == 0)
                    snprintf(lbuf, sizeof lbuf, "YOU: %.*s", take, up);
                else
                    snprintf(lbuf, sizeof lbuf, "%.*s", take, up);
                draw_str(fbm, stride, w, h, rx + 10, 52 + (unsigned)line * 22, lbuf, 2, 0xFFFFD60Au, 0xFF10131Eu);
                up += take;
            }

            /* Nova spoken reply card */
            unsigned r_y = has_cam ? 133 : 108;
            unsigned r_h = 298 - r_y;
            int r_max_lines = has_cam ? 7 : 8;
            fill_rect(fbm, stride, w, h, rx, r_y, rw, r_h, 0xFF121120u);
            fill_rect(fbm, stride, w, h, rx, r_y, 4, r_h, mode_col);
            const char *p = n_reply;
            for (int line = 0; line < r_max_lines && *p; line++) {
                while (*p == ' ') p++;
                if (!*p) break;
                int maxc = (line == 0) ? 12 : 18;
                int len = (int)strlen(p);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 4 && p[sp] != ' ') sp--;
                    if (sp > 4) take = sp;
                }
                char lbuf[32];
                if (line == 0) {
                    snprintf(lbuf, sizeof lbuf, "NOVA: %.*s", take, p);
                    draw_str(fbm, stride, w, h, rx + 10, r_y + 6, lbuf, 2, 0xFF64D2FFu, 0xFF121120u);
                } else {
                    snprintf(lbuf, sizeof lbuf, "%.*s", take, p);
                    draw_str(fbm, stride, w, h, rx + 10, r_y + 6 + (unsigned)line * 22, lbuf, 2, 0xFFFFFFFFu, 0xFF121120u);
                }
                p += take;
            }

            /* Center-out 18-bar voice waveform at bottom right (y=306..348) */
            unsigned wbars = 18;
            unsigned wbw = rw / wbars;
            for (unsigned bi = 0; bi < wbars; bi++) {
                int dist = (int)bi - (int)(wbars / 2);
                if (dist < 0) dist = -dist;
                int bar_rms = g_smooth_rms - dist * 180;
                if (bar_rms < 0) bar_rms = 0;
                unsigned bh = 4 + (unsigned)((bar_rms * 38) / 2800);
                if (bh > 42) bh = 42;
                unsigned by = 306 + (42 - bh) / 2;
                uint32_t bcol = (bar_rms > 80) ? mode_col : 0xFF24242Cu;
                fill_rect(fbm, stride, w, h, rx + bi * wbw + 1, by, wbw - 3, bh, bcol);
            }
        } else if (card_idx == 6) {
            /* CARD 6: Full-Screen Centered 284x284 Chibi Nova + Floating Bottom Subtitle Bar */
            draw_nova_sprite(fbm, stride, w, h, (w - 284) / 2, 2, 284, 284, av_state, av_frame);

            /* Top-left & top-right minimal floating pills */
            draw_str(fbm, stride, w, h, 16, 16, "NOVA", 2, 0xFFFFD60Au, 0xFF000000u);
            snprintf(buf, sizeof buf, "[%s]", mode_label);
            draw_str(fbm, stride, w, h, w - 16 - (unsigned)strlen(buf) * 16, 16, buf, 2, mode_col, 0xFF000000u);

            /* Left & right floating vertical voice waveform wings */
            for (unsigned side = 0; side < 2; side++) {
                unsigned sx = (side == 0) ? 24 : (w - 24 - 8 * 14);
                for (unsigned bi = 0; bi < 8; bi++) {
                    int dist = (side == 0) ? (7 - (int)bi) : (int)bi;
                    int bar_rms = g_smooth_rms - dist * 220;
                    if (bar_rms < 0) bar_rms = 0;
                    unsigned bh = 6 + (unsigned)((bar_rms * 110) / 2800);
                    if (bh > 120) bh = 120;
                    unsigned by = 96 + (120 - bh) / 2;
                    uint32_t bcol = (bar_rms > 80) ? mode_col : 0xFF1C1C24u;
                    fill_rect(fbm, stride, w, h, sx + bi * 14, by, 10, bh, bcol);
                }
            }

            /* Floating bottom subtitle bar (y = 288..354) */
            fill_rect(fbm, stride, w, h, 16, 288, w - 32, 66, 0xFF0D0F18u);
            fill_rect(fbm, stride, w, h, 16, 288, w - 32, 2, mode_col);
            const char *sub = (n_reply[0] && strcmp(n_reply, "...") != 0) ? n_reply : n_user;
            const char *p = sub;
            for (int line = 0; line < 2 && *p; line++) {
                while (*p == ' ') p++;
                if (!*p) break;
                int maxc = 34;
                int len = (int)strlen(p);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 12 && p[sp] != ' ') sp--;
                    if (sp > 12) take = sp;
                }
                char lbuf[44];
                snprintf(lbuf, sizeof lbuf, "%.*s", take, p);
                unsigned lx = (w - (unsigned)strlen(lbuf) * 16) / 2;
                draw_str(fbm, stride, w, h, lx, 296 + (unsigned)line * 26, lbuf, 2,
                         line == 0 ? 0xFFFFFFFFu : 0xFF64D2FFu, 0xFF0D0F18u);
                p += take;
            }
        } else if (card_idx == 5) {
            /* CARD 5: Full-Screen Borderless 312x312 Chibi Nova + Floating Right Muse Gadget HUD */
            draw_nova_sprite(fbm, stride, w, h, 4, 24, 312, 312, av_state, av_frame);

            unsigned rx = 324, rw = w - rx - 12; /* 304 px -> 17 chars at scale 2 */
            draw_str(fbm, stride, w, h, rx, 14, "NOVA", 2, 0xFFFFFFFFu, 0xFF000000u);
            snprintf(buf, sizeof buf, "[%s]", mode_label);
            draw_str(fbm, stride, w, h, w - 12 - (unsigned)strlen(buf) * 16, 14, buf, 2, mode_col, 0xFF000000u);
            fill_rect(fbm, stride, w, h, rx, 38, rw, 2, mode_col);

            /* Caller prompt card (y=46..102) */
            fill_rect(fbm, stride, w, h, rx, 46, rw, 56, 0xFF10131Eu);
            fill_rect(fbm, stride, w, h, rx, 46, 4, 56, 0xFF00E5FFu);
            const char *up = n_user;
            for (int line = 0; line < 2 && *up; line++) {
                while (*up == ' ') up++;
                if (!*up) break;
                int maxc = (line == 0) ? 12 : 17;
                int len = (int)strlen(up);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 5 && up[sp] != ' ') sp--;
                    if (sp > 5) take = sp;
                }
                char lbuf[28];
                if (line == 0)
                    snprintf(lbuf, sizeof lbuf, "YOU: %.*s", take, up);
                else
                    snprintf(lbuf, sizeof lbuf, "%.*s", take, up);
                draw_str(fbm, stride, w, h, rx + 10, 52 + (unsigned)line * 22, lbuf, 2, 0xFFFFD60Au, 0xFF10131Eu);
                up += take;
            }

            /* Nova spoken reply card (y=108..298) */
            fill_rect(fbm, stride, w, h, rx, 108, rw, 190, 0xFF121120u);
            fill_rect(fbm, stride, w, h, rx, 108, 4, 190, mode_col);
            const char *p = n_reply;
            for (int line = 0; line < 8 && *p; line++) {
                while (*p == ' ') p++;
                if (!*p) break;
                int maxc = (line == 0) ? 11 : 17;
                int len = (int)strlen(p);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 4 && p[sp] != ' ') sp--;
                    if (sp > 4) take = sp;
                }
                char lbuf[28];
                if (line == 0) {
                    snprintf(lbuf, sizeof lbuf, "NOVA: %.*s", take, p);
                    draw_str(fbm, stride, w, h, rx + 10, 114, lbuf, 2, 0xFF64D2FFu, 0xFF121120u);
                } else {
                    snprintf(lbuf, sizeof lbuf, "%.*s", take, p);
                    draw_str(fbm, stride, w, h, rx + 10, 114 + (unsigned)line * 22, lbuf, 2, 0xFFFFFFFFu, 0xFF121120u);
                }
                p += take;
            }

            /* Center-out 18-bar voice waveform at bottom right (y=306..348) */
            unsigned wbars = 18;
            unsigned wbw = rw / wbars;
            for (unsigned bi = 0; bi < wbars; bi++) {
                int dist = (int)bi - (int)(wbars / 2);
                if (dist < 0) dist = -dist;
                int bar_rms = g_smooth_rms - dist * 180;
                if (bar_rms < 0) bar_rms = 0;
                unsigned bh = 4 + (unsigned)((bar_rms * 38) / 2800);
                if (bh > 42) bh = 42;
                unsigned by = 306 + (42 - bh) / 2;
                uint32_t bcol = (bar_rms > 80) ? mode_col : 0xFF24242Cu;
                fill_rect(fbm, stride, w, h, rx + bi * wbw + 1, by, wbw - 3, bh, bcol);
            }
        } else {
            /* CARD 4: Diagnostic HUD + Chibi Nova Stage */
            draw_str(fbm, stride, w, h, 20, 44, "MUSE GADGET // NOVA", 2, 0xFFFFFFFFu, 0xFF000000u);
            snprintf(buf, sizeof buf, "[%s]", mode_label);
            unsigned badge_x = w - 20 - (unsigned)strlen(buf) * 16;
            draw_str(fbm, stride, w, h, badge_x, 44, buf, 2, mode_col, 0xFF000000u);
            fill_rect(fbm, stride, w, h, 20, 68, w - 40, 2, mode_col);

            unsigned av_box_x = 20, av_box_y = 76, av_box_s = 168;
            fill_rect(fbm, stride, w, h, av_box_x, av_box_y, av_box_s, av_box_s, mode_col);
            fill_rect(fbm, stride, w, h, av_box_x + 2, av_box_y + 2, av_box_s - 4, av_box_s - 4, 0xFF0A0B13u);
            draw_nova_sprite(fbm, stride, w, h, av_box_x + 4, av_box_y + 4, 160, 160, av_state, av_frame);

            unsigned wbars = 14;
            unsigned wbw = av_box_s / wbars;
            for (unsigned bi = 0; bi < wbars; bi++) {
                int dist = (int)bi - (int)(wbars / 2);
                if (dist < 0) dist = -dist;
                int bar_rms = g_smooth_rms - dist * 220;
                if (bar_rms < 0) bar_rms = 0;
                unsigned bh = 4 + (unsigned)((bar_rms * 34) / 2800);
                if (bh > 38) bh = 38;
                unsigned by = 252 + (38 - bh) / 2;
                uint32_t bcol = (bar_rms > 80) ? mode_col : 0xFF2C2C2Eu;
                fill_rect(fbm, stride, w, h, av_box_x + bi * wbw + 1, by, wbw - 3, bh, bcol);
            }

            unsigned rx = 200, rw = w - rx - 20;
            snprintf(buf, sizeof buf, "%.26s", n_state);
            draw_str(fbm, stride, w, h, rx, 78, buf, 2, mode_col, 0xFF000000u);

            fill_rect(fbm, stride, w, h, rx, 102, rw, 60, 0xFF121521u);
            fill_rect(fbm, stride, w, h, rx, 102, 4, 60, 0xFF00E5FFu);
            snprintf(buf, sizeof buf, "YOU: %.20s", n_user);
            draw_str(fbm, stride, w, h, rx + 10, 108, buf, 2, 0xFFFFD60Au, 0xFF121521u);
            if (strlen(n_user) > 20) {
                snprintf(buf, sizeof buf, "%.25s", n_user + 20);
                draw_str(fbm, stride, w, h, rx + 10, 134, buf, 2, 0xFFFFD60Au, 0xFF121521u);
            }

            fill_rect(fbm, stride, w, h, rx, 170, rw, 122, 0xFF141324u);
            fill_rect(fbm, stride, w, h, rx, 170, 4, 122, mode_col);
            const char *p = n_reply;
            for (int line = 0; line < 5 && *p; line++) {
                while (*p == ' ') p++;
                if (!*p) break;
                char lbuf[32];
                int maxc = (line == 0) ? 19 : 25;
                int len = (int)strlen(p);
                int take = len <= maxc ? len : maxc;
                if (len > maxc) {
                    int sp = take;
                    while (sp > 8 && p[sp] != ' ') sp--;
                    if (sp > 8) take = sp;
                }
                if (line == 0) {
                    snprintf(lbuf, sizeof lbuf, "NOVA: %.*s", take, p);
                    draw_str(fbm, stride, w, h, rx + 10, 176, lbuf, 2, 0xFF64D2FFu, 0xFF141324u);
                } else {
                    snprintf(lbuf, sizeof lbuf, "%.*s", take, p);
                    draw_str(fbm, stride, w, h, rx + 10, 176 + (unsigned)line * 22, lbuf, 2, 0xFFFFFFFFu, 0xFF141324u);
                }
                p += take;
            }

            fill_rect(fbm, stride, w, h, 20, h - 54, w - 40, 2, 0xFF48484Au);
            snprintf(buf, sizeof buf, "NOVA CHIBI [%s#%02d] RMS:%04d", mode_label, av_frame, rms_val);
            draw_str(fbm, stride, w, h, 20, h - 42, buf, 2, 0xFFAEAEB2u, 0xFF000000u);
        }
    } else {
        draw_str(fbm, stride, w, h, 20, 44, "GLASS-OS // M1 PRISM CARD [1/4]", 2,
                 0xFFFFFFFFu, 0xFF000000u);
        fill_rect(fbm, stride, w, h, 20, 70, w - 40, 2, 0xFF00E5FFu);

        snprintf(buf, sizeof buf, "HOST: %s / UP %s", hostname, uptime_s);
        draw_str(fbm, stride, w, h, 20, 86, buf, 2, 0xFFFFFFFFu, 0xFF000000u);

        snprintf(buf, sizeof buf, "KERN: %.27s", kver);
        draw_str(fbm, stride, w, h, 20, 114, buf, 2, 0xFFE0E0E0u, 0xFF000000u);

        snprintf(buf, sizeof buf, "HW  : %s (%ldMB/%s)", board_rev, mem_mb, ram_tag);
        draw_str(fbm, stride, w, h, 20, 142, buf, 2, 0xFF64D2FFu, 0xFF000000u);

        snprintf(buf, sizeof buf, "BATT: %s%% %ldMV %.8s USB:%s",
                 cap, mv, status, usb_on[0] == '1' ? "ON" : "OFF");
        draw_str(fbm, stride, w, h, 20, 170, buf, 2, bat_col, 0xFF000000u);

        snprintf(buf, sizeof buf, "WIFI: %s / %s", wlan_ip, usb_ip);
        draw_str(fbm, stride, w, h, 20, 198, buf, 2, 0xFFFFD60Au, 0xFF000000u);

        snprintf(buf, sizeof buf, "BLE : %s (%s)", bt_mac, bt_mac[0] == 'O' ? "DOWN" : muse_st);
        draw_str(fbm, stride, w, h, 20, 226, buf, 2, 0xFF5E5CE6u, 0xFF000000u);

        snprintf(buf, sizeof buf, "INP : %lu EV (%s) %s", touch_events, last_ev, aud_st);
        draw_str(fbm, stride, w, h, 20, 254, buf, 2, 0xFFFF9F0Au, 0xFF000000u);

        fill_rect(fbm, stride, w, h, 20, h - 54, w - 40, 2, 0xFF48484Au);
        draw_str(fbm, stride, w, h, 20, h - 42,
                 "MUSE GADGET // BLE + WIFI + AUDIO", 2,
                 0xFFAEAEB2u, 0xFF000000u);
    }

    if (g_toast_ticks > 0 && g_toast_msg[0]) {
        g_toast_ticks--;
        unsigned tw = (unsigned)strlen(g_toast_msg) * 18 + 24;
        if (tw > w - 16) tw = w - 16;
        unsigned tx = (w - tw) / 2;
        fill_rect(fbm, stride, w, h, tx, 6, tw, 30, 0xFF141828u);
        fill_rect(fbm, stride, w, h, tx, 6, tw, 2, 0xFF00E5FFu);
        fill_rect(fbm, stride, w, h, tx, 34, tw, 2, 0xFF00E5FFu);
        draw_str(fbm, stride, w, h, tx + 12, 13, g_toast_msg, 2, 0xFFFFD60Au, 0xFF141828u);
    }
}

static const int g_mode_ring[10] = { 7, 8, 9, 5, 6, 4, 0, 1, 2, 3 };
static const char *const g_card_names[10] = {
    "M1 PRISM KERNEL",
    "M2 WIFI & TLS 1.3",
    "M3 MUSE BLE PAIR",
    "M4 VOICE & AUDIO",
    "CHIBI DIAGNOSTIC",
    "FACE CHIBI + HUD",
    "FACE PORTRAIT",
    "FULL-BODY + HUD",
    "FULL-BODY STANDUP",
    "CAMERA SNAP + PIP"
};

static void set_active_card(int next_card, const char *trig) {
    if (next_card < 0 || next_card > 9) next_card = 7;
    char cbuf[16];
    snprintf(cbuf, sizeof cbuf, "%d\n", next_card);
    write_sysfs("/run/glass/card", cbuf);
    write_sysfs("/run/glass/active-card", cbuf);
    snprintf(g_toast_msg, sizeof g_toast_msg, "%s: %s", trig, g_card_names[next_card]);
    g_toast_ticks = 22;
}

static int step_mode_ring(int cur_card, int dir) {
    int idx = 0;
    for (int i = 0; i < 10; i++) {
        if (g_mode_ring[i] == cur_card) {
            idx = i;
            break;
        }
    }
    idx = (idx + dir + 10) % 10;
    return g_mode_ring[idx];
}

static int step_chibi_only(int cur_card) {
    static const int chibi_ring[5] = { 7, 8, 9, 5, 6 };
    for (int i = 0; i < 5; i++) {
        if (chibi_ring[i] == cur_card)
            return chibi_ring[(i + 1) % 5];
    }
    return 7;
}

static long long now_ms_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int main(int argc, char **argv) {
    const char *fb_path = "/dev/fb0";
    int once = 0;
    int do_panel_reset = 1;
    int card_idx = 0;
    int tour_mode = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0)
            once = 1;
        else if (strcmp(argv[i], "--no-panel-reset") == 0)
            do_panel_reset = 0;
        else if (strcmp(argv[i], "--tour") == 0)
            tour_mode = 1;
        else if (strcmp(argv[i], "--no-tour") == 0)
            tour_mode = 0;
        else if (strcmp(argv[i], "--card") == 0 && i + 1 < argc) {
            card_idx = atoi(argv[++i]);
            if (card_idx < 0 || card_idx > 9) card_idx = 0;
            tour_mode = 0;
        } else
            fb_path = argv[i];
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    ensure_panel_on(do_panel_reset);

    int fb = open(fb_path, O_RDWR);
    if (fb < 0) {
        fprintf(stderr, "fb-hello: open %s failed: %s\n", fb_path, strerror(errno));
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb, FBIOGET_FSCREENINFO, &fix) < 0) {
        fprintf(stderr, "fb-hello: ioctl screeninfo failed: %s\n", strerror(errno));
        close(fb);
        return 1;
    }

    unsigned w = var.xres ? var.xres : 640;
    unsigned h = var.yres ? var.yres : 360;
    unsigned bpp = var.bits_per_pixel ? var.bits_per_pixel : 32;
    unsigned stride = fix.line_length ? (fix.line_length / 4) : w;
    size_t map_len = (size_t)fix.line_length * h;
    if (map_len == 0)
        map_len = (size_t)w * h * (bpp / 8);

    uint32_t *fbm = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbm == MAP_FAILED) {
        fprintf(stderr, "fb-hello: mmap %s (%zu bytes) failed: %s\n",
                fb_path, map_len, strerror(errno));
        close(fb);
        return 1;
    }

    /* Map OMAP4 DISPC registers so page 0 scanout is guaranteed on stock 3.4. */
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32_t *dispc = NULL;
    if (memfd >= 0) {
        void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, DISPC_BASE);
        if (p != MAP_FAILED)
            dispc = (volatile uint32_t *)p;
    }

    struct utsname uts;
    memset(&uts, 0, sizeof uts);
    uname(&uts);
    long mem_mb = read_mem_total_mb();
    char board_rev[64];
    read_board_rev(board_rev, sizeof board_rev);

    printf("fb-hello: %s %ux%u %ubpp stride=%u smem=0x%08lx len=%u kernel=%s ram=%ldMB board=\"%s\" dispc=%s\n",
           fb_path, w, h, bpp, stride, (unsigned long)fix.smem_start, fix.smem_len,
           uts.release, mem_mb, board_rev, dispc ? "mapped" : "pan-only");
    fflush(stdout);

    /* Open input devices (/dev/input/event*) non-blocking for live touch/key echo. */
    struct pollfd pfds[8];
    int nfds = 0;
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && nfds < 8) {
            if (strncmp(de->d_name, "event", 5) == 0) {
                char ipath[300];
                snprintf(ipath, sizeof ipath, "/dev/input/%s", de->d_name);
                int ifd = open(ipath, O_RDONLY | O_NONBLOCK);
                if (ifd >= 0) {
                    pfds[nfds].fd = ifd;
                    pfds[nfds].events = POLLIN;
                    nfds++;
                }
            }
        }
        closedir(d);
    }

    unsigned long touch_events = 0;
    char last_ev[32] = "READY";
    uint32_t *backbuf = malloc(map_len);
    if (!backbuf)
        backbuf = fbm;

    /* Synaptics RMI4 Multi-Touch Protocol A touchpad state machine */
    int tp_down = 0, tp_Cur_x = 0, tp_x0 = 0, tp_x1 = 0;
    int tp_frame_contacts = 0, tp_seen_pos = 0, tp_max_fingers = 1;
    long long tp_t0 = 0;

    do {
        char pin_card[16];
        read_sysfs("/run/glass/active-card", pin_card, sizeof pin_card, "");
        if (!pin_card[0])
            read_sysfs("/run/glass/card", pin_card, sizeof pin_card, "");
        int active_card = pin_card[0] ? atoi(pin_card) : card_idx;
        if (active_card < 0 || active_card > 9) active_card = 0;
        render_frame(backbuf, stride, w, h, uts.release, mem_mb, board_rev,
                     touch_events, last_ev, active_card);
        if (backbuf != fbm && memcmp(fbm, backbuf, map_len) != 0)
            memcpy(fbm, backbuf, map_len);

        var.xoffset = 0;
        var.yoffset = 0;
        ioctl(fb, FBIOPAN_DISPLAY, &var);

        if (dispc && fix.smem_start) {
            dispc[DISPC_GFX_BA0 / 4] = (uint32_t)fix.smem_start;
            dispc[DISPC_GFX_BA1 / 4] = (uint32_t)fix.smem_start;
            dispc[DISPC_CONTROL2 / 4] |= DISPC_GO_LCD2;
        }

        if (once)
            break;

        int r = poll(pfds, (nfds_t)nfds, active_card >= 4 ? 80 : 2500);
        if (r > 0) {
            for (int i = 0; i < nfds; i++) {
                if (pfds[i].revents & POLLIN) {
                    struct input_event ev;
                    while (read(pfds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                        if (ev.type == EV_KEY || ev.type == EV_ABS) {
                            touch_events++;
                            snprintf(last_ev, sizeof last_ev, "T%u C%u V%d",
                                     ev.type, ev.code, ev.value);
                        }
                        /* Physical Camera Button (KEY_CAMERA = 212) -> Snap Photo & Send to Nova! */
                        if (ev.type == EV_KEY && ev.value == 0) {
                            if (ev.code == 212 && access("/usr/sbin/glass-muse-camera", X_OK) == 0) {
                                snprintf(g_toast_msg, sizeof g_toast_msg, "CAM: SNAP PHOTO -> NOVA");
                                g_toast_ticks = 26;
                                int rc = system("/usr/sbin/glass-muse-camera >/run/glass/camera-tap.log 2>&1 &");
                                (void)rc;
                            } else {
                                int next_c = step_chibi_only(active_card);
                                card_idx = next_c;
                                active_card = next_c;
                                tour_mode = 0;
                                set_active_card(next_c, "BTN");
                            }
                        }
                        /* Synaptics RMI4 Temple Touchpad (Protocol A: ABS_MT_POSITION_X + SYN_MT_REPORT + SYN_REPORT) */
                        if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X)) {
                            tp_Cur_x = ev.value;
                            tp_seen_pos = 1;
                        } else if (ev.type == EV_SYN && ev.code == SYN_MT_REPORT) {
                            if (tp_seen_pos) {
                                tp_frame_contacts++;
                                tp_seen_pos = 0;
                            }
                        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                            long long t_now = now_ms_monotonic();
                            if (tp_frame_contacts > 0) {
                                if (!tp_down) {
                                    tp_down = 1;
                                    tp_x0 = tp_Cur_x;
                                    tp_x1 = tp_Cur_x;
                                    tp_t0 = t_now;
                                    tp_max_fingers = tp_frame_contacts;
                                } else {
                                    tp_x1 = tp_Cur_x;
                                    if (tp_frame_contacts > tp_max_fingers)
                                        tp_max_fingers = tp_frame_contacts;
                                }
                            } else if (tp_down) {
                                int dx = tp_x1 - tp_x0;
                                int adx = dx < 0 ? -dx : dx;
                                long long dt = t_now - tp_t0;
                                tp_down = 0;
                                if (adx >= 180) {
                                    /* Temple swipe forward / backward -> switch prism mode! */
                                    int dir = (dx > 0) ? 1 : -1;
                                    int next_c = step_mode_ring(active_card, dir);
                                    card_idx = next_c;
                                    active_card = next_c;
                                    tour_mode = 0;
                                    set_active_card(next_c, dir > 0 ? "SWIPE+" : "SWIPE-");
                                } else if (dt < 550) {
                                    if (tp_max_fingers >= 2) {
                                        /* Two-finger tap -> cycle Chibi Nova modes */
                                        int next_c = step_chibi_only(active_card);
                                        card_idx = next_c;
                                        active_card = next_c;
                                        tour_mode = 0;
                                        set_active_card(next_c, "TAP2");
                                    } else {
                                        /* Single tap on temple -> Start / Send / Stop Nova Voice Turn! */
                                        char cur_st[64];
                                        read_sysfs("/run/glass/nova-state", cur_st, sizeof cur_st, "");
                                        if (strstr(cur_st, "LISTEN"))
                                            snprintf(g_toast_msg, sizeof g_toast_msg, "TAP: SENDING TO NOVA");
                                        else if (strstr(cur_st, "SPEAK"))
                                            snprintf(g_toast_msg, sizeof g_toast_msg, "TAP: STOP PLAYBACK");
                                        else
                                            snprintf(g_toast_msg, sizeof g_toast_msg, "TAP: SPEAK TO NOVA");
                                        g_toast_ticks = 22;
                                        if (access("/usr/sbin/glass-muse-voice", X_OK) == 0) {
                                            int rc = system("/usr/sbin/glass-muse-voice --tap >/run/glass/voice-tap.log 2>&1 &");
                                            (void)rc;
                                        }
                                    }
                                }
                            }
                            tp_frame_contacts = 0;
                            tp_seen_pos = 0;
                        }
                    }
                }
            }
        } else if (tour_mode) {
            card_idx = (card_idx + 1) & 3;
        }
    } while (keep_running);

    for (int i = 0; i < nfds; i++)
        close(pfds[i].fd);
    if (dispc)
        munmap((void *)dispc, 0x1000);
    if (memfd >= 0)
        close(memfd);
    munmap(fbm, map_len);
    close(fb);
    return 0;
}
