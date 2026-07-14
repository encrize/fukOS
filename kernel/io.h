#ifndef IO_H
#define IO_H
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid_query(uint32_t leaf, uint32_t *a, uint32_t *b,
                               uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                              : "a"(leaf), "c"(0));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val),
                                  "d"((uint32_t)(val >> 32)));
}
static inline void wbinvd(void) { __asm__ volatile ("wbinvd" ::: "memory"); }
static inline uint32_t read_cr0(void) {
    uint32_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void write_cr0(uint32_t v) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif
