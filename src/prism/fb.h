/* prismd: the 640x360 RGB565 canvas Prism draws cards and Lua apps into, the
 * embedded Terminus fonts, and two ways out: a PNG (harness, evidence) or raw
 * RGB565 (the device framebuffer). No library: the PNG is written with stored
 * deflate blocks and hand-rolled CRC-32/Adler-32. */
#ifndef PRISM_FB_H
#define PRISM_FB_H
#include <stddef.h>
#include <stdint.h>

#define FB_W 640
#define FB_H 360

typedef struct { uint16_t px[FB_W * FB_H]; } fb_t;

typedef struct {
    int w, h, bpr, n;
    const uint32_t *cp;
    const uint8_t *bits;
} fb_font;

/* Terminus: 10x20 regular (kicker, hints), 16x32 bold (title, status value),
 * 14x28 regular (body), 8x16 regular (footer, Lua text at size 1). */
extern const fb_font FONT_KICKER, FONT_TITLE, FONT_BODY, FONT_SMALL;

uint16_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
void fb_clear(fb_t *fb, uint16_t color);
void fb_rect(fb_t *fb, int x, int y, int w, int h, uint16_t color, int fill);
/* Draw UTF-8 text with the font scaled by an integer factor; returns the advance in px.
 * Codepoints the font lacks draw as a hollow box. */
int fb_text(fb_t *fb, const fb_font *f, int x, int y, const char *utf8, uint16_t color, int scale);
int fb_text_width(const fb_font *f, const char *utf8, int scale);
/* Copy utf8 into out, cut to fit max_px with a trailing ellipsis when it does not;
 * returns 1 when it was cut. */
int fb_fit(const fb_font *f, const char *utf8, int scale, int max_px, char *out, size_t cap);
/* Decode one UTF-8 codepoint; advances *s. Returns U+FFFD on malformed input. */
uint32_t fb_utf8_next(const char **s);
int fb_write_png(const fb_t *fb, const char *path);
int fb_write_raw(const fb_t *fb, const char *path);     /* RGB565 little-endian, 460 800 bytes */
int fb_write_bgra32(const fb_t *fb, const char *path);  /* the Glass /dev/fb0 layout, 921 600 bytes */
/* BGRA32 to the panel; with -DPRISM_DEVICE also pans and latches the OMAP4 DISPC. */
int fb_present(const fb_t *fb, const char *fb_path);
#endif
