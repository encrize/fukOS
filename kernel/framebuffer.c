#include "framebuffer.h"
#include "io.h"

extern const unsigned int font_bitmap[95][FB_GLYPH_H];

/* Variable MTRR setup for write-combined framebuffer stores. */
#define MTRR_MSR_CAP        0x0FEu
#define MTRR_MSR_DEF_TYPE   0x2FFu
#define MTRR_MSR_PHYSBASE0  0x200u
#define MTRR_MSR_PHYSMASK0  0x201u
#define MTRR_TYPE_WC        0x01u

int fb_enable_write_combining(const fb_info *fb) {
    uint32_t a, b, c, d;

    cpuid_query(1, &a, &b, &c, &d);
    if (!((d >> 12) & 1u)) return 0;

    uint64_t cap = rdmsr(MTRR_MSR_CAP);
    uint32_t vcnt = (uint32_t)(cap & 0xFFu);
    if (vcnt == 0) return 0;
    if (!((cap >> 10) & 1u)) return 0;

    uint32_t maxphys = 36;
    cpuid_query(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000008u) { cpuid_query(0x80000008u, &a, &b, &c, &d); maxphys = a & 0xFFu; }
    if (maxphys < 32 || maxphys > 52) maxphys = 36;
    uint64_t phys_bits = (maxphys >= 64) ? ~0ULL : (((uint64_t)1 << maxphys) - 1);

    uint64_t base = (uint64_t)(uintptr_t)fb->addr;
    uint64_t need = (uint64_t)fb->pitch * fb->height;
    if (base == 0 || need == 0) return 0;
    if (base < 0x80000000ULL) return 0;
    uint64_t size = 0x1000ULL;
    while (size < need) size <<= 1;
    if (base & (size - 1)) return 0;

    int slot = -1;
    for (uint32_t i = 0; i < vcnt; i++) {
        uint64_t mask = rdmsr(MTRR_MSR_PHYSMASK0 + 2 * i);
        if (!((mask >> 11) & 1u)) { if (slot < 0) slot = (int)i; continue; }
        uint64_t mk = mask & phys_bits & ~0xFFFULL;
        uint64_t bs = rdmsr(MTRR_MSR_PHYSBASE0 + 2 * i) & phys_bits & ~0xFFFULL;
        if (((base & mk) == (bs & mk)) &&
            (((base + size - 1) & mk) == (bs & mk)))
            return 0;
    }
    if (slot < 0) return 0;

    uint64_t newbase = (base & ~0xFFFULL) | MTRR_TYPE_WC;
    uint64_t newmask = ((~(size - 1)) & phys_bits & ~0xFFFULL) | (1ULL << 11);

    __asm__ volatile ("cli");
    uint32_t cr0 = read_cr0();
    write_cr0((cr0 | 0x40000000u) & ~0x20000000u);
    wbinvd();
    uint64_t def = rdmsr(MTRR_MSR_DEF_TYPE);
    wrmsr(MTRR_MSR_DEF_TYPE, def & ~(1ULL << 11));
    wrmsr(MTRR_MSR_PHYSBASE0 + 2 * (uint32_t)slot, newbase);
    wrmsr(MTRR_MSR_PHYSMASK0 + 2 * (uint32_t)slot, newmask);
    wrmsr(MTRR_MSR_DEF_TYPE, def | (1ULL << 11));
    wbinvd();
    write_cr0(cr0);

    return 1;
}

void fb_put_pixel(const fb_info *fb, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height)
        return;
    uint8_t *px = fb->addr + (uint32_t)y * fb->pitch + (uint32_t)x * (fb->bpp / 8);
    if (fb->bpp == 32) {
        *(uint32_t *)px = color;
    } else if (fb->bpp == 24) {
        px[0] = color & 0xFF;
        px[1] = (color >> 8) & 0xFF;
        px[2] = (color >> 16) & 0xFF;
    } else if (fb->bpp == 16) {
        uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
        uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        *(uint16_t *)px = c;
    }
}

void fb_clear(const fb_info *fb, uint32_t color) {
    fb_fill_rect(fb, 0, 0, (int)fb->width, (int)fb->height, color);
}

void fb_fill_rect(const fb_info *fb, int x, int y, int w, int h, uint32_t color) {

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width)  w = (int)fb->width  - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0) return;

    if (fb->bpp == 32) {
        uint8_t *row = fb->addr + (uint32_t)y * fb->pitch + (uint32_t)x * 4;
        for (int j = 0; j < h; j++) {
            uint32_t *p = (uint32_t *)row;
            for (int i = 0; i < w; i++) p[i] = color;
            row += fb->pitch;
        }
        return;
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_put_pixel(fb, x + i, y + j, color);
}

void fb_blit_bgra(const fb_info *fb, int x0, int y0,
                  const uint8_t *pixels, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = pixels + ((uint32_t)y * w + x) * 4;
            uint32_t color = ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
            fb_put_pixel(fb, x0 + (int)x, y0 + (int)y, color);
        }
    }
}

void fb_blit_bgra_scaled(const fb_info *fb, int x0, int y0,
                         const uint8_t *pixels, uint32_t w, uint32_t h,
                         uint32_t dst_w, uint32_t dst_h,
                         int clip_x, int clip_y, int clip_w, int clip_h) {
    if (w == 0 || h == 0 || dst_w == 0 || dst_h == 0) return;

    int cx0 = clip_x, cy0 = clip_y;
    int cx1 = clip_x + clip_w, cy1 = clip_y + clip_h;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > (int)fb->width)  cx1 = (int)fb->width;
    if (cy1 > (int)fb->height) cy1 = (int)fb->height;

    int dx0 = x0 > cx0 ? x0 : cx0;
    int dy0 = y0 > cy0 ? y0 : cy0;
    int dx1 = (x0 + (int)dst_w) < cx1 ? (x0 + (int)dst_w) : cx1;
    int dy1 = (y0 + (int)dst_h) < cy1 ? (y0 + (int)dst_h) : cy1;

    for (int dy = dy0; dy < dy1; dy++) {
        uint32_t sy = (uint32_t)(dy - y0) * h / dst_h;
        if (sy >= h) sy = h - 1;
        const uint8_t *srow = pixels + (uint32_t)sy * w * 4;
        for (int dx = dx0; dx < dx1; dx++) {
            uint32_t sx = (uint32_t)(dx - x0) * w / dst_w;
            if (sx >= w) sx = w - 1;
            const uint8_t *s = srow + (uint32_t)sx * 4;
            uint32_t color = ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
            fb_put_pixel(fb, dx, dy, color);
        }
    }
}

static void fb_draw_bitmap_glyph(const fb_info *fb, int x, int y,
                                 unsigned index, uint32_t color) {
    const unsigned int *g = font_bitmap[index];

    if (fb->bpp == 32 && x >= 0 && y >= 0 &&
        x + FB_GLYPH_W <= (int)fb->width && y + FB_GLYPH_H <= (int)fb->height) {
        uint8_t *row = fb->addr + (uint32_t)y * fb->pitch + (uint32_t)x * 4;
        for (int r = 0; r < FB_GLYPH_H; r++) {
            unsigned int bits = g[r];
            if (bits) {
                uint32_t *p = (uint32_t *)row;
                for (int col = 0; col < FB_GLYPH_W; col++)
                    if (bits & (1u << (FB_GLYPH_W - 1 - col))) p[col] = color;
            }
            row += fb->pitch;
        }
        return;
    }
    for (int row = 0; row < FB_GLYPH_H; row++)
        for (int col = 0; col < FB_GLYPH_W; col++)
            if (g[row] & (1u << (FB_GLYPH_W - 1 - col)))
                fb_put_pixel(fb, x + col, y + row, color);
}

void fb_draw_glyph(const fb_info *fb, int x, int y, uint32_t cp, uint32_t color) {

    if (cp >= 32u && cp <= 126u) {
        fb_draw_bitmap_glyph(fb, x, y, (unsigned)(cp - 32u), color);
        return;
    }

    if (cp >= 0x2800u && cp <= 0x28FFu) {
        unsigned bits = (unsigned)(cp - 0x2800u);
        static const int dcol[8] = {0, 0, 0, 1, 1, 1, 0, 1};
        static const int drow[8] = {0, 1, 2, 0, 1, 2, 3, 3};
        int cw = FB_GLYPH_W / 2;
        int chh = FB_GLYPH_H / 4;
        int dw = cw - 1; if (dw < 1) dw = 1;
        int dh = chh - 1; if (dh < 1) dh = 1;
        for (int i = 0; i < 8; i++) {
            if (!(bits & (1u << i))) continue;
            int px = x + dcol[i] * cw;
            int py = y + drow[i] * chh;
            for (int yy = 0; yy < dh; yy++)
                for (int xx = 0; xx < dw; xx++)
                    fb_put_pixel(fb, px + xx, py + yy, color);
        }
        return;
    }

    if (cp != 32u && cp != 0x2800u)
        fb_draw_bitmap_glyph(fb, x, y, (unsigned)('?' - 32), color);
}

void fb_draw_char(const fb_info *fb, int x, int y, char c, uint32_t color) {
    unsigned char ch = (unsigned char)c;
    if (ch < 32 || ch > 126) return;
    fb_draw_bitmap_glyph(fb, x, y, (unsigned)(ch - 32), color);
}

void fb_draw_string(const fb_info *fb, int x, int y, const char *s, uint32_t color) {
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += FB_GLYPH_H + 2; cx = x; continue; }
        fb_draw_char(fb, cx, y, *s, color);
        cx += FB_GLYPH_W;
    }
}
