#ifndef RENDER_H
#define RENDER_H
#include "framebuffer.h"
#include "image.h"

/* Full-screen image viewer surfaces. */
void render_demo(const fb_info *fb);

void render_view(const fb_info *fb, const image_t *img,
                 const char *title, const char *caption);

void render_view_zoom(const fb_info *fb, const image_t *img,
                      const char *title, const char *caption,
                      int zoom_pct, int pan_x, int pan_y);

void render_photo_fullscreen(const fb_info *fb, const image_t *img,
                             int fill_screen, int pan_x, int pan_y);

void render_message(const fb_info *fb, const char *title, const char *msg);

#endif
