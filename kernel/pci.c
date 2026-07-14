#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return ((uint32_t)1 << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(offset & 0xFC));
    return (uint16_t)(v >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(offset & 0xFC));
    return (uint8_t)(v >> ((offset & 3) * 8));
}

int pci_scan(pci_scan_cb cb) {
    int found = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;

            uint8_t header_type = pci_read8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t nfuncs = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < nfuncs; func++) {
                uint16_t v2 = pci_read16((uint8_t)bus, slot, func, 0x00);
                if (v2 == 0xFFFF) continue;

                pci_dev_t d;
                d.bus = (uint8_t)bus; d.slot = slot; d.func = func;
                d.vendor_id = v2;
                d.device_id = pci_read16((uint8_t)bus, slot, func, 0x02);

                uint32_t classreg = pci_read32((uint8_t)bus, slot, func, 0x08);
                d.revision   = (uint8_t)(classreg);
                d.prog_if    = (uint8_t)(classreg >> 8);
                d.subclass   = (uint8_t)(classreg >> 16);
                d.class_code = (uint8_t)(classreg >> 24);
                d.header_type = pci_read8((uint8_t)bus, slot, func, 0x0E);

                if (cb) cb(&d);
                found++;
            }
        }
    }
    return found;
}

typedef struct {
    uint8_t class_code, subclass, prog_if;
    pci_dev_t *out;
    int found;
} find_ctx_t;

static find_ctx_t *g_find_ctx;

static void find_cb(const pci_dev_t *d) {
    if (g_find_ctx->found) return;
    if ((g_find_ctx->class_code == 0xFF || d->class_code == g_find_ctx->class_code) &&
        (g_find_ctx->subclass  == 0xFF || d->subclass  == g_find_ctx->subclass)  &&
        (g_find_ctx->prog_if   == 0xFF || d->prog_if   == g_find_ctx->prog_if)) {
        *g_find_ctx->out = *d;
        g_find_ctx->found = 1;
    }
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_dev_t *out) {
    find_ctx_t ctx = { class_code, subclass, prog_if, out, 0 };
    g_find_ctx = &ctx;
    pci_scan(find_cb);
    g_find_ctx = 0;
    return ctx.found;
}

uint64_t pci_bar_address(uint8_t bus, uint8_t slot, uint8_t func, int bar_index) {
    if (bar_index < 0 || bar_index > 5) return 0;
    uint8_t off = (uint8_t)(0x10 + bar_index * 4);
    uint32_t bar = pci_read32(bus, slot, func, off);
    if (bar == 0) return 0;
    if (bar & 0x1) return 0;

    uint64_t base = bar & 0xFFFFFFF0u;
    uint8_t type = (uint8_t)((bar >> 1) & 0x3);
    if (type == 0x2 && bar_index < 5) {
        uint32_t hi = pci_read32(bus, slot, func, (uint8_t)(off + 4));
        base |= ((uint64_t)hi) << 32;
    }
    return base;
}

void pci_enable_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t combined = pci_read32(bus, slot, func, 0x04);
    uint16_t cmd = (uint16_t)combined;
    cmd |= 0x0002   | 0x0004  ;
    combined = (combined & 0xFFFF0000u) | cmd;
    pci_write32(bus, slot, func, 0x04, combined);
}
