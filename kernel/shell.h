#ifndef SHELL_H
#define SHELL_H
#include <stdint.h>
#include "framebuffer.h"

/* Initialize services and enter the interactive shell. */
void shell_run(const fb_info *fb, uint64_t total_ram_bytes, const uint8_t *acpi_rsdp);

#endif
