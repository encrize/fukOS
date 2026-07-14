#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H
#include <stdint.h>
#include "framebuffer.h"

#define MB2_BOOTLOADER_MAGIC 0x36D76289

/* Extract the boot information used by the kernel. */
int mb2_get_framebuffer(uint32_t mbi_addr, fb_info *fb);

uint64_t mb2_get_total_ram(uint32_t mbi_addr);

/* Pick the largest usable identity-mapped RAM tail above min_addr. */
int mb2_find_heap_range(uint32_t mbi_addr, uintptr_t min_addr,
                        uintptr_t *base, uintptr_t *end);

const uint8_t *mb2_get_acpi_rsdp(uint32_t mbi_addr);

#endif
