#include "image.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

int image_parse(const uint8_t *buf, uint32_t size, image_t *out) {
    if (size < 16) return 0;
    if (!(buf[0] == 'I' && buf[1] == 'M' && buf[2] == 'G' && buf[3] == '1'))
        return 0;
    uint32_t w   = rd32(buf + 4);
    uint32_t h   = rd32(buf + 8);
    uint32_t bpp = rd32(buf + 12);
    if (bpp != 32) return 0;
    if ((uint64_t)16 + (uint64_t)w * h * 4 > size) return 0;
    out->width  = w;
    out->height = h;
    out->bpp    = bpp;
    out->pixels = buf + 16;
    return 1;
}

int bmp_parse(const uint8_t *buf, uint32_t size,
              uint8_t *dst, uint32_t dst_size, image_t *out) {
    if (size < 54) return 0;
    if (!(buf[0] == 'B' && buf[1] == 'M')) return 0;

    uint32_t off_bits = rd32(buf + 10);
    uint32_t dib_size = rd32(buf + 14);
    if (dib_size < 40) return 0;

    int32_t  w_signed = (int32_t)rd32(buf + 18);
    int32_t  h_signed = (int32_t)rd32(buf + 22);
    uint16_t bpp      = rd16(buf + 28);
    uint32_t comp     = rd32(buf + 30);

    if (comp != 0) return 0;
    if (bpp != 24 && bpp != 32) return 0;
    if (w_signed <= 0) return 0;

    uint32_t w = (uint32_t)w_signed;
    int top_down = 0;
    uint32_t h;
    if (h_signed < 0) { top_down = 1; h = (uint32_t)(-h_signed); }
    else              { h = (uint32_t)h_signed; }
    if (h == 0) return 0;

    if ((uint64_t)w * h * 4 > dst_size) return 0;

    uint32_t bytes_pp = bpp / 8;
    uint32_t row_raw  = w * bytes_pp;
    uint32_t stride   = (row_raw + 3) & ~3u;
    if ((uint64_t)off_bits + (uint64_t)stride * h > size) return 0;

    for (uint32_t y = 0; y < h; y++) {

        uint32_t src_row = top_down ? y : (h - 1 - y);
        const uint8_t *s = buf + off_bits + (uint64_t)src_row * stride;
        uint8_t *d = dst + (uint64_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = (bytes_pp == 4) ? s[3] : 0xFF;
            s += bytes_pp;
            d += 4;
        }
    }

    out->width  = w;
    out->height = h;
    out->bpp    = bpp;
    out->pixels = dst;
    return 1;
}

int image_decode(const uint8_t *buf, uint32_t size,
                 uint8_t *dst, uint32_t dst_size, image_t *out) {
    if (size >= 4 && buf[0] == 'I' && buf[1] == 'M' &&
        buf[2] == 'G' && buf[3] == '1')
        return image_parse(buf, size, out);
    if (size >= 2 && buf[0] == 'B' && buf[1] == 'M')
        return bmp_parse(buf, size, dst, dst_size, out);
    return 0;
}

int image_probe(const uint8_t *buf, uint32_t size, image_t *out) {
    if (size >= 16 && buf[0] == 'I' && buf[1] == 'M' &&
        buf[2] == 'G' && buf[3] == '1') {
        out->width  = rd32(buf + 4);
        out->height = rd32(buf + 8);
        out->bpp    = rd32(buf + 12);
        out->pixels = 0;
        return 1;
    }
    if (size >= 54 && buf[0] == 'B' && buf[1] == 'M') {
        int32_t h_signed = (int32_t)rd32(buf + 22);
        out->width  = rd32(buf + 18);
        out->height = (h_signed < 0) ? (uint32_t)(-h_signed) : (uint32_t)h_signed;
        out->bpp    = rd16(buf + 28);
        out->pixels = 0;
        return 1;
    }
    return 0;
}
