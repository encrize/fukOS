#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

int serial_init(void);
int serial_ready(void);
void serial_putc(char c);
void serial_write(const char *text);
void serial_write_hex(uint32_t value);
void serial_write_dec(uint32_t value);
void klog(const char *level, const char *message);
void klog_hex(const char *level, const char *label, uint32_t value);

#endif
