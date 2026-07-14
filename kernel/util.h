#ifndef UTIL_H
#define UTIL_H
#include <stddef.h>
#include <stdint.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
/* Named separately because Doom provides the global memmove symbol. */
void  *kmemmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);
void   kstrcat(char *dst, const char *src);
void   kutoa(uint32_t v, char *out);
void   khtoa(uint32_t v, char *out);
void   khtoa_fixed(uint32_t v, int digits, char *out);

#endif
