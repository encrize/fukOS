#include "panic.h"
#include "framebuffer.h"
#include "io.h"
#include "serial.h"
#include "util.h"

static const fb_info *g_panic_fb;
static volatile int g_panicking;

static const char *const EXCEPTION_NAMES[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "BOUND range exceeded", "Invalid opcode", "Device unavailable",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point exception", "Alignment check", "Machine check", "SIMD exception",
    "Virtualization exception", "Control protection exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved"
};

static uint32_t read_cr2(void) {
    uint32_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void halt_forever(void) __attribute__((noreturn));
static void halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

void panic_set_framebuffer(const fb_info *fb) {
    g_panic_fb = fb;
}

static void serial_field(const char *name, uint32_t value) {
    serial_write(name);
    serial_write("=");
    serial_write_hex(value);
    serial_write("  ");
}

static void panic_line(int row, const char *text, uint32_t color) {
    if (!g_panic_fb || !g_panic_fb->addr) return;
    fb_draw_string(g_panic_fb, 24, 24 + row * (FB_GLYPH_H + 4), text, color);
}

static void append_hex(char *line, const char *name, uint32_t value) {
    char hex[9];
    khtoa_fixed(value, 8, hex);
    kstrcat(line, name);
    kstrcat(line, "=0x");
    kstrcat(line, hex);
    kstrcat(line, "  ");
}

static void begin_panic_screen(const char *reason) {
    if (g_panicking) {
        serial_write("\n[PANIC] recursive kernel panic\n");
        halt_forever();
    }
    g_panicking = 1;
    interrupts_disable();

    serial_write("\n================ KERNEL PANIC ================\n");
    serial_write(reason ? reason : "unknown fatal error");
    serial_putc('\n');

    if (g_panic_fb && g_panic_fb->addr) {
        fb_clear(g_panic_fb, 0x180008u);
        panic_line(0, "fukOS KERNEL PANIC", 0xFFFFFFu);
        panic_line(1, reason ? reason : "Unknown fatal error", 0xFF8080u);
    }
}

void kernel_panic(const char *message) {
    begin_panic_screen(message);
    panic_line(3, "CPU halted. See COM1 serial output for diagnostics.", 0xD8E0F0u);
    serial_write("CPU halted.\n");
    halt_forever();
}

void panic_from_interrupt(const interrupt_frame_t *frame) {
    const char *name = "Unknown exception";
    if (frame && frame->vector < 32u) name = EXCEPTION_NAMES[frame->vector];
    begin_panic_screen(name);
    uint32_t interrupted_esp = frame->esp + 20u;

    serial_field("vector", frame->vector);
    serial_field("error", frame->error_code);
    serial_field("cr2", read_cr2());
    serial_putc('\n');
    serial_field("EAX", frame->eax); serial_field("EBX", frame->ebx);
    serial_field("ECX", frame->ecx); serial_field("EDX", frame->edx); serial_putc('\n');
    serial_field("ESI", frame->esi); serial_field("EDI", frame->edi);
    serial_field("EBP", frame->ebp); serial_field("ESP", interrupted_esp); serial_putc('\n');
    serial_field("EIP", frame->eip); serial_field("CS", frame->cs);
    serial_field("EFLAGS", frame->eflags); serial_putc('\n');
    serial_field("DS", frame->ds); serial_field("ES", frame->es);
    serial_field("FS", frame->fs); serial_field("GS", frame->gs); serial_putc('\n');

    char line[160];
    line[0] = 0;
    append_hex(line, "VECTOR", frame->vector);
    append_hex(line, "ERROR", frame->error_code);
    append_hex(line, "CR2", read_cr2());
    panic_line(3, line, 0xFFD080u);

    line[0] = 0;
    append_hex(line, "EAX", frame->eax); append_hex(line, "EBX", frame->ebx);
    append_hex(line, "ECX", frame->ecx); append_hex(line, "EDX", frame->edx);
    panic_line(5, line, 0xFFFFFFu);

    line[0] = 0;
    append_hex(line, "ESI", frame->esi); append_hex(line, "EDI", frame->edi);
    append_hex(line, "EBP", frame->ebp); append_hex(line, "ESP", interrupted_esp);
    panic_line(6, line, 0xFFFFFFu);

    line[0] = 0;
    append_hex(line, "EIP", frame->eip); append_hex(line, "CS", frame->cs);
    append_hex(line, "EFLAGS", frame->eflags);
    panic_line(8, line, 0x80D0FFu);

    line[0] = 0;
    append_hex(line, "DS", frame->ds); append_hex(line, "ES", frame->es);
    append_hex(line, "FS", frame->fs); append_hex(line, "GS", frame->gs);
    panic_line(9, line, 0xD8E0F0u);

    panic_line(11, "CPU halted. Capture the screen or COM1 log before rebooting.", 0xFF8080u);
    serial_write("CPU halted after exception.\n");
    halt_forever();
}
