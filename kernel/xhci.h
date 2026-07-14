#ifndef XHCI_H
#define XHCI_H
#include <stdint.h>

typedef struct {
    uint64_t mmio_base;
    uint64_t op_base;
    uint64_t rt_base;
    uint64_t db_base;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    uint32_t context_size;
    int      ready;
} xhci_hc_t;

/* Controller and root-port setup. */
int xhci_init(uint64_t mmio_base, xhci_hc_t *out);

typedef void (*xhci_port_cb)(int port_num, uint32_t portsc);
int xhci_scan_ports(const xhci_hc_t *hc, xhci_port_cb cb);

uint32_t xhci_portsc(const xhci_hc_t *hc, int port_num);

typedef void (*xhci_protocol_cb)(int major, int port_offset, int port_count);
void xhci_scan_protocols(const xhci_hc_t *hc, xhci_protocol_cb cb);

typedef struct {
    uint8_t  usb_class;
    uint8_t  usb_subclass;
    uint8_t  usb_proto;
    uint16_t vendor_id;
    uint16_t product_id;
    int      ok;

    uint32_t fail_stage;
    uint32_t fail_detail;

    uint8_t  if_num;
    uint8_t  if_class;
    uint8_t  if_subclass;
    uint8_t  if_proto;
    uint8_t  bulk_in_ep;
    uint8_t  bulk_out_ep;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    uint8_t  config_value;

    uint8_t  storage_ok;
    uint8_t  storage_stage;
    uint8_t  boot_sig_ok;
    char     inq_vendor[9];
    char     inq_product[17];
    uint32_t block_size;
    uint32_t block_count;
    uint8_t  sector0[16];
} xhci_dev_info_t;

int xhci_probe_port(xhci_hc_t *hc, int port_num, xhci_dev_info_t *out);

/* Persistent USB Mass Storage backend used by FAT. */
int xhci_storage_mount(uint64_t bar0);

int xhci_msc_ready(void);

int xhci_msc_read(uint32_t lba, uint32_t count, void *buf);

int xhci_msc_write(uint32_t lba, uint32_t count, const void *buf);

/* Drain unsolicited events while no transfer is active. */
void xhci_idle_drain(void);

uint32_t xhci_msc_block_size(void);
uint32_t xhci_msc_block_count(void);

/* SCSI sense info (key/ASC/ASCQ) captured after the last failed read/write,
   via REQUEST SENSE. 0xFF/0xFF/0xFF if unavailable. Lets callers report
   *why* a transfer failed (e.g. media write-protected) instead of a bare
   "I/O error". */
void xhci_msc_last_sense(uint8_t *key, uint8_t *asc, uint8_t *ascq);

/* Which BOT stage failed last (0=none, 1=CBW, 2=data, 3=CSW recv,
   4=CSW bad signature, 5=CSW phase error) and the raw xHCI completion
   code from that stage, so a wedged pipe can still be diagnosed even if
   REQUEST SENSE itself cannot get through. */
void xhci_msc_last_stage(uint8_t *stage, uint8_t *cc);

/* Completion code of the last Reset Endpoint command and Set TR Dequeue
   Pointer command (0xFE = command itself timed out, no Command Completion
   Event). Lets a caller tell whether the reset commands are even being
   acknowledged by this hardware, separate from whether the pipe still
   works afterwards. */
void xhci_msc_last_reset_result(uint8_t *ep_cc, uint8_t *deq_cc);
void xhci_msc_last_ep_diag(uint8_t *slot_state, uint8_t *ep_out_state, uint8_t *ep_in_state);

/* Raw USBSTS register value captured when a read/write last gave up.
   Bit 0 (HCH) = Host Controller Halted, bit 2 (HSE) = Host System Error.
   If either is set, the whole controller has stopped, not just one
   endpoint. 0xFFFFFFFF if never captured. */
uint32_t xhci_msc_last_usbsts(void);

/* Raw PORTSC value for the mass-storage device's root port, captured when
   a read/write last gave up. Bit 17 CSC (Connect Status Change), bit 18
   PEC (Port Enabled Change), bit 19 WRC (Warm Reset Change), bit 20 OCC
   (Over-current Change), bit 21 PRC (Port Reset Change), bit 22 PLC (Port
   Link State Change), bit 23 CEC (Config Error Change), bit 0 CCS
   (Current Connect Status), bits 5-8 PLS (Port Link State). Any change
   bit set here means the port itself flagged something the driver never
   acted on. 0xFFFFFFFF if never captured. */
uint32_t xhci_msc_last_portsc(void);

#endif
