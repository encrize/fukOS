/* doomgeneric platform layer for fukOS (bare-metal x86, 32-bit).
   Implements the DG_* interface plus doom_run(), the shell entry point. */
#include <stdint.h>
#include <stddef.h>

#include "framebuffer.h"
#include "console.h"
#include "io.h"
#include "hda.h"
#include "xhci.h"
#include "heap.h"

#include "doomgeneric.h"
#include "doomkeys.h"

extern pixel_t *DG_ScreenBuffer;

/* Provided by dg_libc.c */
void dg_heap_init(uintptr_t base, uintptr_t end);

/* Minimal setjmp/longjmp so DOOM's exit() can unwind back to the shell instead
   of rebooting. jmp_buf = int[6]: ebx, esi, edi, ebp, esp, return-eip. */
int  dg_setjmp(int *buf);
void dg_longjmp(int *buf, int val) __attribute__((noreturn));
__asm__(
    ".text\n"
    ".globl dg_setjmp\n"
    "dg_setjmp:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %ebx, 0(%eax)\n"
    "    movl %esi, 4(%eax)\n"
    "    movl %edi, 8(%eax)\n"
    "    movl %ebp, 12(%eax)\n"
    "    leal 4(%esp), %ecx\n"
    "    movl %ecx, 16(%eax)\n"
    "    movl (%esp), %ecx\n"
    "    movl %ecx, 20(%eax)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".globl dg_longjmp\n"
    "dg_longjmp:\n"
    "    movl 4(%esp), %edx\n"
    "    movl 8(%esp), %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl $1, %eax\n"
    "1:\n"
    "    movl 0(%edx), %ebx\n"
    "    movl 4(%edx), %esi\n"
    "    movl 8(%edx), %edi\n"
    "    movl 12(%edx), %ebp\n"
    "    movl 16(%edx), %esp\n"
    "    movl 20(%edx), %ecx\n"
    "    jmp *%ecx\n"
);

static int dg_shell_jb[6];
static int dg_shell_jb_valid = 0;

/* Called by dg_libc.c exit(): unwind back into doom_run() -> return to shell. */
void dg_return_to_shell(void)
{
    if (dg_shell_jb_valid) {
        dg_shell_jb_valid = 0;
        dg_longjmp(dg_shell_jb, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Framebuffer                                                        */
/* ------------------------------------------------------------------ */
static const fb_info *g_fb;

void DG_Init(void)
{
    if (g_fb) fb_clear(g_fb, 0x000000);
}

/* Fast 32bpp integer-scale blit: build one destination scanline in cached RAM,
   then write it to the framebuffer as 32-bit words. This avoids the per-pixel
   function-call + divide + bounds-check overhead of the generic scaler, which
   is the difference between a few FPS and a smooth frame rate. */
#define DG_MAXW 4096
static uint32_t dg_linebuf[DG_MAXW];

static void dg_blit32(const fb_info *fb, int x0, int y0,
                      const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t sc)
{
    uint8_t *base = fb->addr;
    uint32_t pitch = fb->pitch;
    uint32_t dw = sw * sc;
    for (uint32_t sy = 0; sy < sh; sy++) {
        const uint32_t *srow = src + sy * sw;
        /* Horizontal scale: build one scanline in RAM. */
        uint32_t di = 0;
        if (sc == 1) {
            for (uint32_t x = 0; x < sw; x++) dg_linebuf[di++] = srow[x] & 0x00FFFFFFu;
        } else if (sc == 2) {
            for (uint32_t x = 0; x < sw; x++) { uint32_t c = srow[x] & 0x00FFFFFFu; dg_linebuf[di++] = c; dg_linebuf[di++] = c; }
        } else {
            for (uint32_t x = 0; x < sw; x++) { uint32_t c = srow[x] & 0x00FFFFFFu; for (uint32_t k = 0; k < sc; k++) dg_linebuf[di++] = c; }
        }
        /* Vertical scale: copy that scanline to sc framebuffer rows. */
        for (uint32_t r = 0; r < sc; r++) {
            uint32_t dy = (uint32_t)y0 + sy * sc + r;
            uint32_t *drow = (uint32_t *)(base + dy * pitch) + x0;
            for (uint32_t x = 0; x < dw; x++) drow[x] = dg_linebuf[x];
        }
    }
}

/* Fractional stretch blit: fill the whole screen. A per-column source-index
   table is built once per frame so the inner loops contain no divides. */
static uint16_t dg_colmap[DG_MAXW];

static void dg_blit32_stretch(const fb_info *fb, int x0, int y0,
                              const uint32_t *src, uint32_t sw, uint32_t sh,
                              uint32_t dw, uint32_t dh)
{
    uint8_t *base = fb->addr;
    uint32_t pitch = fb->pitch;
    for (uint32_t dx = 0; dx < dw; dx++) {
        uint32_t sx = dx * sw / dw;
        if (sx >= sw) sx = sw - 1;
        dg_colmap[dx] = (uint16_t)sx;
    }
    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t sy = dy * sh / dh;
        if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + sy * sw;
        for (uint32_t dx = 0; dx < dw; dx++)
            dg_linebuf[dx] = srow[dg_colmap[dx]] & 0x00FFFFFFu;
        uint32_t *drow = (uint32_t *)(base + (uint32_t)(y0 + (int)dy) * pitch) + x0;
        for (uint32_t dx = 0; dx < dw; dx++) drow[dx] = dg_linebuf[dx];
    }
    (void)dg_blit32; /* kept as an integer-scale reference; not used now */
}

void DG_DrawFrame(void)
{
    if (!g_fb || !DG_ScreenBuffer) return;

    const uint32_t sw = DOOMGENERIC_RESX;
    const uint32_t sh = DOOMGENERIC_RESY;
    const uint8_t *px = (const uint8_t *)DG_ScreenBuffer; /* B,G,R,A */

    uint32_t fw = g_fb->width, fh = g_fb->height;

    /* Fast path: 32bpp framebuffer -> stretch to fill the entire screen. */
    if (g_fb->bpp == 32 && fw > 0 && fh > 0) {
        uint32_t dw = fw, dh = fh;
        if (dw > DG_MAXW) dw = DG_MAXW;
        int x0 = (int)((fw - dw) / 2);
        int y0 = (int)((fh - dh) / 2);
        dg_blit32_stretch(g_fb, x0, y0, (const uint32_t *)DG_ScreenBuffer,
                          sw, sh, dw, dh);
        return;
    }

    /* Fallback: generic scaler (non-32bpp framebuffers or very large widths). */
    if (fw >= sw && fh >= sh) {
        uint32_t s = fw / sw, t = fh / sh;
        uint32_t sc = s < t ? s : t;
        if (sc < 1) sc = 1;
        uint32_t dw = sw * sc, dh = sh * sc;
        int x0 = (int)((fw - dw) / 2);
        int y0 = (int)((fh - dh) / 2);
        if (sc == 1)
            fb_blit_bgra(g_fb, x0, y0, px, sw, sh);
        else
            fb_blit_bgra_scaled(g_fb, x0, y0, px, sw, sh, dw, dh, 0, 0, (int)fw, (int)fh);
    } else {
        uint32_t dw = fw;
        uint32_t dh = (fw * sh) / sw;
        if (dh > fh) { dh = fh; dw = (fh * sw) / sh; }
        int x0 = (int)((fw - dw) / 2);
        int y0 = (int)((fh - dh) / 2);
        fb_blit_bgra_scaled(g_fb, x0, y0, px, sw, sh, dw, dh, 0, 0, (int)fw, (int)fh);
    }
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* ------------------------------------------------------------------ */
/* Timer: PIT channel 0, latched-count polling                        */
/* ------------------------------------------------------------------ */
#define PIT_FREQ 1193182u

static uint64_t g_pit_accum;     /* accumulated PIT ticks */
static uint16_t g_pit_last;      /* last latched count    */
static int      g_pit_ready;

static uint16_t pit_read_count(void)
{
    outb(0x43, 0x00);            /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)((hi << 8) | lo);
}

static void pit_init(void)
{
    /* Channel 0, access lo/hi, mode 2 (rate generator), reload = 0 (65536). */
    outb(0x43, 0x34);
    outb(0x40, 0x00);
    outb(0x40, 0x00);
    g_pit_accum = 0;
    g_pit_last = pit_read_count();
    g_pit_ready = 1;
}

static void pit_sample(void)
{
    if (!g_pit_ready) pit_init();
    uint16_t now = pit_read_count();
    /* Count decrements; handle wrap at 65536. */
    uint16_t elapsed = (uint16_t)(g_pit_last - now);
    g_pit_accum += elapsed;
    g_pit_last = now;
}

uint32_t DG_GetTicksMs(void)
{
    pit_sample();
    return (uint32_t)((g_pit_accum * 1000ULL) / PIT_FREQ);
}

void DG_SleepMs(uint32_t ms)
{
    uint32_t start = DG_GetTicksMs();
    while ((DG_GetTicksMs() - start) < ms) {
        /* Keep cooperative DMA/storage work alive while Doom waits. */
        xhci_idle_drain();
        hda_bg_poll();
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard: raw PS/2 (scancode set 1) -> doom keys, with key-up       */
/* ------------------------------------------------------------------ */

/* make-code (0x00..0x58) -> doom key for the base (non-extended) set */
static const unsigned char sc_to_doom[0x59] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
    [0x0c] = KEY_MINUS, [0x0d] = KEY_EQUALS,
    [0x0e] = KEY_BACKSPACE,
    [0x0f] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1a] = '[', [0x1b] = ']',
    [0x1c] = KEY_ENTER,
    [0x1d] = KEY_FIRE,      /* left ctrl */
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2a] = KEY_RSHIFT,    /* left shift */
    [0x2b] = '\\',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = KEY_RSHIFT,    /* right shift */
    [0x38] = KEY_RALT,      /* left alt -> strafe */
    [0x39] = KEY_USE,       /* space -> use */
    [0x3b] = KEY_F1, [0x3c] = KEY_F2, [0x3d] = KEY_F3, [0x3e] = KEY_F4,
    [0x3f] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x57] = KEY_F11, [0x58] = KEY_F12,
};

static unsigned char ext_to_doom(unsigned char code)
{
    switch (code) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4b: return KEY_LEFTARROW;
        case 0x4d: return KEY_RIGHTARROW;
        case 0x1d: return KEY_FIRE;    /* right ctrl */
        case 0x38: return KEY_RALT;    /* right alt  */
        case 0x1c: return KEY_ENTER;   /* keypad enter */
        case 0x47: return KEY_HOME;
        case 0x4f: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        default:   return 0;
    }
}

#define KQ_SIZE 128
static struct { unsigned char pressed; unsigned char key; } kq[KQ_SIZE];
static int kq_head, kq_tail;
static int kq_ext;   /* saw an 0xE0 prefix */

static void kq_push(unsigned char pressed, unsigned char key)
{
    if (!key) return;
    int n = (kq_tail + 1) % KQ_SIZE;
    if (n == kq_head) return;   /* full: drop */
    kq[kq_tail].pressed = pressed;
    kq[kq_tail].key = key;
    kq_tail = n;
}

static void kbd_drain(void)
{
    /* Read all pending bytes from the PS/2 controller output buffer. */
    for (int guard = 0; guard < 256; guard++) {
        uint8_t status = inb(0x64);
        if (!(status & 0x01)) break;      /* output buffer empty */
        if (status & 0x20) { (void)inb(0x60); continue; } /* mouse byte */
        uint8_t sc = inb(0x60);
        if (sc == 0xE0) { kq_ext = 1; continue; }
        if (sc == 0xE1) { kq_ext = 0; continue; } /* pause: ignore */
        if (sc == 0xFA || sc == 0xFE || sc == 0xEE || sc == 0x00 || sc == 0xFF) {
            kq_ext = 0; continue;         /* ACK / resend / echo / errors */
        }
        unsigned char pressed = (sc & 0x80) ? 0 : 1;
        unsigned char code = sc & 0x7F;
        unsigned char key;
        if (kq_ext) { key = ext_to_doom(code); kq_ext = 0; }
        else        { key = (code < sizeof(sc_to_doom)) ? sc_to_doom[code] : 0; }
        kq_push(pressed, key);
    }
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    /* The shell keyboard path normally services these. Doom reads PS/2
       directly, so it must explicitly keep bgplay and USB FAT progressing. */
    xhci_idle_drain();
    hda_bg_poll();
    kbd_drain();
    if (kq_head == kq_tail) return 0;
    *pressed = kq[kq_head].pressed;
    *doomKey = kq[kq_head].key;
    kq_head = (kq_head + 1) % KQ_SIZE;
    return 1;
}

/* Blocking read of one doom key press (used by the reboot prompt). */
unsigned char dg_wait_key(void)
{
    for (;;) {
        int p; unsigned char k;
        if (DG_GetKey(&p, &k) && p) return k;
    }
}

/* ------------------------------------------------------------------ */
/* Entry point invoked by the shell 'doom' command                    */
/* ------------------------------------------------------------------ */
/* Reset all of DOOM's static/global state (kernel.c) so 'doom' can be launched
   more than once. MUST run before we touch any static below. */
extern void doom_reset_state(void);

void doom_run(const fb_info *fb, uint64_t total_ram_bytes)
{
    /* Give DOOM a pristine set of globals on every launch (fixes the
       "Z_Free: freed a pointer without ZONEID" crash on the 2nd run). This
       zeroes our own statics too, so it must come first -- everything below
       re-initialises what it needs. */
    doom_reset_state();

    g_fb = fb;

    /* Reserve a private arena from the real kernel heap. Doom defaults to a
       16 MiB zone; 32 MiB leaves room for its libc allocations as well. */
    (void)total_ram_bytes;
    const uint32_t arena_bytes = 32u * 1024u * 1024u;
    void *arena = kheap_alloc(arena_bytes);
    if (!arena) {
        console_puts("doom: kernel heap cannot reserve 32 MiB\n");
        return;
    }
    dg_heap_init((uintptr_t)arena, (uintptr_t)arena + arena_bytes);

    pit_init();
    kq_head = kq_tail = 0;
    kq_ext = 0;

    static char *argv[] = { "doom", "-iwad", "doom1.wad", NULL };

    if (dg_setjmp(dg_shell_jb) == 0) {
        dg_shell_jb_valid = 1;
        doomgeneric_Create(3, argv);
        for (;;) {
            doomgeneric_Tick();
        }
    }

    /* Reached only when DOOM quits (exit() -> dg_longjmp). Return to shell. */
    dg_shell_jb_valid = 0;
    kheap_free(arena);
    /* Restore the PIT to the BIOS default (channel 0, mode 3) for the kernel. */
    outb(0x43, 0x36);
    outb(0x40, 0x00);
    outb(0x40, 0x00);
    if (g_fb) fb_clear(g_fb, 0x000000);
    console_clear();
}
