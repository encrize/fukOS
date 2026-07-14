#include <stdint.h>
#include "render.h"
#include "framebuffer.h"
#include "image.h"
#include "util.h"

extern const unsigned int  demo_image_width;
extern const unsigned int  demo_image_height;
extern const unsigned char demo_image_data[];

#define COL_BG      0x0B0F1A
#define COL_BAR     0x161C2B
#define COL_TITLE   0xEDEDED
#define COL_INFO    0x9FD0FF
#define COL_OK      0x9FE0A0
#define COL_WARN    0xFFD27F
#define COL_FOOT    0x8899AA

static int line_h(void)  { return FB_GLYPH_H + 2; }

static void top_bar(const fb_info *fb, const char *title, const char *info) {
    int lh    = line_h();
    int bar_h = 2 * lh + 12;
    fb_fill_rect(fb, 0, 0, (int)fb->width, bar_h, COL_BAR);
    fb_draw_string(fb, 12, 6,          title, COL_TITLE);
    fb_draw_string(fb, 12, 6 + lh,     info,  COL_INFO);
}

static void center_blit(const fb_info *fb, const image_t *img) {
    int x = ((int)fb->width  - (int)img->width)  / 2;
    int y = ((int)fb->height - (int)img->height) / 2;
    if (y < line_h() * 2 + 16) y = line_h() * 2 + 16;
    fb_blit_bgra(fb, x, y, img->pixels, img->width, img->height);
}

void render_view(const fb_info *fb, const image_t *img,
                 const char *title, const char *caption) {
    char info[96];
    char n[16];
    info[0] = 0;
    kstrcat(info, "Image ");
    kutoa(img->width, n);  kstrcat(info, n); kstrcat(info, "x");
    kutoa(img->height, n); kstrcat(info, n); kstrcat(info, " @ ");
    kutoa(img->bpp, n);    kstrcat(info, n); kstrcat(info, " bpp");

    fb_clear(fb, COL_BG);
    top_bar(fb, title, info);
    center_blit(fb, img);

    if (caption)
        fb_draw_string(fb, 12, (int)fb->height - 2 * line_h() - 8,
                       caption, COL_OK);
    fb_draw_string(fb, 12, (int)fb->height - line_h() - 4,
        "[n]/space next  [p] prev  [q]/Enter back to shell", COL_FOOT);
}

void render_view_zoom(const fb_info *fb, const image_t *img,
                      const char *title, const char *caption,
                      int zoom_pct, int pan_x, int pan_y) {
    if (zoom_pct < 1) zoom_pct = 1;

    char info[112];
    char n[16];
    info[0] = 0;
    kstrcat(info, "Image ");
    kutoa(img->width, n);  kstrcat(info, n); kstrcat(info, "x");
    kutoa(img->height, n); kstrcat(info, n); kstrcat(info, " @ ");
    kutoa(img->bpp, n);    kstrcat(info, n); kstrcat(info, " bpp   zoom ");
    kutoa((uint32_t)zoom_pct, n); kstrcat(info, n); kstrcat(info, "%");

    fb_clear(fb, COL_BG);
    top_bar(fb, title, info);

    int lh     = line_h();
    int top    = 2 * lh + 12;
    int foot   = 2 * lh + 12;
    int area_w = (int)fb->width;
    int area_h = (int)fb->height - top - foot;
    if (area_h < 1) area_h = 1;

    uint32_t dw = img->width  * (uint32_t)zoom_pct / 100u;
    uint32_t dh = img->height * (uint32_t)zoom_pct / 100u;
    if (dw == 0) dw = 1;
    if (dh == 0) dh = 1;

    int x = (area_w - (int)dw) / 2 + pan_x;
    int y = top + (area_h - (int)dh) / 2 + pan_y;

    fb_blit_bgra_scaled(fb, x, y, img->pixels, img->width, img->height,
                        dw, dh, 0, top, area_w, area_h);

    if (caption)
        fb_draw_string(fb, 12, (int)fb->height - 2 * line_h() - 8,
                       caption, COL_OK);
    fb_draw_string(fb, 12, (int)fb->height - line_h() - 4,
        "[+/-] zoom  [w/a/s/d] pan  [f] fit  [n]/[p] next/prev  [q] back",
        COL_FOOT);
}

void render_photo_fullscreen(const fb_info *fb, const image_t *img,
                             int fill_screen, int pan_x, int pan_y) {
    if (!img || !img->pixels || !img->width || !img->height) return;
    uint64_t zw = (uint64_t)fb->width * 100u / img->width;
    uint64_t zh = (uint64_t)fb->height * 100u / img->height;
    uint64_t zoom = fill_screen ? (zw > zh ? zw : zh) : (zw < zh ? zw : zh);
    if (zoom == 0) zoom = 1;
    uint32_t width = (uint32_t)((uint64_t)img->width * zoom / 100u);
    uint32_t height = (uint32_t)((uint64_t)img->height * zoom / 100u);
    if (!width) width = 1;
    if (!height) height = 1;
    int x = ((int)fb->width - (int)width) / 2 + pan_x;
    int y = ((int)fb->height - (int)height) / 2 + pan_y;
    fb_clear(fb, 0x000000);
    fb_blit_bgra_scaled(fb, x, y, img->pixels, img->width, img->height,
                        width, height, 0, 0, (int)fb->width, (int)fb->height);
}

void render_demo(const fb_info *fb) {
    image_t img;
    img.width  = demo_image_width;
    img.height = demo_image_height;
    img.bpp    = 32;
    img.pixels = demo_image_data;

    fb_clear(fb, COL_BG);
    top_bar(fb, "MyOS  -  image viewer", "Built-in demo image");
    center_blit(fb, &img);
    fb_draw_string(fb, 12, (int)fb->height - 2 * line_h() - 8,
                   "No storage disk found - showing the embedded demo image.",
                   COL_WARN);
    fb_draw_string(fb, 12, (int)fb->height - line_h() - 4,
                   "Press any key to return to the shell.", COL_FOOT);
}

void render_message(const fb_info *fb, const char *title, const char *msg) {
    fb_clear(fb, COL_BG);
    top_bar(fb, title, "");
    fb_draw_string(fb, 12, (int)fb->height / 2, msg, COL_WARN);
    fb_draw_string(fb, 12, (int)fb->height - line_h() - 4,
                   "Press any key to return to the shell.", COL_FOOT);
}
