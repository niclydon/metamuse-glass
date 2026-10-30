#include "fb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_body.h"
#include "font_kicker.h"
#include "font_small.h"
#include "font_title.h"

const fb_font FONT_KICKER = {FONT_KICKER_W, FONT_KICKER_H, FONT_KICKER_BPR, FONT_KICKER_N, font_kicker_cp, &font_kicker_bits[0][0]};
const fb_font FONT_TITLE = {FONT_TITLE_W, FONT_TITLE_H, FONT_TITLE_BPR, FONT_TITLE_N, font_title_cp, &font_title_bits[0][0]};
const fb_font FONT_BODY = {FONT_BODY_W, FONT_BODY_H, FONT_BODY_BPR, FONT_BODY_N, font_body_cp, &font_body_bits[0][0]};
const fb_font FONT_SMALL = {FONT_SMALL_W, FONT_SMALL_H, FONT_SMALL_BPR, FONT_SMALL_N, font_small_cp, &font_small_bits[0][0]};

uint16_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void fb_clear(fb_t *fb, uint16_t color) {
    for (size_t i = 0; i < FB_W * FB_H; i++) fb->px[i] = color;
}

void fb_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color, int fill) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > FB_W ? FB_W : x + w, y1 = y + h > FB_H ? FB_H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            if (fill || yy == y || yy == y + h - 1 || xx == x || xx == x + w - 1) fb->px[yy * FB_W + xx] = color;
}

uint32_t fb_utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int k = 0;
    if (c < 0x80) k = 0;
    else if ((c & 0xE0) == 0xC0) { k = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { k = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { k = 3; c &= 0x07; }
    else { *s += 1; return 0xFFFD; }
    for (int i = 1; i <= k; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += i; return 0xFFFD; }
        c = c << 6 | (p[i] & 0x3F);
    }
    *s += k + 1;
    return c;
}

static const uint8_t *glyph(const fb_font *f, uint32_t cp) {
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cp[mid] == cp) return f->bits + (size_t)mid * (size_t)(f->h * f->bpr);
        if (f->cp[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int fb_text(fb_t *fb, const fb_font *f, int x, int y, const char *utf8, uint16_t color, int scale) {
    if (scale < 1) scale = 1;
    int x0 = x;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = fb_utf8_next(&s);
        const uint8_t *g = glyph(f, cp);
        if (!g) {
            fb_rect(fb, x + scale, y + 2 * scale, (f->w - 2) * scale, (f->h - 4) * scale, color, 0);
        } else {
            for (int row = 0; row < f->h; row++) {
                const uint8_t *r = g + row * f->bpr;
                for (int col = 0; col < f->w; col++) {
                    if (!(r[col >> 3] & (0x80 >> (col & 7)))) continue;
                    int px = x + col * scale, py = y + row * scale;
                    if (scale == 1) {
                        if (px >= 0 && px < FB_W && py >= 0 && py < FB_H) fb->px[py * FB_W + px] = color;
                    } else fb_rect(fb, px, py, scale, scale, color, 1);
                }
            }
        }
        x += f->w * scale;
    }
    return x - x0;
}

int fb_text_width(const fb_font *f, const char *utf8, int scale) {
    if (scale < 1) scale = 1;
    int n = 0;
    const char *s = utf8;
    while (*s) { fb_utf8_next(&s); n++; }
    return n * f->w * scale;
}

int fb_fit(const fb_font *f, const char *utf8, int scale, int max_px, char *out, size_t cap) {
    if (scale < 1) scale = 1;
    int cw = f->w * scale;
    int max_chars = cw > 0 ? max_px / cw : 0;
    int n = 0;
    const char *s = utf8;
    while (*s) { fb_utf8_next(&s); n++; }
    if (n <= max_chars) {
        snprintf(out, cap, "%s", utf8);
        return 0;
    }
    /* keep max_chars-1 characters and append an ellipsis */
    int keep = max_chars > 0 ? max_chars - 1 : 0;
    const char *p = utf8;
    for (int i = 0; i < keep && *p; i++) fb_utf8_next(&p);
    size_t bytes = (size_t)(p - utf8);
    if (bytes + 4 > cap) bytes = cap > 4 ? cap - 4 : 0;
    memcpy(out, utf8, bytes);
    memcpy(out + bytes, "\xE2\x80\xA6", 4);  /* U+2026 */
    return 1;
}

/* ---- PNG ---------------------------------------------------------------- */

static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t hdr[8], crcb[4];
    be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc_update(0, hdr + 4, 4);
    if (len) crc = crc_update(crc, data, len);
    be32(crcb, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(crcb, 1, 4, f) == 4 ? 0 : -1;
}

int fb_write_png(const fb_t *fb, const char *path) {
    crc_init();
    const size_t row = 1 + FB_W * 3, raw_len = row * FB_H;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < FB_H; y++) {
        uint8_t *r = raw + y * row;
        r[0] = 0;  /* filter: none */
        for (int x = 0; x < FB_W; x++) {
            uint16_t p = fb->px[y * FB_W + x];
            uint8_t rr = (uint8_t)((p >> 11) & 31), gg = (uint8_t)((p >> 5) & 63), bb = (uint8_t)(p & 31);
            r[1 + 3 * x] = (uint8_t)(rr << 3 | rr >> 2);
            r[2 + 3 * x] = (uint8_t)(gg << 2 | gg >> 4);
            r[3 + 3 * x] = (uint8_t)(bb << 3 | bb >> 2);
        }
    }
    /* zlib stream: header, stored blocks of <= 65535 bytes, adler32 */
    size_t blocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(zlen);
    if (!z) { free(raw); return -1; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[o++] = off + n >= raw_len ? 1 : 0;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n); o += n;
    }
    be32(z + o, b << 16 | a); o += 4;
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    be32(ihdr, FB_W); be32(ihdr + 4, FB_H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;
    if (!rc) rc = chunk(f, "IHDR", ihdr, 13);
    if (!rc) rc = chunk(f, "IDAT", z, o);
    if (!rc) rc = chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0 ? rc : -1;
}

/* The Glass panel's /dev/fb0 is 640x360, 32 bits per pixel, bytes B G R A, tightly
 * packed (921 600 bytes; measured by glass-os's screenshot path on the unit). The
 * canvas is RGB565, so expand on the way out. */
int fb_write_bgra32(const fb_t *fb, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t *buf = malloc((size_t)FB_W * FB_H * 4);
    if (!buf) { fclose(f); return -1; }
    for (size_t i = 0; i < (size_t)FB_W * FB_H; i++) {
        uint16_t p = fb->px[i];
        /* the same bit-replicating expansion the PNG writer uses, so a frame pulled
         * back from the panel matches the host render pixel for pixel */
        uint8_t rr = (p >> 11) & 0x1F, gg = (p >> 5) & 0x3F, bb = p & 0x1F;
        uint8_t r = (uint8_t)(rr << 3 | rr >> 2), g = (uint8_t)(gg << 2 | gg >> 4), b = (uint8_t)(bb << 3 | bb >> 2);
        buf[4 * i] = b; buf[4 * i + 1] = g; buf[4 * i + 2] = r; buf[4 * i + 3] = 0xFF;
    }
    int rc = fwrite(buf, 1, (size_t)FB_W * FB_H * 4, f) == (size_t)FB_W * FB_H * 4 ? 0 : -1;
    free(buf);
    return fclose(f) == 0 ? rc : -1;
}

int fb_write_raw(const fb_t *fb, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t *buf = malloc(FB_W * FB_H * 2);
    if (!buf) { fclose(f); return -1; }
    for (size_t i = 0; i < FB_W * FB_H; i++) { buf[2 * i] = (uint8_t)fb->px[i]; buf[2 * i + 1] = (uint8_t)(fb->px[i] >> 8); }
    int rc = fwrite(buf, 1, FB_W * FB_H * 2, f) == FB_W * FB_H * 2 ? 0 : -1;
    free(buf);
    return fclose(f) == 0 ? rc : -1;
}

/* ---- the panel ---------------------------------------------------------------- */
/* On the unit, writing /dev/fb0 is not enough on the stock 3.4.94 kernel: the DISPC
 * graphics pipeline must be pointed at page 0 and told to go (glass-os's fb-hello.c
 * learned this on the hardware). The register poke is compiled in only for the device
 * build (-DPRISM_DEVICE); on the host fb_present is the plain file write. */
#ifdef PRISM_DEVICE
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define DISPC_BASE      0x48041000u
#define DISPC_GFX_BA0   0x080u
#define DISPC_GFX_BA1   0x084u
#define DISPC_CONTROL2  0x238u
#define DISPC_GO_LCD2   0x020u
static volatile uint32_t *g_dispc;
static int g_dispc_tried;

static void dispc_latch(const char *fb_path) {
    int fb = open(fb_path, O_RDWR);
    if (fb < 0) return;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(fb, FBIOGET_FSCREENINFO, &fix) == 0 && ioctl(fb, FBIOGET_VSCREENINFO, &var) == 0) {
        var.xoffset = var.yoffset = 0;
        ioctl(fb, FBIOPAN_DISPLAY, &var);
        if (!g_dispc_tried) {
            g_dispc_tried = 1;
            int memfd = open("/dev/mem", O_RDWR | O_SYNC);
            if (memfd >= 0) {
                void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, DISPC_BASE);
                if (p != MAP_FAILED) g_dispc = (volatile uint32_t *)p;
                close(memfd);
            }
        }
        if (g_dispc && fix.smem_start) {
            g_dispc[DISPC_GFX_BA0 / 4] = (uint32_t)fix.smem_start;
            g_dispc[DISPC_GFX_BA1 / 4] = (uint32_t)fix.smem_start;
            g_dispc[DISPC_CONTROL2 / 4] |= DISPC_GO_LCD2;
        }
    }
    close(fb);
}
#endif

int fb_present(const fb_t *fb, const char *fb_path) {
    if (fb_write_bgra32(fb, fb_path)) return -1;
#ifdef PRISM_DEVICE
    dispc_latch(fb_path);
#endif
    return 0;
}
