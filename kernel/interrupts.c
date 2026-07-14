#include "interrupts.h"
#include "io.h"
#include "panic.h"
#include "serial.h"

#define IDT_ENTRIES 256
#define PIC1_COMMAND 0x20u
#define PIC1_DATA    0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA    0xA1u
#define PIC_EOI      0x20u
#define PIT_COMMAND  0x43u
#define PIT_CHANNEL0 0x40u
#define PIT_HZ       1193182u
#define TICK_HZ      100u

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry g_idt[IDT_ENTRIES];
static volatile uint32_t g_ticks;
extern void (*interrupt_stub_table[])(void);

static uint16_t current_cs(void) {
    uint16_t value;
    __asm__ volatile ("mov %%cs, %0" : "=r"(value));
    return value;
}

static void idt_set_gate(uint8_t vector, void (*handler)(void), uint16_t selector) {
    uintptr_t address = (uintptr_t)handler;
    g_idt[vector].offset_low = (uint16_t)address;
    g_idt[vector].selector = selector;
    g_idt[vector].zero = 0;
    g_idt[vector].type_attr = 0x8Eu;
    g_idt[vector].offset_high = (uint16_t)(address >> 16);
}

static void pic_remap_and_mask(void) {
    outb(PIC1_COMMAND, 0x11u);
    outb(PIC2_COMMAND, 0x11u);
    outb(PIC1_DATA, 0x20u);
    outb(PIC2_DATA, 0x28u);
    outb(PIC1_DATA, 0x04u);
    outb(PIC2_DATA, 0x02u);
    outb(PIC1_DATA, 0x01u);
    outb(PIC2_DATA, 0x01u);

    outb(PIC1_DATA, 0xFEu);
    outb(PIC2_DATA, 0xFFu);
}

static void pit_init(void) {
    uint32_t divisor = PIT_HZ / TICK_HZ;
    outb(PIT_COMMAND, 0x36u);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

void interrupts_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

void interrupts_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

uint32_t interrupts_ticks(void) {
    return g_ticks;
}

void interrupts_init(void) {
    interrupts_disable();

    for (uint32_t i = 0; i < IDT_ENTRIES; i++) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].zero = 0;
        g_idt[i].type_attr = 0;
        g_idt[i].offset_high = 0;
    }

    uint16_t selector = current_cs();
    for (uint32_t i = 0; i < 48u; i++)
        idt_set_gate((uint8_t)i, interrupt_stub_table[i], selector);

    struct idt_pointer pointer;
    pointer.limit = (uint16_t)(sizeof g_idt - 1u);
    pointer.base = (uint32_t)(uintptr_t)g_idt;
    __asm__ volatile ("lidt %0" : : "m"(pointer));

    pic_remap_and_mask();
    pit_init();
    interrupts_enable();
    klog("INFO", "IDT loaded; PIC remapped; PIT running at 100 Hz");
}

static void pic_eoi(uint32_t vector) {
    if (vector >= 40u) outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

void interrupt_dispatch(interrupt_frame_t *frame) {
    if (!frame) kernel_panic("interrupt without a stack frame");

    if (frame->vector < 32u)
        panic_from_interrupt(frame);

    if (frame->vector >= 32u && frame->vector < 48u) {
        if (frame->vector == 32u) g_ticks++;
        pic_eoi(frame->vector);
        return;
    }

    kernel_panic("unexpected interrupt vector");
}
