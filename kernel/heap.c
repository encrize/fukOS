#include "heap.h"
#include "util.h"

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    struct heap_block *prev;
    struct heap_block *next;
    uint8_t free;
    uint8_t reserved[3];
} heap_block_t;

#define HEAP_MAGIC 0x48454150u
#define HEAP_ALIGN 16u
#define HEAP_MIN_SPLIT 32u
#define HEAP_HEADER ((uint32_t)((sizeof(heap_block_t) + HEAP_ALIGN - 1u) & ~(HEAP_ALIGN - 1u)))

static uintptr_t g_base;
static uintptr_t g_end;
static heap_block_t *g_first;
static uint32_t g_allocations;

static uint32_t align_size(size_t value) {
    if (value > 0xFFFFFFF0u) return 0;
    return ((uint32_t)value + HEAP_ALIGN - 1u) & ~(HEAP_ALIGN - 1u);
}

int kheap_init(uintptr_t base, uintptr_t end) {
    base = (base + HEAP_ALIGN - 1u) & ~(uintptr_t)(HEAP_ALIGN - 1u);
    end &= ~(uintptr_t)(HEAP_ALIGN - 1u);
    if (end <= base || end - base <= HEAP_HEADER + HEAP_MIN_SPLIT ||
        end - base > 0xFFFFFFFFu) {
        g_base = g_end = 0;
        g_first = 0;
        g_allocations = 0;
        return 0;
    }

    g_base = base;
    g_end = end;
    g_first = (heap_block_t *)base;
    g_first->magic = HEAP_MAGIC;
    g_first->size = (uint32_t)(end - base) - HEAP_HEADER;
    g_first->prev = 0;
    g_first->next = 0;
    g_first->free = 1;
    g_allocations = 0;
    return 1;
}

int kheap_ready(void) {
    return g_first != 0;
}

static void split_block(heap_block_t *block, uint32_t need) {
    if (block->size < need + HEAP_HEADER + HEAP_MIN_SPLIT) return;
    heap_block_t *tail = (heap_block_t *)((uint8_t *)block + HEAP_HEADER + need);
    tail->magic = HEAP_MAGIC;
    tail->size = block->size - need - HEAP_HEADER;
    tail->prev = block;
    tail->next = block->next;
    tail->free = 1;
    if (tail->next) tail->next->prev = tail;
    block->next = tail;
    block->size = need;
}

void *kheap_alloc(size_t bytes) {
    uint32_t need = align_size(bytes ? bytes : 1u);
    if (!g_first || !need) return 0;
    for (heap_block_t *block = g_first; block; block = block->next) {
        if (block->magic != HEAP_MAGIC) return 0;
        if (!block->free || block->size < need) continue;
        split_block(block, need);
        block->free = 0;
        g_allocations++;
        return (uint8_t *)block + HEAP_HEADER;
    }
    return 0;
}

static void merge_next(heap_block_t *block) {
    heap_block_t *next = block->next;
    if (!next || !next->free || next->magic != HEAP_MAGIC) return;
    block->size += HEAP_HEADER + next->size;
    block->next = next->next;
    if (block->next) block->next->prev = block;
}

void kheap_free(void *ptr) {
    if (!ptr || !g_first) return;
    uintptr_t address = (uintptr_t)ptr;
    if (address < g_base + HEAP_HEADER || address >= g_end) return;
    heap_block_t *block = (heap_block_t *)(address - HEAP_HEADER);
    if (block->magic != HEAP_MAGIC || block->free) return;
    block->free = 1;
    if (g_allocations) g_allocations--;
    merge_next(block);
    if (block->prev && block->prev->free) {
        block = block->prev;
        merge_next(block);
    }
}

void *kheap_calloc(size_t count, size_t bytes) {
    if (count && bytes > (size_t)0xFFFFFFFFu / count) return 0;
    size_t total = count * bytes;
    void *ptr = kheap_alloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *kheap_realloc(void *ptr, size_t bytes) {
    if (!ptr) return kheap_alloc(bytes);
    if (!bytes) { kheap_free(ptr); return 0; }
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - HEAP_HEADER);
    if (block->magic != HEAP_MAGIC || block->free) return 0;
    uint32_t need = align_size(bytes);
    if (!need) return 0;
    if (block->size >= need) {
        split_block(block, need);
        return ptr;
    }
    if (block->next && block->next->free &&
        block->size + HEAP_HEADER + block->next->size >= need) {
        merge_next(block);
        split_block(block, need);
        return ptr;
    }
    void *replacement = kheap_alloc(bytes);
    if (!replacement) return 0;
    memcpy(replacement, ptr, block->size);
    kheap_free(ptr);
    return replacement;
}

void kheap_get_info(kheap_info_t *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->base = g_base;
    info->end = g_end;
    info->allocations = g_allocations;
    if (!g_first) return;
    info->total_bytes = (uint32_t)(g_end - g_base);
    for (heap_block_t *block = g_first; block; block = block->next) {
        if (block->magic != HEAP_MAGIC) break;
        if (block->free) {
            info->free_bytes += block->size;
            if (block->size > info->largest_free_bytes)
                info->largest_free_bytes = block->size;
        } else {
            info->used_bytes += block->size;
        }
    }
}
