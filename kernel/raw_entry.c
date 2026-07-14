#include <stdint.h>
#include "framebuffer.h"
#include "shell.h"
#include "fat.h"
#include "ata.h"

#define BOOTINFO_ADDR 0x00000500u

#define PART_LBA 2048u

/* Entry point for the legacy flat-binary loader. */
void kmain_raw(void) {
    const volatile uint32_t *bi = (const volatile uint32_t *)BOOTINFO_ADDR;
    fb_info fb;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    fb.addr   = (uint8_t *)(uintptr_t)bi[0];
    fb.pitch  = bi[1];
    fb.width  = bi[2];
    fb.height = bi[3];
    fb.bpp    = bi[4];
    uint32_t storage_phys = bi[5];
    uint32_t storage_size = bi[6];
#pragma GCC diagnostic pop

    if (!(ata_present() && fat_mount_ata(PART_LBA))) {
        if (storage_phys != 0)
            fat_mount_mem((const void *)(uintptr_t)storage_phys, storage_size);
    }

    shell_run(&fb, 0, 0);
    for (;;) __asm__ volatile ("hlt");
}
