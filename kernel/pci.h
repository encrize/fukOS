#ifndef PCI_H
#define PCI_H
#include <stdint.h>

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
} pci_dev_t;

/* Legacy PCI configuration mechanism #1. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

typedef void (*pci_scan_cb)(const pci_dev_t *dev);
int pci_scan(pci_scan_cb cb);

/* Device discovery and MMIO BAR setup. */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_dev_t *out);

uint64_t pci_bar_address(uint8_t bus, uint8_t slot, uint8_t func, int bar_index);

void pci_enable_device(uint8_t bus, uint8_t slot, uint8_t func);

#endif
