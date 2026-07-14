#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t base;
    uintptr_t end;
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t largest_free_bytes;
    uint32_t allocations;
} kheap_info_t;

/* Initialize the kernel heap over one identity-mapped physical range. */
int   kheap_init(uintptr_t base, uintptr_t end);
void *kheap_alloc(size_t bytes);
void *kheap_calloc(size_t count, size_t bytes);
void *kheap_realloc(void *ptr, size_t bytes);
void  kheap_free(void *ptr);
int   kheap_ready(void);
void  kheap_get_info(kheap_info_t *info);

#endif
