#include "util.h"

void *memset(void *dst, int c, size_t n) {
    void *ret = dst;
    uint8_t v = (uint8_t)c;
    uint32_t vw = (uint32_t)v * 0x01010101u;
    size_t words = n >> 2, rem = n & 3u;
    __asm__ volatile ("cld; rep stosl" : "+D"(dst), "+c"(words) : "a"(vw) : "memory");
    __asm__ volatile ("rep stosb"      : "+D"(dst), "+c"(rem)   : "a"(vw) : "memory");
    return ret;
}

void *memcpy(void *dst, const void *src, size_t n) {
    void *ret = dst;
    size_t words = n >> 2, rem = n & 3u;
    __asm__ volatile ("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(words) :: "memory");
    __asm__ volatile ("rep movsb"      : "+D"(dst), "+S"(src), "+c"(rem)   :: "memory");
    return ret;
}

void *kmemmove(void *dst, const void *src, size_t n) {
    if (dst == src || n == 0) return dst;
    if ((uintptr_t)dst < (uintptr_t)src) return memcpy(dst, src, n);
    unsigned char *d = (unsigned char *)dst + n - 1;
    const unsigned char *s = (const unsigned char *)src + n - 1;
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void kstrcat(char *dst, const char *src) {
    size_t n = kstrlen(dst);
    size_t i = 0;
    while (src[i]) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

void kutoa(uint32_t v, char *out) {
    char tmp[16];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static char hex_digit(uint32_t v) {
    v &= 0xF;
    return (char)(v < 10 ? ('0' + v) : ('a' + (v - 10)));
}

void khtoa(uint32_t v, char *out) {
    char tmp[8];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0) { tmp[i++] = hex_digit(v); v >>= 4; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

void khtoa_fixed(uint32_t v, int digits, char *out) {
    if (digits > 8) digits = 8;
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = hex_digit(v);
        v >>= 4;
    }
    out[digits] = 0;
}
