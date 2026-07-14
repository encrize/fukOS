#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H
#include <stdint.h>

/* Bitmap font cell dimensions. */
#define FB_GLYPH_W 10
#define FB_GLYPH_H 20

typedef struct {
    uint8_t *addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} fb_info;

void fb_put_pixel(const fb_info *fb, int x, int y, uint32_t color);
void fb_clear(const fb_info *fb, uint32_t color);
void fb_fill_rect(const fb_info *fb, int x, int y, int w, int h, uint32_t color);

void fb_blit_bgra(const fb_info *fb, int x0, int y0,
                  const uint8_t *pixels, uint32_t w, uint32_t h);

void fb_blit_bgra_scaled(const fb_info *fb, int x0, int y0,
                         const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint32_t dst_w, uint32_t dst_h,
                         int clip_x, int clip_y, int clip_w, int clip_h);

/* Best-effort MTRR write combining for the linear framebuffer. */
int fb_enable_write_combining(const fb_info *fb);

void fb_draw_char(const fb_info *fb, int x, int y, char c, uint32_t color);

void fb_draw_glyph(const fb_info *fb, int x, int y, uint32_t cp, uint32_t color);
void fb_draw_string(const fb_info *fb, int x, int y, const char *s, uint32_t color);

#endif
