#ifndef IMAGE_H
#define IMAGE_H
#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    const uint8_t *pixels;
} image_t;

/* IMG1 references pixels in place; BMP decoding writes BGRA to dst. */
int image_parse(const uint8_t *buf, uint32_t size, image_t *out);

int bmp_parse(const uint8_t *buf, uint32_t size,
              uint8_t *dst, uint32_t dst_size, image_t *out);

int image_decode(const uint8_t *buf, uint32_t size,
                 uint8_t *dst, uint32_t dst_size, image_t *out);

/* Header-only format and dimension check. */
int image_probe(const uint8_t *buf, uint32_t size, image_t *out);

#endif
