#include <stdint.h>
#include "multiboot2.h"
#include "framebuffer.h"
#include "shell.h"
#include "util.h"
#include "heap.h"
#include "interrupts.h"
#include "panic.h"
#include "serial.h"

/* Restore linker-isolated Doom globals before every launch. */
extern uint8_t __doom_data_start[], __doom_data_end[];
extern uint8_t __doom_bss_start[],  __doom_bss_end[];

static uint8_t  doom_data_pristine[256u * 1024u];
static int      doom_data_saved;
static uint32_t doom_data_len;

void doom_reset_state(void) {
    uint32_t dlen = (uint32_t)(__doom_data_end - __doom_data_start);
    uint32_t blen = (uint32_t)(__doom_bss_end  - __doom_bss_start);
    if (!doom_data_saved) {

        if (dlen <= sizeof doom_data_pristine) {
            memcpy(doom_data_pristine, __doom_data_start, dlen);
            doom_data_len = dlen;
            doom_data_saved = 1;
        }
        return;
    }

    if (doom_data_len == dlen)
        memcpy(__doom_data_start, doom_data_pristine, dlen);
    memset(__doom_bss_start, 0, blen);
}

void kmain(uint32_t magic, uint32_t mbi_addr) {
    serial_init();
    klog("INFO", "kernel entry");
    if (magic != MB2_BOOTLOADER_MAGIC)
        kernel_panic("invalid Multiboot2 magic");

    fb_info fb;
    if (!mb2_get_framebuffer(mbi_addr, &fb)) {
        kernel_panic("Multiboot2 framebuffer is unavailable");
    }

    panic_set_framebuffer(&fb);
    interrupts_init();
    fb_enable_write_combining(&fb);
    klog("INFO", "framebuffer initialized");
    uint64_t total_ram = mb2_get_total_ram(mbi_addr);

    /* Keep one guard page after the static kernel image, then use the
       largest contiguous usable Multiboot memory-map range as the heap. */
    extern uint8_t _kernel_end;
    uintptr_t min_heap = ((uintptr_t)&_kernel_end + 0xFFFFu) & ~0xFFFFu;
    min_heap += 0x10000u;
    uintptr_t heap_base, heap_end;
    if (mb2_find_heap_range(mbi_addr, min_heap, &heap_base, &heap_end)) {
        if (kheap_init(heap_base, heap_end)) klog("INFO", "kernel heap initialized");
        else klog("WARN", "kernel heap initialization failed");
    } else {
        klog("WARN", "no suitable Multiboot2 heap range");
    }

    const uint8_t *acpi_rsdp = mb2_get_acpi_rsdp(mbi_addr);
    klog("INFO", "starting shell");
    shell_run(&fb, total_ram, acpi_rsdp);
    kernel_panic("shell returned unexpectedly");
}
