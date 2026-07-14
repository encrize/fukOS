#include "xhci.h"
#include "util.h"

#define CAP_CAPLENGTH   0x00u
#define CAP_HCSPARAMS1  0x04u
#define CAP_HCSPARAMS2  0x08u
#define CAP_HCCPARAMS1  0x10u
#define CAP_DBOFF       0x14u
#define CAP_RTSOFF      0x18u

#define OP_USBCMD      0x00u
#define OP_USBSTS      0x04u
#define OP_DCBAAP      0x30u
#define OP_CONFIG      0x38u
#define OP_CRCR        0x18u
#define OP_PORTSC_BASE 0x400u

#define USBCMD_RUN   (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)
#define PORTSC_CCS   (1u << 0)
#define PORTSC_PP    (1u << 9)

#define RT_IR0_ERSTSZ  0x28u
#define RT_IR0_ERSTBA  0x30u
#define RT_IR0_ERDP    0x38u

static inline uint32_t mmio_r32(uint64_t addr) { return *(volatile uint32_t *)(uintptr_t)addr; }
static inline void     mmio_w32(uint64_t addr, uint32_t v) { *(volatile uint32_t *)(uintptr_t)addr = v; }
static inline void     mmio_w64(uint64_t addr, uint64_t v) { *(volatile uint64_t *)(uintptr_t)addr = v; }

#define CMD_RING_TRBS  32
#define EVT_RING_TRBS  32
#define MAX_SLOTS_WANT 8
#define MAX_SCRATCHPAD 32
#define EP0_RING_TRBS  8
#define BULK_RING_TRBS 16384
#define BOT_DATA_BYTES (64u * 1024u)

typedef struct { uint32_t d0, d1, d2, d3; } trb_t;

static trb_t   g_cmd_ring[CMD_RING_TRBS]             __attribute__((aligned(64)));
static trb_t   g_evt_ring[EVT_RING_TRBS]             __attribute__((aligned(64)));
static uint64_t g_erst[2]                            __attribute__((aligned(64)));
static uint64_t g_dcbaa[MAX_SLOTS_WANT + 1]          __attribute__((aligned(64)));
static uint64_t g_scratch_ptrs[MAX_SCRATCHPAD]       __attribute__((aligned(64)));
static uint8_t  g_scratch_buf[MAX_SCRATCHPAD][4096]  __attribute__((aligned(4096)));

static trb_t    g_ep0_ring[EP0_RING_TRBS]            __attribute__((aligned(64)));
static uint8_t  g_input_ctx[32 * 32]                 __attribute__((aligned(64)));
static uint8_t  g_dev_ctx_out[32 * 32]               __attribute__((aligned(64)));
static uint8_t  g_ctrl_buf[256]                      __attribute__((aligned(64)));

/* A screenshot is a sustained multi-megabyte write. The former seven-entry
   bulk rings wrapped every few BOT commands, exercising the Link TRB/cycle
   transition hundreds of times. Keep a full-size ring so one screenshot
   does not wrap the transfer rings at all. */
static trb_t    g_bulk_in_ring[BULK_RING_TRBS]       __attribute__((aligned(65536)));
static trb_t    g_bulk_out_ring[BULK_RING_TRBS]      __attribute__((aligned(65536)));
static uint8_t  g_bot_cbw[32]                        __attribute__((aligned(64)));
static uint8_t  g_bot_csw[16]                        __attribute__((aligned(64)));
/* 64 KiB aligned so every data TRB stays inside one 64 KiB DMA window while
   reducing a 1366x768 BMP from ~769 SCSI commands to about 49. */
static uint8_t  g_bot_data[BOT_DATA_BYTES]           __attribute__((aligned(65536)));

static uint32_t g_cmd_enq   = 0;
static uint32_t g_cmd_cycle = 1;
static uint32_t g_evt_deq   = 0;
static uint32_t g_evt_cycle = 1;

static uint32_t g_ep0_enq   = 0;
static uint32_t g_ep0_cycle = 1;

static uint32_t g_bin_enq   = 0, g_bin_cycle  = 1;
static uint32_t g_bout_enq  = 0, g_bout_cycle = 1;

static xhci_hc_t g_msc_hc;
static int       g_msc_ready       = 0;
static uint32_t  g_msc_slot        = 0;
static uint32_t  g_msc_in_dci      = 0;
static uint32_t  g_msc_out_dci     = 0;
static uint8_t   g_msc_in_ep       = 0;
static uint8_t   g_msc_out_ep      = 0;
static uint32_t  g_msc_block_size  = 0;
static uint32_t  g_msc_block_count = 0;
static uint8_t   g_msc_if_num      = 0;
static uint8_t   g_msc_sense_key   = 0xFFu;
static uint8_t   g_msc_sense_asc   = 0xFFu;
static uint8_t   g_msc_sense_ascq  = 0xFFu;
/* Which BOT stage failed last (0=none, 1=CBW, 2=data, 3=CSW recv,
   4=CSW bad signature, 5=CSW phase error) and the raw xHCI completion
   code (cc) from that stage's transfer, so a wedged pipe (where even
   REQUEST SENSE cannot get through) can still be diagnosed. */
static uint8_t   g_msc_last_stage  = 0;
static uint8_t   g_msc_last_cc     = 0;
/* Result of the last endpoint-reset attempt itself: completion code of the
   Reset Endpoint command and of the Set TR Dequeue Pointer command
   (0xFE = no Command Completion Event ever arrived, i.e. that command
   itself timed out). Lets us tell "reset commands succeeded but the pipe
   is still wedged" apart from "the reset commands themselves are not
   taking effect on this hardware". */
static uint8_t   g_msc_reset_ep_cc  = 0xFFu;
static uint8_t   g_msc_reset_deq_cc = 0xFFu;
/* Raw USBSTS register captured at the moment a read/write ultimately gives
   up. If HCH (bit 0, Host Controller Halted) or HSE (bit 2, Host System
   Error) is set here, the whole controller has stopped processing
   anything -- commands, transfers, everything -- which explains why even
   Reset Endpoint / Set TR Dequeue Pointer commands never complete. */
static uint32_t  g_msc_last_usbsts  = 0xFFFFFFFFu;
/* Root port number the mass-storage device was enumerated on, and the raw
   PORTSC value captured at the moment a read/write last gave up. The
   change bits (CSC/PEC/WRC/OCC/PRC/PLC/CEC) tell us whether the port
   itself flagged a disconnect, reset, or link-state change that the
   driver never noticed while it kept retrying the old device/slot. */
static int       g_msc_port         = 0;
static int       g_msc_port_pending = 0;
static uint32_t  g_msc_last_portsc  = 0xFFFFFFFFu;
/* Saved endpoint descriptors from enumeration, kept so a slot-level
   recovery (Reset Device + Configure Endpoint) can rebuild the bulk
   endpoints without re-running full device enumeration. */
static uint16_t  g_msc_in_mps       = 0;
static uint16_t  g_msc_out_mps      = 0;
static uint8_t   g_msc_config_value = 0;
static uint16_t  g_msc_ep0_mps      = 0;
/* Number of times slot-level recovery (Reset Device) has been performed,
   purely for future diagnostics/limiting. */
static uint32_t  g_msc_slot_recoveries = 0;
/* Raw xHC-reported Slot State and bulk Endpoint States, captured straight
   from the device context the moment a read/write ultimately gives up.
   Slot State: 0=Disabled/Enabled, 1=Default, 2=Addressed, 3=Configured.
   Endpoint State: 0=Disabled, 1=Running, 2=Halted, 3=Stopped, 4=Error.
   This tells us the actual hardware-side state directly instead of only
   inferring it indirectly from recovery-command completion codes. */
static uint8_t   g_msc_last_slot_state   = 0xFFu;
static uint8_t   g_msc_last_ep_out_state = 0xFFu;
static uint8_t   g_msc_last_ep_in_state  = 0xFFu;

static void capture_slot_ep_diag(uint32_t out_dci, uint32_t in_dci) {
    const uint32_t *slot_ctx = (const uint32_t *)g_dev_ctx_out;
    g_msc_last_slot_state = (uint8_t)((slot_ctx[3] >> 27) & 0x1Fu);
    const uint32_t *ep_out = (const uint32_t *)(g_dev_ctx_out + 32u * (out_dci + 1u));
    const uint32_t *ep_in  = (const uint32_t *)(g_dev_ctx_out + 32u * (in_dci + 1u));
    g_msc_last_ep_out_state = (uint8_t)(ep_out[0] & 0x7u);
    g_msc_last_ep_in_state  = (uint8_t)(ep_in[0] & 0x7u);
}

static void zero_bytes(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static void xhci_bios_handoff(uint64_t mmio_base, uint32_t hccparams1) {
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFFu;
    if (xecp == 0) return;
    uint64_t addr = mmio_base + ((uint64_t)xecp * 4u);

    for (;;) {
        uint32_t cap = mmio_r32(addr);
        uint8_t id   = (uint8_t)(cap & 0xFFu);
        uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);

        if (id == 1) {
            cap |= (1u << 24);
            mmio_w32(addr, cap);
            for (uint32_t i = 0; i < 1000000u; i++) {
                cap = mmio_r32(addr);
                if (!(cap & (1u << 16)) && (cap & (1u << 24))) break;
            }
            return;
        }
        if (next == 0) return;

        addr = addr + ((uint64_t)next * 4u);
    }
}

void xhci_scan_protocols(const xhci_hc_t *hc, xhci_protocol_cb cb) {
    uint32_t hccparams1 = mmio_r32(hc->mmio_base + CAP_HCCPARAMS1);
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFFu;
    if (xecp == 0) return;
    uint64_t addr = hc->mmio_base + ((uint64_t)xecp * 4u);

    for (;;) {
        uint32_t cap  = mmio_r32(addr);
        uint8_t  id   = (uint8_t)(cap & 0xFFu);
        uint8_t  next = (uint8_t)((cap >> 8) & 0xFFu);

        if (id == 2) {
            uint32_t dw2 = mmio_r32(addr + 8u);
            int major       = (int)((cap >> 24) & 0xFFu);
            int port_offset = (int)(dw2 & 0xFFu);
            int port_count  = (int)((dw2 >> 8) & 0xFFu);
            if (cb) cb(major, port_offset, port_count);
        }
        if (next == 0) return;
        addr = addr + ((uint64_t)next * 4u);
    }
}

static void drain_pending_events(const xhci_hc_t *hc);

static uint32_t xhci_ss_port_mask(uint64_t mmio_base, uint32_t hccparams1) {
    uint32_t mask = 0;
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFFu;
    if (xecp == 0) return 0;
    uint64_t addr = mmio_base + ((uint64_t)xecp * 4u);
    for (;;) {
        uint32_t cap  = mmio_r32(addr);
        uint8_t  id   = (uint8_t)(cap & 0xFFu);
        uint8_t  next = (uint8_t)((cap >> 8) & 0xFFu);
        if (id == 2) {
            uint32_t dw2 = mmio_r32(addr + 8u);
            int major = (int)((cap >> 24) & 0xFFu);
            int off   = (int)(dw2 & 0xFFu);
            int cnt   = (int)((dw2 >> 8) & 0xFFu);
            if (major >= 3) {
                for (int i = 0; i < cnt; i++) {
                    int pn = off + i;
                    if (pn >= 1 && pn <= 32) mask |= (1u << (pn - 1));
                }
            }
        }
        if (next == 0) break;
        addr = addr + ((uint64_t)next * 4u);
    }
    return mask;
}

int xhci_init(uint64_t mmio_base, xhci_hc_t *out) {
    zero_bytes(out, sizeof(*out));
    out->mmio_base = mmio_base;

    uint8_t  caplen     = (uint8_t)mmio_r32(mmio_base + CAP_CAPLENGTH);
    uint32_t hcsparams1 = mmio_r32(mmio_base + CAP_HCSPARAMS1);
    uint32_t hcsparams2 = mmio_r32(mmio_base + CAP_HCSPARAMS2);
    uint32_t hccparams1 = mmio_r32(mmio_base + CAP_HCCPARAMS1);
    uint32_t dboff      = mmio_r32(mmio_base + CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff     = mmio_r32(mmio_base + CAP_RTSOFF) & ~0x1Fu;

    uint64_t op_base = mmio_base + caplen;
    uint64_t rt_base = mmio_base + rtsoff;
    uint64_t db_base = mmio_base + dboff;

    out->op_base      = op_base;
    out->rt_base      = rt_base;
    out->db_base      = db_base;
    out->max_slots    = hcsparams1 & 0xFFu;
    out->max_intrs    = (hcsparams1 >> 8) & 0x7FFu;
    out->max_ports    = (hcsparams1 >> 24) & 0xFFu;
    out->context_size = (hccparams1 & (1u << 2)) ? 64u : 32u;

    g_cmd_enq  = 0; g_cmd_cycle  = 1;
    g_evt_deq  = 0; g_evt_cycle  = 1;
    g_ep0_enq  = 0; g_ep0_cycle  = 1;
    g_bin_enq  = 0; g_bin_cycle  = 1;
    g_bout_enq = 0; g_bout_cycle = 1;

    xhci_bios_handoff(mmio_base, hccparams1);

    uint32_t cmd = mmio_r32(op_base + OP_USBCMD);
    if (cmd & USBCMD_RUN) {
        mmio_w32(op_base + OP_USBCMD, cmd & ~USBCMD_RUN);
        for (uint32_t i = 0; i < 2000000u; i++)
            if (mmio_r32(op_base + OP_USBSTS) & USBSTS_HCH) break;
    }

    mmio_w32(op_base + OP_USBCMD, USBCMD_HCRST);
    for (uint32_t i = 0; i < 4000000u; i++) {
        uint32_t c = mmio_r32(op_base + OP_USBCMD);
        uint32_t s = mmio_r32(op_base + OP_USBSTS);
        if (!(c & USBCMD_HCRST) && !(s & USBSTS_CNR)) break;
    }

    uint32_t slots_en = out->max_slots < MAX_SLOTS_WANT ? out->max_slots : MAX_SLOTS_WANT;
    mmio_w32(op_base + OP_CONFIG, slots_en);

    uint32_t sp_hi = (hcsparams2 >> 21) & 0x1Fu;
    uint32_t sp_lo = (hcsparams2 >> 27) & 0x1Fu;
    uint32_t max_scratch = (sp_hi << 5) | sp_lo;
    if (max_scratch > MAX_SCRATCHPAD) max_scratch = MAX_SCRATCHPAD;

    zero_bytes(g_dcbaa, sizeof(g_dcbaa));
    if (max_scratch > 0) {
        for (uint32_t i = 0; i < max_scratch; i++) {
            zero_bytes(g_scratch_buf[i], sizeof(g_scratch_buf[i]));
            g_scratch_ptrs[i] = (uint64_t)(uintptr_t)&g_scratch_buf[i][0];
        }
        g_dcbaa[0] = (uint64_t)(uintptr_t)&g_scratch_ptrs[0];
    }

    mmio_w64(op_base + OP_DCBAAP, (uint64_t)(uintptr_t)&g_dcbaa[0]);

    zero_bytes(g_cmd_ring, sizeof(g_cmd_ring));
    {
        uint64_t ring_addr = (uint64_t)(uintptr_t)&g_cmd_ring[0];
        trb_t *link = &g_cmd_ring[CMD_RING_TRBS - 1];
        link->d0 = (uint32_t)ring_addr;
        link->d1 = (uint32_t)(ring_addr >> 32);
        link->d2 = 0;
        link->d3 = (6u << 10)   | (1u << 1)   | 1u  ;
    }
    mmio_w64(op_base + OP_CRCR, ((uint64_t)(uintptr_t)&g_cmd_ring[0]) | 1u  );

    zero_bytes(g_evt_ring, sizeof(g_evt_ring));
    g_erst[0] = (uint64_t)(uintptr_t)&g_evt_ring[0];
    g_erst[1] = EVT_RING_TRBS;
    mmio_w32(rt_base + RT_IR0_ERSTSZ, 1);
    mmio_w64(rt_base + RT_IR0_ERDP, (uint64_t)(uintptr_t)&g_evt_ring[0]);
    mmio_w64(rt_base + RT_IR0_ERSTBA, (uint64_t)(uintptr_t)&g_erst[0]);

    mmio_w32(op_base + OP_USBCMD, USBCMD_RUN);
    int started = 0;
    for (uint32_t i = 0; i < 4000000u; i++) {
        if (!(mmio_r32(op_base + OP_USBSTS) & USBSTS_HCH)) { started = 1; break; }
    }

    if (started) {

        for (uint32_t p = 0; p < out->max_ports; p++) {
            uint64_t addr = op_base + OP_PORTSC_BASE + (uint64_t)p * 0x10u;
            uint32_t portsc = mmio_r32(addr);
            if (!(portsc & PORTSC_PP)) mmio_w32(addr, portsc | PORTSC_PP);
        }

        for (uint32_t i = 0; i < 8000000u; i++) { __asm__ volatile ("nop"); }

        uint32_t ss_mask = xhci_ss_port_mask(mmio_base, hccparams1);

        for (uint32_t p = 0; p < out->max_ports; p++) {
            if (!(ss_mask & (1u << p))) continue;
            uint64_t addr = op_base + OP_PORTSC_BASE + (uint64_t)p * 0x10u;
            uint32_t portsc = mmio_r32(addr);
            if (!(portsc & PORTSC_CCS)) mmio_w32(addr, (portsc & PORTSC_PP) | (1u << 31));
        }

        for (uint32_t iter = 0; iter < 220u; iter++) {
            int done = 0;
            if (ss_mask) {
                for (uint32_t p = 0; p < out->max_ports; p++) {
                    if (!(ss_mask & (1u << p))) continue;
                    uint64_t addr = op_base + OP_PORTSC_BASE + (uint64_t)p * 0x10u;
                    if (mmio_r32(addr) & PORTSC_CCS) { done = 1; break; }
                }
            } else {
                for (uint32_t p = 0; p < out->max_ports; p++) {
                    uint64_t addr = op_base + OP_PORTSC_BASE + (uint64_t)p * 0x10u;
                    if (mmio_r32(addr) & PORTSC_CCS) { done = 1; break; }
                }
            }
            if (done) break;
            for (uint32_t i = 0; i < 1000000u; i++) { __asm__ volatile ("nop"); }
        }

        for (uint32_t p = 0; p < out->max_ports; p++) {
            uint64_t addr = op_base + OP_PORTSC_BASE + (uint64_t)p * 0x10u;
            uint32_t portsc = mmio_r32(addr);
            mmio_w32(addr, (portsc & PORTSC_PP) | (1u << 17) | (1u << 19) | (1u << 21));
        }

        drain_pending_events(out);
    }

    out->ready = started;
    return started;
}

int xhci_scan_ports(const xhci_hc_t *hc, xhci_port_cb cb) {
    int connected = 0;
    for (uint32_t p = 0; p < hc->max_ports; p++) {
        uint64_t addr = hc->op_base + OP_PORTSC_BASE + (uint64_t)p * 0x10u;
        uint32_t portsc = mmio_r32(addr);
        if (portsc & PORTSC_CCS) {
            connected++;
            if (cb) cb((int)(p + 1), portsc);
        }
    }
    return connected;
}

uint32_t xhci_portsc(const xhci_hc_t *hc, int port_num) {
    uint64_t addr = hc->op_base + OP_PORTSC_BASE + (uint64_t)(port_num - 1) * 0x10u;
    return mmio_r32(addr);
}

static void ring_doorbell(const xhci_hc_t *hc, uint32_t slot_id, uint32_t target) {
    /* Publish ordinary RAM writes to TRBs/DMA buffers before notifying the
       controller. This matters on real hardware even when an emulator happens
       to observe the stores in program order. */
    __asm__ volatile ("mfence" ::: "memory");
    mmio_w32(hc->db_base + (uint64_t)slot_id * 4u, target);
}

static uint64_t cmd_ring_enqueue(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3_no_cycle) {
    trb_t *slot = &g_cmd_ring[g_cmd_enq];
    uint64_t phys = (uint64_t)(uintptr_t)slot;
    slot->d0 = d0; slot->d1 = d1; slot->d2 = d2;
    slot->d3 = d3_no_cycle | g_cmd_cycle;
    uint32_t next = g_cmd_enq + 1;
    if (next == (uint32_t)(CMD_RING_TRBS - 1)) {
        /* The Link TRB is consumed with the current producer cycle state.
           Refresh it on every wrap before Toggle Cycle flips the consumer
           state; leaving its initial cycle bit here eventually makes the
           controller treat the command ring as permanently empty. */
        g_cmd_ring[CMD_RING_TRBS - 1].d3 = (6u << 10) | (1u << 1) | g_cmd_cycle;
        g_cmd_enq = 0;
        g_cmd_cycle ^= 1u;
    } else g_cmd_enq = next;
    return phys;
}

static void drain_pending_events(const xhci_hc_t *hc) {
    for (uint32_t guard = 0; guard < EVT_RING_TRBS + 4u; guard++) {
        trb_t *e = &g_evt_ring[g_evt_deq];
        if ((e->d3 & 1u) != g_evt_cycle) break;
        g_evt_deq++;
        if (g_evt_deq == EVT_RING_TRBS) { g_evt_deq = 0; g_evt_cycle ^= 1u; }

        /* ERDP bit 3 clears EHB and must be written as one. */
        mmio_w64(hc->rt_base + RT_IR0_ERDP, ((uint64_t)(uintptr_t)&g_evt_ring[g_evt_deq]) | 8u);
    }
}

void xhci_idle_drain(void) {
    if (g_msc_ready) drain_pending_events(&g_msc_hc);
}

static int wait_event(const xhci_hc_t *hc, uint32_t expect_type, trb_t *out_evt, uint32_t timeout_iters) {
    for (uint32_t i = 0; i < timeout_iters; i++) {
        trb_t *e = &g_evt_ring[g_evt_deq];
        if ((e->d3 & 1u) == g_evt_cycle) {
            trb_t got = *e;
            g_evt_deq++;
            if (g_evt_deq == EVT_RING_TRBS) { g_evt_deq = 0; g_evt_cycle ^= 1u; }

            mmio_w64(hc->rt_base + RT_IR0_ERDP, ((uint64_t)(uintptr_t)&g_evt_ring[g_evt_deq]) | 8u);
            uint32_t type = (got.d3 >> 10) & 0x3Fu;
            if (type == expect_type) { *out_evt = got; return 1; }

            continue;
        }
    }
    return 0;
}

static int reset_port_if_needed(const xhci_hc_t *hc, int port_num, uint32_t *diag) {
    uint64_t addr = hc->op_base + OP_PORTSC_BASE + (uint64_t)(port_num - 1) * 0x10u;
    uint32_t portsc = mmio_r32(addr);
    uint32_t pls = (portsc >> 5) & 0xFu;
    if (pls == 0) { if (diag) *diag = portsc; return (portsc & PORTSC_CCS) != 0; }

    mmio_w32(addr, (portsc & PORTSC_PP) | (1u << 4));
    for (uint32_t i = 0; i < 4000000u; i++) {
        portsc = mmio_r32(addr);
        if (portsc & (1u << 21)) break;
    }

    if (diag) *diag = portsc;

    mmio_w32(addr, (portsc & PORTSC_PP) | (1u << 21) | (1u << 17));
    portsc = mmio_r32(addr);
    return (portsc & PORTSC_CCS) && (portsc & (1u << 1));
}

static int evaluate_ep0_max_packet(const xhci_hc_t *hc, uint32_t slot_id, uint32_t new_max_packet) {
    zero_bytes(g_input_ctx, sizeof(g_input_ctx));
    uint32_t *ic = (uint32_t *)g_input_ctx;
    ic[1] = (1u << 1);
    uint32_t *ep0_ctx = (uint32_t *)(g_input_ctx + 64);
    ep0_ctx[1] = (3u << 1) | (4u << 3) | (new_max_packet << 16);

    uint64_t input_ctx_phys = (uint64_t)(uintptr_t)g_input_ctx;
    cmd_ring_enqueue((uint32_t)input_ctx_phys, (uint32_t)(input_ctx_phys >> 32),
                      0, (13u << 10) | (slot_id << 24));
    ring_doorbell(hc, 0, 0);
    trb_t evt;
    if (!wait_event(hc, 33u, &evt, 8000000u)) return 0;
    return ((evt.d2 >> 24) & 0xFFu) == 1;
}

static void ep0_ring_init(void) {
    zero_bytes(g_ep0_ring, sizeof(g_ep0_ring));
    uint64_t ring_addr = (uint64_t)(uintptr_t)&g_ep0_ring[0];
    trb_t *link = &g_ep0_ring[EP0_RING_TRBS - 1];
    link->d0 = (uint32_t)ring_addr;
    link->d1 = (uint32_t)(ring_addr >> 32);
    link->d2 = 0;
    link->d3 = (6u << 10) | (1u << 1) | 1u;
    g_ep0_enq   = 0;
    g_ep0_cycle = 1;
}

static void ep0_enqueue(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3_no_cycle) {
    trb_t *slot = &g_ep0_ring[g_ep0_enq];
    slot->d0 = d0; slot->d1 = d1; slot->d2 = d2;
    slot->d3 = d3_no_cycle | g_ep0_cycle;
    uint32_t next = g_ep0_enq + 1;
    if (next == (uint32_t)(EP0_RING_TRBS - 1)) {

        trb_t *link = &g_ep0_ring[EP0_RING_TRBS - 1];
        link->d3 = (6u << 10) | (1u << 1) | g_ep0_cycle;
        g_ep0_enq   = 0;
        g_ep0_cycle ^= 1u;
    } else {
        g_ep0_enq = next;
    }
}

static uint32_t do_control_in(const xhci_hc_t *hc, uint32_t slot_id,
                              uint32_t bm_request_type, uint32_t b_request,
                              uint32_t w_value, uint32_t w_index, uint32_t want_len) {
    if (want_len > sizeof(g_ctrl_buf)) want_len = sizeof(g_ctrl_buf);
    zero_bytes(g_ctrl_buf, want_len);
    uint64_t buf_phys = (uint64_t)(uintptr_t)g_ctrl_buf;

    ep0_enqueue(bm_request_type | (b_request << 8) | (w_value << 16),
                w_index | (want_len << 16), 8u,
                (2u << 10) | (1u << 6) | (3u << 16));

    ep0_enqueue((uint32_t)buf_phys, (uint32_t)(buf_phys >> 32), want_len,
                (3u << 10) | (1u << 16));

    ep0_enqueue(0, 0, 0, (4u << 10) | (1u << 5));

    ring_doorbell(hc, slot_id, 1);
    trb_t evt;
    if (!wait_event(hc, 32u, &evt, 8000000u)) return 0xFFu;
    return (evt.d2 >> 24) & 0xFFu;
}

static uint32_t do_get_device_descriptor(const xhci_hc_t *hc, uint32_t slot_id, uint32_t want_len) {
    return do_control_in(hc, slot_id, 0x80u, 6u, 0x0100u, 0u, want_len);
}

static int read_config_and_find_bulk(const xhci_hc_t *hc, uint32_t slot_id, xhci_dev_info_t *out) {

    uint32_t cc = do_control_in(hc, slot_id, 0x80u, 6u, 0x0200u, 0u, 9u);
    if (cc != 1 && cc != 13) return 0;
    uint32_t total = (uint32_t)g_ctrl_buf[2] | ((uint32_t)g_ctrl_buf[3] << 8);
    if (total < 9) return 0;
    if (total > sizeof(g_ctrl_buf)) total = sizeof(g_ctrl_buf);

    cc = do_control_in(hc, slot_id, 0x80u, 6u, 0x0200u, 0u, total);
    if (cc != 1 && cc != 13) return 0;

    out->config_value = g_ctrl_buf[5];

    uint32_t i = 0;
    int in_ms_iface = 0;
    while (i + 2u <= total) {
        uint8_t blen  = g_ctrl_buf[i];
        uint8_t btype = g_ctrl_buf[i + 1];
        if (blen == 0) break;
        if (btype == 4u && i + 9u <= total) {
            uint8_t icls = g_ctrl_buf[i + 5];
            if (icls == 0x08u) {
                in_ms_iface = 1;
                out->if_class    = icls;
                out->if_subclass = g_ctrl_buf[i + 6];
                out->if_proto    = g_ctrl_buf[i + 7];
                out->if_num      = g_ctrl_buf[i + 2];
            } else {
                in_ms_iface = 0;
            }
        } else if (btype == 5u && i + 7u <= total && in_ms_iface) {
            uint8_t  epaddr = g_ctrl_buf[i + 2];
            uint8_t  attr   = g_ctrl_buf[i + 3];
            uint16_t mps    = (uint16_t)(g_ctrl_buf[i + 4] | (g_ctrl_buf[i + 5] << 8));
            if ((attr & 0x3u) == 2u) {
                if (epaddr & 0x80u) { out->bulk_in_ep  = epaddr; out->bulk_in_mps  = mps; }
                else                { out->bulk_out_ep = epaddr; out->bulk_out_mps = mps; }
            }
        }
        i += blen;
    }
    return (out->if_class == 0x08u && out->bulk_in_ep != 0 && out->bulk_out_ep != 0);
}

static void tring_init(trb_t *ring, uint32_t size, uint32_t *enq, uint32_t *cycle) {
    zero_bytes(ring, size * sizeof(trb_t));
    uint64_t addr = (uint64_t)(uintptr_t)ring;
    trb_t *link = &ring[size - 1];
    link->d0 = (uint32_t)addr;
    link->d1 = (uint32_t)(addr >> 32);
    link->d2 = 0;
    link->d3 = (6u << 10) | (1u << 1) | 1u;
    *enq = 0; *cycle = 1;
}

static void tring_enqueue(trb_t *ring, uint32_t size, uint32_t *enq, uint32_t *cycle,
                          uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3_no_cycle) {
    trb_t *slot = &ring[*enq];
    slot->d0 = d0; slot->d1 = d1; slot->d2 = d2;
    slot->d3 = d3_no_cycle | *cycle;
    uint32_t next = *enq + 1;
    if (next == size - 1) {
        trb_t *link = &ring[size - 1];
        link->d3 = (6u << 10) | (1u << 1) | *cycle;
        *enq = 0; *cycle ^= 1u;
    } else {
        *enq = next;
    }
}

static uint32_t bulk_xfer(const xhci_hc_t *hc, uint32_t slot_id, uint32_t dci,
                          trb_t *ring, uint32_t rsize, uint32_t *renq, uint32_t *rcycle,
                          uint64_t buf_phys, uint32_t len) {
    tring_enqueue(ring, rsize, renq, rcycle,
                  (uint32_t)buf_phys, (uint32_t)(buf_phys >> 32),
                  (len & 0x1FFFFu),
                  (1u << 10) | (1u << 5) | (1u << 2));
    ring_doorbell(hc, slot_id, dci);
    trb_t evt;
    /* Give slow flash media a bit more headroom than a plain control
       transfer, but keep it bounded: a persistently failing/rejecting
       device must fail fast, not hang the whole system for minutes. */
    if (!wait_event(hc, 32u, &evt, 25000000u)) return 0xFFu;
    return (evt.d2 >> 24) & 0xFFu;
}

static uint32_t do_control_no_data(const xhci_hc_t *hc, uint32_t slot_id,
                                   uint32_t bm_request_type, uint32_t b_request,
                                   uint32_t w_value, uint32_t w_index) {
    ep0_enqueue(bm_request_type | (b_request << 8) | (w_value << 16),
                w_index, 8u,
                (2u << 10) | (1u << 6));
    ep0_enqueue(0, 0, 0, (4u << 10) | (1u << 5) | (1u << 16));
    ring_doorbell(hc, slot_id, 1);
    trb_t evt;
    if (!wait_event(hc, 32u, &evt, 8000000u)) return 0xFFu;
    return (evt.d2 >> 24) & 0xFFu;
}

/* Recover both host and device endpoint state after a USB STALL.
   TRB command type IDs per the xHCI spec: Reset Endpoint = 14,
   Set TR Dequeue Pointer = 16. (Types 16/18 were used here before, which
   are actually Set TR Dequeue Pointer and the optional/often-unsupported
   Force Event command -- so a real Reset Endpoint was never issued, and a
   Halted endpoint stayed Halted forever, silently swallowing every later
   doorbell ring on this endpoint.) */
static int reset_bulk_endpoint(const xhci_hc_t *hc, uint32_t slot_id, uint32_t dci,
                               trb_t *ring, uint32_t rsize, uint32_t *enq, uint32_t *cycle,
                               uint8_t ep_addr) {
    trb_t evt;

    /* Reset Endpoint is legal only for a Halted endpoint. The old timeout
       path sent it unconditionally, so a still-Running endpoint returned
       Context State Error (0x13). Worse, the code then erased the software
       ring even though Set TR Dequeue Pointer was rejected too, permanently
       desynchronising software from the xHC. Select the command required by
       the live endpoint state and never touch the ring if that command fails. */
    const uint32_t *ep_ctx = (const uint32_t *)(g_dev_ctx_out + 32u * (dci + 1u));
    uint32_t ep_state = ep_ctx[0] & 0x7u;
    if (ep_state == 0u) {
        g_msc_reset_ep_cc = 0x13u;
        g_msc_reset_deq_cc = 0x13u;
        return 0;
    }

    if (ep_state == 1u) {
        /* Stop Endpoint before replacing the dequeue pointer of a Running
           ring whose last transfer timed out. */
        cmd_ring_enqueue(0, 0, 0, (15u << 10) | (dci << 16) | (slot_id << 24));
    } else if (ep_state == 2u || ep_state == 4u) {
        cmd_ring_enqueue(0, 0, 0, (14u << 10) | (dci << 16) | (slot_id << 24));
    } else if (ep_state != 3u) {
        g_msc_reset_ep_cc = 0x13u;
        g_msc_reset_deq_cc = 0x13u;
        return 0;
    }

    if (ep_state != 3u) {
        ring_doorbell(hc, 0, 0);
        g_msc_reset_ep_cc = wait_event(hc, 33u, &evt, 8000000u) ?
                            (uint8_t)((evt.d2 >> 24) & 0xFFu) : 0xFEu;
        if (g_msc_reset_ep_cc != 1u) {
            g_msc_reset_deq_cc = 0xFFu;
            return 0;
        }
    } else {
        g_msc_reset_ep_cc = 1u;
    }

    tring_init(ring, rsize, enq, cycle);

    uint64_t deq = (uint64_t)(uintptr_t)ring | 1u;
    cmd_ring_enqueue((uint32_t)deq, (uint32_t)(deq >> 32), 0,
                     (16u << 10) | (dci << 16) | (slot_id << 24));
    ring_doorbell(hc, 0, 0);
    g_msc_reset_deq_cc = wait_event(hc, 33u, &evt, 8000000u) ? (uint8_t)((evt.d2 >> 24) & 0xFFu) : 0xFEu;

    if (g_msc_reset_deq_cc != 1u) return 0;

    if (ep_state == 2u || ep_state == 4u)
        do_control_no_data(hc, slot_id, 0x02u, 1u, 0u, ep_addr);
    return 1;
}

static uint32_t ep_dci(uint8_t ep_addr) {
    uint32_t num = ep_addr & 0x0Fu;
    return (ep_addr & 0x80u) ? (num * 2u + 1u) : (num * 2u);
}

static int xhci_configure_bulk(const xhci_hc_t *hc, uint32_t slot_id, xhci_dev_info_t *info) {
    uint32_t in_dci  = ep_dci(info->bulk_in_ep);
    uint32_t out_dci = ep_dci(info->bulk_out_ep);
    uint32_t max_dci = in_dci > out_dci ? in_dci : out_dci;

    tring_init(g_bulk_in_ring,  BULK_RING_TRBS, &g_bin_enq,  &g_bin_cycle);
    tring_init(g_bulk_out_ring, BULK_RING_TRBS, &g_bout_enq, &g_bout_cycle);

    zero_bytes(g_input_ctx, sizeof(g_input_ctx));
    uint32_t *ic = (uint32_t *)g_input_ctx;

    ic[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);

    uint32_t *slot_in  = (uint32_t *)(g_input_ctx + 32);
    uint32_t *slot_out = (uint32_t *)g_dev_ctx_out;
    for (int k = 0; k < 8; k++) slot_in[k] = slot_out[k];
    /* Device Address and Slot State are output-only fields. */
    slot_in[3] = 0;
    slot_in[0] = (slot_in[0] & ~(0x1Fu << 27)) | (max_dci << 27);

    uint16_t in_mps  = info->bulk_in_mps  ? info->bulk_in_mps  : 512;
    uint16_t out_mps = info->bulk_out_mps ? info->bulk_out_mps : 512;

    uint32_t *ep_in = (uint32_t *)(g_input_ctx + 32u * (in_dci + 1u));
    uint64_t in_ring_phys = (uint64_t)(uintptr_t)g_bulk_in_ring;
    ep_in[1] = (3u << 1) | (6u << 3) | ((uint32_t)in_mps << 16);
    ep_in[2] = (uint32_t)in_ring_phys | 1u;
    ep_in[3] = (uint32_t)(in_ring_phys >> 32);
    ep_in[4] = in_mps;

    uint32_t *ep_out = (uint32_t *)(g_input_ctx + 32u * (out_dci + 1u));
    uint64_t out_ring_phys = (uint64_t)(uintptr_t)g_bulk_out_ring;
    ep_out[1] = (3u << 1) | (2u << 3) | ((uint32_t)out_mps << 16);
    ep_out[2] = (uint32_t)out_ring_phys | 1u;
    ep_out[3] = (uint32_t)(out_ring_phys >> 32);
    ep_out[4] = out_mps;

    uint64_t input_ctx_phys = (uint64_t)(uintptr_t)g_input_ctx;
    cmd_ring_enqueue((uint32_t)input_ctx_phys, (uint32_t)(input_ctx_phys >> 32),
                     0, (12u << 10) | (slot_id << 24));
    ring_doorbell(hc, 0, 0);
    trb_t evt;
    if (!wait_event(hc, 33u, &evt, 8000000u)) return 0;
    if (((evt.d2 >> 24) & 0xFFu) != 1) return 0;

    uint32_t cc = do_control_no_data(hc, slot_id, 0x00u, 9u, info->config_value, 0u);
    return (cc == 1 || cc == 13);
}

static int scsi_bot(const xhci_hc_t *hc, uint32_t slot_id, uint32_t in_dci, uint32_t out_dci,
                    uint8_t in_ep, uint8_t out_ep,
                    const uint8_t *cdb, uint32_t cdb_len, int data_in,
                    uint8_t *data, uint32_t data_len) {
    static uint32_t tag = 0x53434200u;
    tag++;

    zero_bytes(g_bot_cbw, sizeof(g_bot_cbw));
    g_bot_cbw[0] = 0x55; g_bot_cbw[1] = 0x53; g_bot_cbw[2] = 0x42; g_bot_cbw[3] = 0x43;
    g_bot_cbw[4] = tag & 0xFF; g_bot_cbw[5] = (tag >> 8) & 0xFF;
    g_bot_cbw[6] = (tag >> 16) & 0xFF; g_bot_cbw[7] = (tag >> 24) & 0xFF;
    g_bot_cbw[8]  = data_len & 0xFF;         g_bot_cbw[9]  = (data_len >> 8) & 0xFF;
    g_bot_cbw[10] = (data_len >> 16) & 0xFF; g_bot_cbw[11] = (data_len >> 24) & 0xFF;
    g_bot_cbw[12] = data_in ? 0x80 : 0x00;
    g_bot_cbw[13] = 0;
    g_bot_cbw[14] = cdb_len & 0x1F;
    for (uint32_t i = 0; i < cdb_len && i < 16; i++) g_bot_cbw[15 + i] = cdb[i];

    uint32_t cc = bulk_xfer(hc, slot_id, out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                            &g_bout_enq, &g_bout_cycle,
                            (uint64_t)(uintptr_t)g_bot_cbw, 31u);
    /* A timeout (0xFF), not just an explicit STALL (6), can leave the ring
       and device endpoint desynced. Recover in both cases, otherwise every
       later transfer on this endpoint keeps failing for the rest of the
       boot session. */
    if (cc == 6u || cc == 0xFFu) reset_bulk_endpoint(hc, slot_id, out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                                      &g_bout_enq, &g_bout_cycle, out_ep);
    if (cc != 1 && cc != 13) { g_msc_last_stage = 1; g_msc_last_cc = (uint8_t)cc; capture_slot_ep_diag(out_dci, in_dci); return 0; }

    int data_ok = 1;
    if (data_len > 0) {
        if (data_in) {
            cc = bulk_xfer(hc, slot_id, in_dci, g_bulk_in_ring, BULK_RING_TRBS,
                           &g_bin_enq, &g_bin_cycle, (uint64_t)(uintptr_t)data, data_len);
            if (cc == 6u || cc == 0xFFu) reset_bulk_endpoint(hc, slot_id, in_dci, g_bulk_in_ring, BULK_RING_TRBS,
                                              &g_bin_enq, &g_bin_cycle, in_ep);
        } else {
            cc = bulk_xfer(hc, slot_id, out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                           &g_bout_enq, &g_bout_cycle, (uint64_t)(uintptr_t)data, data_len);
            if (cc == 6u || cc == 0xFFu) reset_bulk_endpoint(hc, slot_id, out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                                              &g_bout_enq, &g_bout_cycle, out_ep);
        }
        if (cc != 1 && cc != 13) { data_ok = 0; g_msc_last_stage = 2; g_msc_last_cc = (uint8_t)cc; capture_slot_ep_diag(out_dci, in_dci); }
    }

    zero_bytes(g_bot_csw, sizeof(g_bot_csw));
    cc = bulk_xfer(hc, slot_id, in_dci, g_bulk_in_ring, BULK_RING_TRBS,
                   &g_bin_enq, &g_bin_cycle, (uint64_t)(uintptr_t)g_bot_csw, 13u);
    if (cc == 6u || cc == 0xFFu) reset_bulk_endpoint(hc, slot_id, in_dci, g_bulk_in_ring, BULK_RING_TRBS,
                                      &g_bin_enq, &g_bin_cycle, in_ep);
    if (!data_ok) return 0;
    if (cc != 1 && cc != 13) { g_msc_last_stage = 3; g_msc_last_cc = (uint8_t)cc; capture_slot_ep_diag(out_dci, in_dci); return 0; }

    if (!(g_bot_csw[0] == 0x55 && g_bot_csw[1] == 0x53 &&
          g_bot_csw[2] == 0x42 && g_bot_csw[3] == 0x53)) { g_msc_last_stage = 4; g_msc_last_cc = 0; return 0; }

    if (g_bot_csw[12] == 2u) {

        reset_bulk_endpoint(hc, slot_id, in_dci,  g_bulk_in_ring,  BULK_RING_TRBS,
                            &g_bin_enq,  &g_bin_cycle,  in_ep);
        reset_bulk_endpoint(hc, slot_id, out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                            &g_bout_enq, &g_bout_cycle, out_ep);
        g_msc_last_stage = 5; g_msc_last_cc = 0;
        capture_slot_ep_diag(out_dci, in_dci);
        return 0;
    }
    if (g_bot_csw[12] == 0) { g_msc_last_stage = 0; g_msc_last_cc = 0; }
    return g_bot_csw[12] == 0;
}

/* Class-specific Bulk-Only Mass Storage Reset (USB BOT spec 3.1). A plain
   endpoint-halt clear only fixes the host/endpoint side; some drives need
   this to resync their internal command state machine after a failed or
   timed-out transfer, especially under sustained write bursts. */
static void bot_mass_storage_reset(const xhci_hc_t *hc, uint32_t slot_id, uint8_t if_num) {
    do_control_no_data(hc, slot_id, 0x21u, 0xFFu, 0u, if_num);
}

/* After a write/read command ultimately fails, ask the device *why* via
   REQUEST SENSE (opcode 0x03), instead of just retrying blindly. This is
   the only way to tell a transient bus glitch apart from a device that is
   deliberately rejecting the command (e.g. write-protected media). */
static void scsi_capture_sense(void) {
    uint8_t cdb[16];
    uint8_t buf[18];
    zero_bytes(cdb, sizeof(cdb));
    zero_bytes(buf, sizeof(buf));
    cdb[0] = 0x03; cdb[4] = 18;
    if (scsi_bot(&g_msc_hc, g_msc_slot, g_msc_in_dci, g_msc_out_dci,
                 g_msc_in_ep, g_msc_out_ep, cdb, 6, 1, buf, 18)) {
        g_msc_sense_key  = buf[2]  & 0x0Fu;
        g_msc_sense_asc  = buf[12];
        g_msc_sense_ascq = buf[13];
    } else {
        g_msc_sense_key = g_msc_sense_asc = g_msc_sense_ascq = 0xFFu;
    }
}

void xhci_msc_last_sense(uint8_t *key, uint8_t *asc, uint8_t *ascq) {
    if (key)  *key  = g_msc_sense_key;
    if (asc)  *asc  = g_msc_sense_asc;
    if (ascq) *ascq = g_msc_sense_ascq;
}

void xhci_msc_last_stage(uint8_t *stage, uint8_t *cc) {
    if (stage) *stage = g_msc_last_stage;
    if (cc)    *cc    = g_msc_last_cc;
}

void xhci_msc_last_ep_diag(uint8_t *slot_state, uint8_t *ep_out_state, uint8_t *ep_in_state) {
    if (slot_state)   *slot_state   = g_msc_last_slot_state;
    if (ep_out_state) *ep_out_state = g_msc_last_ep_out_state;
    if (ep_in_state)  *ep_in_state  = g_msc_last_ep_in_state;
}

void xhci_msc_last_reset_result(uint8_t *ep_cc, uint8_t *deq_cc) {
    if (ep_cc)  *ep_cc  = g_msc_reset_ep_cc;
    if (deq_cc) *deq_cc = g_msc_reset_deq_cc;
}

uint32_t xhci_msc_last_usbsts(void) {
    return g_msc_last_usbsts;
}

uint32_t xhci_msc_last_portsc(void) {
    return g_msc_last_portsc;
}

static void xhci_storage_read(const xhci_hc_t *hc, uint32_t slot_id, xhci_dev_info_t *out) {
    uint32_t in_dci  = ep_dci(out->bulk_in_ep);
    uint32_t out_dci = ep_dci(out->bulk_out_ep);
    uint8_t cdb[16];

    g_msc_in_mps       = out->bulk_in_mps;
    g_msc_out_mps      = out->bulk_out_mps;
    g_msc_config_value = out->config_value;

    if (!xhci_configure_bulk(hc, slot_id, out)) { out->storage_stage = 1; return; }

    zero_bytes(cdb, sizeof(cdb));
    cdb[0] = 0x12; cdb[4] = 36;
    if (!scsi_bot(hc, slot_id, in_dci, out_dci, out->bulk_in_ep, out->bulk_out_ep, cdb, 6, 1, g_bot_data, 36)) { out->storage_stage = 2; return; }
    for (int i = 0; i < 8;  i++) out->inq_vendor[i]  = (char)g_bot_data[8 + i];
    out->inq_vendor[8] = 0;
    for (int i = 0; i < 16; i++) out->inq_product[i] = (char)g_bot_data[16 + i];
    out->inq_product[16] = 0;

    zero_bytes(cdb, sizeof(cdb));
    cdb[0] = 0x25;
    if (!scsi_bot(hc, slot_id, in_dci, out_dci, out->bulk_in_ep, out->bulk_out_ep, cdb, 10, 1, g_bot_data, 8)) { out->storage_stage = 3; return; }
    uint32_t last_lba = ((uint32_t)g_bot_data[0] << 24) | ((uint32_t)g_bot_data[1] << 16) |
                        ((uint32_t)g_bot_data[2] << 8)  |  (uint32_t)g_bot_data[3];
    uint32_t blk      = ((uint32_t)g_bot_data[4] << 24) | ((uint32_t)g_bot_data[5] << 16) |
                        ((uint32_t)g_bot_data[6] << 8)  |  (uint32_t)g_bot_data[7];
    out->block_size  = blk;
    out->block_count = last_lba + 1u;

    uint32_t rd = blk ? blk : 512u;
    if (rd > sizeof(g_bot_data)) rd = sizeof(g_bot_data);
    zero_bytes(cdb, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[8] = 1;
    int rok = 0;
    for (int attempt = 0; attempt < 4 && !rok; attempt++) {
        if (attempt) drain_pending_events(hc);
        if (scsi_bot(hc, slot_id, in_dci, out_dci, out->bulk_in_ep, out->bulk_out_ep, cdb, 10, 1, g_bot_data, rd)) rok = 1;
    }
    if (!rok) { out->storage_stage = 4; return; }
    for (int i = 0; i < 16; i++) out->sector0[i] = g_bot_data[i];
    if (rd >= 512 && g_bot_data[510] == 0x55 && g_bot_data[511] == 0xAA) out->boot_sig_ok = 1;

    g_msc_hc          = *hc;
    g_msc_slot        = slot_id;
    g_msc_in_dci      = in_dci;
    g_msc_out_dci     = out_dci;
    g_msc_in_ep       = out->bulk_in_ep;
    g_msc_out_ep      = out->bulk_out_ep;
    g_msc_block_size  = blk ? blk : 512u;
    g_msc_block_count = out->block_count;
    g_msc_if_num      = out->if_num;
    g_msc_port        = g_msc_port_pending;
    g_msc_ready       = 1;

    out->storage_ok = 1;
    out->storage_stage = 0;
}

int xhci_probe_port(xhci_hc_t *hc, int port_num, xhci_dev_info_t *out) {
    zero_bytes(out, sizeof(*out));
    if (hc->context_size != 32) return 0;

    uint32_t portsc = xhci_portsc(hc, port_num);
    if (!(portsc & PORTSC_CCS)) { out->fail_stage = 1; out->fail_detail = portsc; return 0; }
    uint32_t pls = (portsc >> 5) & 0xFu;
    if (pls != 0) {
        uint32_t diag = portsc;
        if (!reset_port_if_needed(hc, port_num, &diag)) {
            out->fail_stage = 2; out->fail_detail = diag; return 0;
        }
    }
    portsc = xhci_portsc(hc, port_num);
    if (!(portsc & PORTSC_CCS)) { out->fail_stage = 1; out->fail_detail = portsc; return 0; }

    drain_pending_events(hc);

    uint32_t speed_id = (portsc >> 10) & 0xFu;
    uint32_t max_packet = 512;
    if (speed_id == 1 || speed_id == 2) max_packet = 8;
    else if (speed_id == 3) max_packet = 64;

    cmd_ring_enqueue(0, 0, 0, (9u << 10));
    ring_doorbell(hc, 0, 0);
    trb_t evt;
    if (!wait_event(hc, 33u, &evt, 8000000u)) { out->fail_stage = 3; out->fail_detail = 0xFFu; return 0; }
    uint32_t comp_code = (evt.d2 >> 24) & 0xFFu;
    uint32_t slot_id   = (evt.d3 >> 24) & 0xFFu;
    if (comp_code != 1 || slot_id == 0 || slot_id > MAX_SLOTS_WANT) {
        out->fail_stage = 3; out->fail_detail = comp_code; return 0;
    }

    zero_bytes(g_input_ctx, sizeof(g_input_ctx));
    zero_bytes(g_dev_ctx_out, sizeof(g_dev_ctx_out));
    ep0_ring_init();

    uint32_t *ic = (uint32_t *)g_input_ctx;
    ic[1] = (1u << 0) | (1u << 1);

    uint32_t *slot_ctx = (uint32_t *)(g_input_ctx + 32);
    slot_ctx[0] = (speed_id << 20) | (1u << 27);
    slot_ctx[1] = ((uint32_t)port_num << 16);

    uint64_t ep0_ring_phys = (uint64_t)(uintptr_t)&g_ep0_ring[0];
    uint32_t *ep0_ctx = (uint32_t *)(g_input_ctx + 64);
    ep0_ctx[1] = (3u << 1) | (4u << 3) | (max_packet << 16);
    ep0_ctx[2] = (uint32_t)ep0_ring_phys | 1u;
    ep0_ctx[3] = (uint32_t)(ep0_ring_phys >> 32);
    ep0_ctx[4] = 8u;

    g_dcbaa[slot_id] = (uint64_t)(uintptr_t)g_dev_ctx_out;

    uint64_t input_ctx_phys = (uint64_t)(uintptr_t)g_input_ctx;
    cmd_ring_enqueue((uint32_t)input_ctx_phys, (uint32_t)(input_ctx_phys >> 32),
                      0, (11u << 10) | (slot_id << 24));
    ring_doorbell(hc, 0, 0);

    int addr_ok = 0;
    for (int attempt = 0; attempt < 3 && !addr_ok; attempt++) {
        if (attempt > 0) {
            cmd_ring_enqueue((uint32_t)input_ctx_phys, (uint32_t)(input_ctx_phys >> 32),
                              0, (11u << 10) | (slot_id << 24));
            ring_doorbell(hc, 0, 0);
        }
        if (!wait_event(hc, 33u, &evt, 8000000u)) { out->fail_stage = 4; out->fail_detail = 0xFFu; continue; }
        comp_code = (evt.d2 >> 24) & 0xFFu;
        if (comp_code != 1) { out->fail_stage = 4; out->fail_detail = comp_code; continue; }
        addr_ok = 1;
    }
    if (!addr_ok) return 0;

    if (speed_id == 1) {
        comp_code = do_get_device_descriptor(hc, slot_id, 8u);
        if (comp_code == 1 || comp_code == 13) {
            uint32_t real_mp = g_ctrl_buf[7];
            if (real_mp != 0 && real_mp != max_packet) {
                if (evaluate_ep0_max_packet(hc, slot_id, real_mp)) max_packet = real_mp;
            }
        }

    }

    comp_code = do_get_device_descriptor(hc, slot_id, 18u);
    if (comp_code == 0xFFu) { out->fail_stage = 5; out->fail_detail = 0xFFu; return 0; }
    if (comp_code != 1 && comp_code != 13) { out->fail_stage = 5; out->fail_detail = comp_code; return 0; }

    out->usb_class    = g_ctrl_buf[4];
    out->usb_subclass = g_ctrl_buf[5];
    out->usb_proto    = g_ctrl_buf[6];
    out->vendor_id    = (uint16_t)(g_ctrl_buf[8]  | (g_ctrl_buf[9]  << 8));
    out->product_id   = (uint16_t)(g_ctrl_buf[10] | (g_ctrl_buf[11] << 8));

    if (out->usb_class == 0x00u || out->usb_class == 0x08u) {
        if (read_config_and_find_bulk(hc, slot_id, out)) {
            /* Preserve the exact EP0 packet size used by successful initial
               enumeration. Slot recovery must rebuild a clean Address Device
               input context rather than copy output-only context fields. */
            g_msc_ep0_mps = (uint16_t)max_packet;
            g_msc_port_pending = port_num;
            xhci_storage_read(hc, slot_id, out);
        }
    }

    out->ok = 1;
    return 1;
}

int xhci_storage_mount(uint64_t bar0) {
    g_msc_ready = 0;
    xhci_hc_t hc;
    if (!xhci_init(bar0, &hc)) return 0;
    for (uint32_t p = 1; p <= hc.max_ports; p++) {
        uint32_t ps = xhci_portsc(&hc, (int)p);
        if (!(ps & PORTSC_CCS)) continue;
        xhci_dev_info_t info;
        if (xhci_probe_port(&hc, (int)p, &info) && info.storage_ok) return 1;
    }
    return g_msc_ready;
}

/* Escalation beyond endpoint-level recovery: if the port itself reports a
   connect/warm-reset/port-reset change (CSC/WRC/PRC), the SuperSpeed link
   dropped and auto-recovered at the hardware level. That invalidates the
   xHC's view of this slot's endpoints -- which is why Reset Endpoint and
   Set TR Dequeue Pointer themselves stop completing (cc 0xFE): the slot
   itself needs recovering first. Issue Reset Device, then redo Configure
   Endpoint + SET_CONFIGURATION to bring the bulk endpoints back to the
   Running state. No-ops (returns 0 quickly) if no such port change is
   pending, so calling this speculatively on unrelated failures is cheap
   and safe. Returns 1 if a slot-level recovery was performed. */
static int slot_level_recover(void) {
    if (g_msc_port <= 0) return 0;
    uint64_t addr = g_msc_hc.op_base + OP_PORTSC_BASE + (uint64_t)(g_msc_port - 1) * 0x10u;
    uint32_t portsc = mmio_r32(addr);
    mmio_w32(addr, (portsc & PORTSC_PP) | (1u << 17) | (1u << 18) | (1u << 19) |
                   (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23));

    /* Trigger on a live-observed broken Slot/Endpoint state too, not just a
       PORTSC change bit. A change bit only fires once and gets cleared the
       first time any recovery reads PORTSC; if that earlier recovery itself
       left the slot stuck in Default (Configure Endpoint never legally ran
       from there), every later attempt sees a clean PORTSC and this
       function used to bail out immediately, silently doing nothing while
       the endpoints stayed Disabled for the rest of the boot session. */
    const uint32_t *slot_ctx    = (const uint32_t *)g_dev_ctx_out;
    uint32_t         slot_state = (slot_ctx[3] >> 27) & 0x1Fu;
    const uint32_t *ep_out_ctx  = (const uint32_t *)(g_dev_ctx_out + 32u * (g_msc_out_dci + 1u));
    const uint32_t *ep_in_ctx   = (const uint32_t *)(g_dev_ctx_out + 32u * (g_msc_in_dci + 1u));
    uint32_t out_state = ep_out_ctx[0] & 0x7u;
    uint32_t in_state  = ep_in_ctx[0] & 0x7u;
    int port_change  = (portsc & ((1u << 17) | (1u << 19) | (1u << 21))) != 0;
    int state_broken = (slot_state != 3u) || (out_state != 1u) || (in_state != 1u);
    if (!port_change && !state_broken) return 0;

    trb_t evt;
    /* Do not use Reset Device merely because a bulk endpoint is Disabled.
       That command itself is what demoted a healthy Configured slot to
       Default in the previous recovery path. A Configured/Addressed slot can
       have its missing bulk contexts installed directly with Configure
       Endpoint; only an already-Default slot needs Address Device below. */

    /* Address Device again from Default. Build a clean input context just
       like initial enumeration. Copying the output Slot/EP0 contexts here
       used to copy output-only Device Address, Slot State and Endpoint State
       bits back into an input context. Real Apollo Lake hardware rejects
       that malformed context, although emulators may tolerate it. */
    if (slot_state == 1u) {
        uint32_t live_portsc = mmio_r32(addr);
        uint32_t speed_id = (live_portsc >> 10) & 0xFu;
        uint32_t ep0_mps = g_msc_ep0_mps;
        if (ep0_mps == 0) {
            ep0_mps = 512u;
            if (speed_id == 1u || speed_id == 2u) ep0_mps = 8u;
            else if (speed_id == 3u) ep0_mps = 64u;
        }

        ep0_ring_init();
        zero_bytes(g_input_ctx, sizeof(g_input_ctx));
        uint32_t *ic = (uint32_t *)g_input_ctx;
        ic[1] = (1u << 0) | (1u << 1);

        uint32_t *slot_in = (uint32_t *)(g_input_ctx + 32);
        slot_in[0] = (speed_id << 20) | (1u << 27);
        slot_in[1] = ((uint32_t)g_msc_port << 16);

        uint64_t ep0_ring_phys = (uint64_t)(uintptr_t)&g_ep0_ring[0];
        uint32_t *ep0_in = (uint32_t *)(g_input_ctx + 64);
        ep0_in[1] = (3u << 1) | (4u << 3) | (ep0_mps << 16);
        ep0_in[2] = (uint32_t)ep0_ring_phys | 1u;
        ep0_in[3] = (uint32_t)(ep0_ring_phys >> 32);
        ep0_in[4] = 8u;

        uint64_t input_ctx_phys = (uint64_t)(uintptr_t)g_input_ctx;
        cmd_ring_enqueue((uint32_t)input_ctx_phys, (uint32_t)(input_ctx_phys >> 32),
                         0, (11u << 10) | (g_msc_slot << 24));
        ring_doorbell(&g_msc_hc, 0, 0);
        if (!wait_event(&g_msc_hc, 33u, &evt, 8000000u)) return 0;
        if (((evt.d2 >> 24) & 0xFFu) != 1) return 0;
        slot_state = 2u;
    }

    if (slot_state != 2u && slot_state != 3u) return 0;

    xhci_dev_info_t tmp;
    zero_bytes(&tmp, sizeof(tmp));
    tmp.bulk_in_ep   = g_msc_in_ep;
    tmp.bulk_out_ep  = g_msc_out_ep;
    tmp.bulk_in_mps  = g_msc_in_mps;
    tmp.bulk_out_mps = g_msc_out_mps;
    tmp.config_value = g_msc_config_value;
    if (!xhci_configure_bulk(&g_msc_hc, g_msc_slot, &tmp)) return 0;

    g_msc_slot_recoveries++;
    return 1;
}

int xhci_msc_ready(void) { return g_msc_ready; }
uint32_t xhci_msc_block_size(void)  { return g_msc_block_size; }
uint32_t xhci_msc_block_count(void) { return g_msc_block_count; }

int xhci_msc_read(uint32_t lba, uint32_t count, void *buf) {
    if (!g_msc_ready || g_msc_block_size == 0) return -1;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t bs  = g_msc_block_size;

    /* Keep each DMA transfer inside one 64 KiB window. */
    uint32_t max_blocks = (uint32_t)sizeof(g_bot_data) / bs;
    if (max_blocks == 0) max_blocks = 1;
    while (count > 0) {
        uint32_t chunk = count < max_blocks ? count : max_blocks;
        uint8_t cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x28;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);
        int ok = 0;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (attempt == 1) {
                drain_pending_events(&g_msc_hc);
            } else if (attempt == 2) {
                /* Quick recovery wasn't enough; try one full BOT + endpoint
                   resync, then give up quickly rather than hammering a
                   device that is actively rejecting the command. */
                bot_mass_storage_reset(&g_msc_hc, g_msc_slot, g_msc_if_num);
                reset_bulk_endpoint(&g_msc_hc, g_msc_slot, g_msc_in_dci, g_bulk_in_ring, BULK_RING_TRBS,
                                    &g_bin_enq, &g_bin_cycle, g_msc_in_ep);
                reset_bulk_endpoint(&g_msc_hc, g_msc_slot, g_msc_out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                                    &g_bout_enq, &g_bout_cycle, g_msc_out_ep);
            } else if (attempt == 3) {
                /* Endpoint-level recovery itself got no completion at all
                   (cc 0xFE) -- a sign the SuperSpeed link auto-recovered
                   via a warm/port reset and left this slot stale. Escalate
                   to slot-level recovery; this is a no-op if the port
                   shows no such change. */
                slot_level_recover();
            }
            if (scsi_bot(&g_msc_hc, g_msc_slot, g_msc_in_dci, g_msc_out_dci,
                         g_msc_in_ep, g_msc_out_ep,
                         cdb, 10, 1, g_bot_data, chunk * bs)) ok = 1;
        }
        if (!ok) {
            g_msc_last_usbsts = mmio_r32(g_msc_hc.op_base + OP_USBSTS);
            if (g_msc_port > 0) g_msc_last_portsc = mmio_r32(g_msc_hc.op_base + OP_PORTSC_BASE + (uint64_t)(g_msc_port - 1) * 0x10u);
            scsi_capture_sense();
            return -1;
        }
        for (uint32_t i = 0; i < chunk * bs; i++) dst[i] = g_bot_data[i];
        dst   += chunk * bs;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

int xhci_msc_write(uint32_t lba, uint32_t count, const void *buf) {
    if (!g_msc_ready || g_msc_block_size == 0) return -1;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bs  = g_msc_block_size;
    uint32_t max_blocks = (uint32_t)sizeof(g_bot_data) / bs;
    if (max_blocks == 0) max_blocks = 1;
    while (count > 0) {
        uint32_t chunk = count < max_blocks ? count : max_blocks;
        for (uint32_t i = 0; i < chunk * bs; i++) g_bot_data[i] = src[i];
        uint8_t cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x2A;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);
        int ok = 0;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (attempt == 1) {
                drain_pending_events(&g_msc_hc);
            } else if (attempt == 2) {
                /* Quick recovery wasn't enough; try one full BOT + endpoint
                   resync, then give up quickly rather than hammering a
                   device that is actively rejecting the command. */
                bot_mass_storage_reset(&g_msc_hc, g_msc_slot, g_msc_if_num);
                reset_bulk_endpoint(&g_msc_hc, g_msc_slot, g_msc_in_dci, g_bulk_in_ring, BULK_RING_TRBS,
                                    &g_bin_enq, &g_bin_cycle, g_msc_in_ep);
                reset_bulk_endpoint(&g_msc_hc, g_msc_slot, g_msc_out_dci, g_bulk_out_ring, BULK_RING_TRBS,
                                    &g_bout_enq, &g_bout_cycle, g_msc_out_ep);
            } else if (attempt == 3) {
                /* Endpoint-level recovery itself got no completion at all
                   (cc 0xFE) -- a sign the SuperSpeed link auto-recovered
                   via a warm/port reset and left this slot stale. Escalate
                   to slot-level recovery; this is a no-op if the port
                   shows no such change. */
                slot_level_recover();
            }
            if (scsi_bot(&g_msc_hc, g_msc_slot, g_msc_in_dci, g_msc_out_dci,
                         g_msc_in_ep, g_msc_out_ep,
                         cdb, 10, 0, g_bot_data, chunk * bs)) ok = 1;
        }
        if (!ok) {
            g_msc_last_usbsts = mmio_r32(g_msc_hc.op_base + OP_USBSTS);
            if (g_msc_port > 0) g_msc_last_portsc = mmio_r32(g_msc_hc.op_base + OP_PORTSC_BASE + (uint64_t)(g_msc_port - 1) * 0x10u);
            scsi_capture_sense();
            return -1;
        }
        src   += chunk * bs;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}
