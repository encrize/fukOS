#include "serial.h"
#include "io.h"
#include "util.h"

#define COM1 0x3F8u

static int g_serial_ready;

static int tx_ready(void) {
    return (inb(COM1 + 5u) & 0x20u) != 0;
}

int serial_init(void) {
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x80u);
    outb(COM1 + 0u, 0x01u);
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x03u);
    outb(COM1 + 2u, 0xC7u);
    outb(COM1 + 4u, 0x0Bu);

    g_serial_ready = inb(COM1 + 5u) != 0xFFu;
    if (g_serial_ready) serial_write("\r\n--- fukOS serial log ---\r\n");
    return g_serial_ready;
}

int serial_ready(void) {
    return g_serial_ready;
}

void serial_putc(char c) {
    if (!g_serial_ready) return;
    if (c == '\n') serial_putc('\r');
    for (uint32_t wait = 0; wait < 1000000u; wait++) {
        if (tx_ready()) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

void serial_write(const char *text) {
    if (!text) return;
    while (*text) serial_putc(*text++);
}

void serial_write_hex(uint32_t value) {
    char text[9];
    khtoa_fixed(value, 8, text);
    serial_write("0x");
    serial_write(text);
}

void serial_write_dec(uint32_t value) {
    char text[16];
    kutoa(value, text);
    serial_write(text);
}

void klog(const char *level, const char *message) {
    serial_write("[");
    serial_write(level ? level : "INFO");
    serial_write("] ");
    serial_write(message ? message : "");
    serial_putc('\n');
}

void klog_hex(const char *level, const char *label, uint32_t value) {
    serial_write("[");
    serial_write(level ? level : "INFO");
    serial_write("] ");
    serial_write(label ? label : "value");
    serial_write("=");
    serial_write_hex(value);
    serial_putc('\n');
}
