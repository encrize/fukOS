#include "multiboot2.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

int mb2_get_framebuffer(uint32_t mbi_addr, fb_info *fb) {
    const uint8_t *base = (const uint8_t *)mbi_addr;
    uint32_t total_size = rd32(base);
    const uint8_t *end = base + total_size;
    const uint8_t *tag = base + 8;

    while (tag + 8 <= end) {
        uint32_t type = rd32(tag);
        uint32_t size = rd32(tag + 4);
        if (type == 0) break;
        if (type == 8) {
            fb->addr   = (uint8_t *)(uintptr_t)rd64(tag + 8);
            fb->pitch  = rd32(tag + 16);
            fb->width  = rd32(tag + 20);
            fb->height = rd32(tag + 24);
            fb->bpp    = tag[28];
            return 1;
        }
        tag += (size + 7) & ~7u;
    }
    return 0;
}

const uint8_t *mb2_get_acpi_rsdp(uint32_t mbi_addr) {
    const uint8_t *base = (const uint8_t *)mbi_addr;
    uint32_t total_size = rd32(base);
    const uint8_t *end = base + total_size;
    const uint8_t *tag = base + 8;
    const uint8_t *v1 = 0;

    while (tag + 8 <= end) {
        uint32_t type = rd32(tag);
        uint32_t size = rd32(tag + 4);
        if (type == 0) break;
        if (type == 15) return tag + 8;
        if (type == 14 && !v1) v1 = tag + 8;
        tag += (size + 7) & ~7u;
    }
    return v1;
}

uint64_t mb2_get_total_ram(uint32_t mbi_addr) {
    const uint8_t *base = (const uint8_t *)mbi_addr;
    uint32_t total_size = rd32(base);
    const uint8_t *end = base + total_size;
    const uint8_t *tag = base + 8;
    uint64_t total = 0;

    while (tag + 8 <= end) {
        uint32_t type = rd32(tag);
        uint32_t size = rd32(tag + 4);
        if (type == 0) break;
        if (type == 6) {
            uint32_t entry_size = rd32(tag + 8);
            const uint8_t *mmap_end = tag + size;
            const uint8_t *entry = tag + 16;
            while (entry_size >= 24 && entry + entry_size <= mmap_end) {
                uint64_t entry_len  = rd64(entry + 8);
                uint32_t entry_type = rd32(entry + 16);
                if (entry_type == 1) total += entry_len;
                entry += entry_size;
            }
        }
        tag += (size + 7) & ~7u;
    }
    return total;
}

int mb2_find_heap_range(uint32_t mbi_addr, uintptr_t min_addr,
                        uintptr_t *heap_base, uintptr_t *heap_end) {
    const uint8_t *base = (const uint8_t *)mbi_addr;
    uint32_t total_size = rd32(base);
    const uint8_t *end = base + total_size;
    const uint8_t *tag = base + 8;
    uint64_t best_base = 0, best_end = 0, best_len = 0;

    while (tag + 8 <= end) {
        uint32_t type = rd32(tag);
        uint32_t size = rd32(tag + 4);
        if (type == 0) break;
        if (type == 6) {
            uint32_t entry_size = rd32(tag + 8);
            const uint8_t *mmap_end = tag + size;
            const uint8_t *entry = tag + 16;
            while (entry_size >= 24 && entry + entry_size <= mmap_end) {
                uint64_t start = rd64(entry);
                uint64_t length = rd64(entry + 8);
                uint32_t entry_type = rd32(entry + 16);
                uint64_t finish = start + length;
                if (finish < start) finish = 0x100000000ULL;
                if (finish > 0xF0000000ULL) finish = 0xF0000000ULL;
                if (entry_type == 1 && finish > min_addr) {
                    if (start < min_addr) start = min_addr;
                    start = (start + 0xFFFFu) & ~0xFFFFULL;
                    finish &= ~0xFFFFULL;
                    if (finish > start && finish - start > best_len) {
                        best_base = start;
                        best_end = finish;
                        best_len = finish - start;
                    }
                }
                entry += entry_size;
            }
        }
        tag += (size + 7) & ~7u;
    }
    if (!best_len || best_base > 0xFFFFFFFFULL || best_end > 0xFFFFFFFFULL)
        return 0;
    *heap_base = (uintptr_t)best_base;
    *heap_end = (uintptr_t)best_end;
    return 1;
}
