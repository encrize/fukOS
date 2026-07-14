#ifndef PANIC_H
#define PANIC_H

#include "framebuffer.h"
#include "interrupts.h"

void panic_set_framebuffer(const fb_info *fb);
void kernel_panic(const char *message) __attribute__((noreturn));
void panic_from_interrupt(const interrupt_frame_t *frame) __attribute__((noreturn));

#endif
