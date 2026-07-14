#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

typedef struct interrupt_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp;
    uint32_t ebx, edx, ecx, eax;
    uint32_t vector, error_code;
    uint32_t eip, cs, eflags;
} interrupt_frame_t;

void interrupts_init(void);
void interrupts_enable(void);
void interrupts_disable(void);
uint32_t interrupts_ticks(void);
void interrupt_dispatch(interrupt_frame_t *frame);

#endif
