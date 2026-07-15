#include <stdint.h>
#include "keyboard.h"
#include "io.h"
#include "xhci.h"
#include "hda.h"

static const char map_lower[128] = {
    [0x01]=27,
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',
    [0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',
    [0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',
    [0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x37]='*',
    [0x39]=' ',
};

static const char map_upper[128] = {
    [0x01]=27,
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
    [0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',[0x0C]='_',[0x0D]='+',
    [0x0E]='\b',[0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',
    [0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',
    [0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',
    [0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',
    [0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',
    [0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',[0x37]='*',
    [0x39]=' ',
};

static int g_shift = 0;
static int g_caps  = 0;
static int g_ctrl  = 0;

/* Software repeat is timed from CPUID/PIT-calibrated TSC ticks. */
static char     g_held_key = 0;
static uint8_t  g_held_sc  = 0;
static int      g_held_ext = 0;
static uint64_t g_rep_next    = 0;
static uint64_t g_last_make   = 0;

static uint64_t g_rep_delay   = 385000000ULL;
static uint64_t g_rep_rate    =  55000000ULL;
static uint64_t g_rep_release = 275000000ULL;
static uint64_t g_tsc_khz     = 0;

static char decode_event(void) {

    xhci_idle_drain();
    hda_bg_poll();

    uint8_t status = inb(0x64);
    if (!(status & 0x01)) {

        if (g_held_key) {
            uint64_t now = rdtsc();
            if ((int64_t)(now - g_last_make) > (int64_t)g_rep_release) {
                g_held_key = 0;
            } else if ((int64_t)(now - g_rep_next) >= 0) {
                g_rep_next = now + g_rep_rate;
                return g_held_key;
            }
        }
        return 0;
    }
    if (status & 0x20) { (void)inb(0x60); return 0; }
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        int spin = 200000;
        while (!(inb(0x64) & 0x01) && spin-- > 0) { }
        uint8_t e = inb(0x60);
        if (e == 0x1D) { g_ctrl = 1; return 0; }
        if (e & 0x80) {
            if (e == 0x9D) g_ctrl = 0;
            if (g_held_ext && g_held_sc == (uint8_t)(e & 0x7F)) g_held_key = 0;
            return 0;
        }
        char r = 0;
        switch (e) {
            case 0x48: r = g_shift ? KEY_SCROLL_UP : KEY_UP;       break;
            case 0x50: r = g_shift ? KEY_SCROLL_DOWN : KEY_DOWN;   break;
            case 0x4B: r = KEY_LEFT;  break;
            case 0x4D: r = KEY_RIGHT; break;
            case 0x49: r = KEY_PGUP;  break;
            case 0x51: r = KEY_PGDN;  break;
            case 0x47: r = KEY_HOME;  break;
            case 0x4F: r = KEY_END;   break;
            case 0x53: r = KEY_DEL;   break;
            default:   return 0;
        }
        uint64_t nowe = rdtsc();
        if (g_held_ext && g_held_sc == e && g_held_key == r) {
            g_last_make = nowe;
            return 0;
        }
        g_held_sc = e; g_held_ext = 1; g_held_key = r;
        g_rep_next = nowe + g_rep_delay;
        g_last_make = nowe;
        return r;
    }

    if (sc & 0x80) {
        uint8_t code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) g_shift = 0;
        if (code == 0x1D)                 g_ctrl  = 0;
        if (!g_held_ext && g_held_sc == code) g_held_key = 0;
        return 0;
    }
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return 0; }
    if (sc == 0x1D)               { g_ctrl  = 1; return 0; }
    if (sc == 0x3A)               { g_caps  = !g_caps; return 0; }

    if (sc == 0x58) {
        uint64_t nowf = rdtsc();
        if (!g_held_ext && g_held_sc == sc && g_held_key == KEY_F12) {
            g_last_make = nowf;
            return 0;
        }
        g_held_sc = sc; g_held_ext = 0; g_held_key = KEY_F12;
        g_rep_next = nowf + g_rep_delay;
        g_last_make = nowf;
        return KEY_F12;
    }

    char c = g_shift ? map_upper[sc] : map_lower[sc];
    if (!c) return 0;
    if (g_caps) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    if (g_ctrl) {

        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 1);
        return 0;
    }
    uint64_t nown = rdtsc();
    if (!g_held_ext && g_held_sc == sc && g_held_key == c) {
        g_last_make = nown;
        return 0;
    }
    g_held_sc = sc; g_held_ext = 0; g_held_key = c;
    g_rep_next = nown + g_rep_delay;
    g_last_make = nown;
    return c;
}

static int kbd_wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) return 1;
    return 0;
}
static int kbd_wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(0x64) & 0x01) return 1;
    return 0;
}

static void tsc_from_pit(void) {
    const uint32_t PIT_HZ = 1193182u;
    const uint32_t ms = 20u;
    uint32_t count = (PIT_HZ * ms) / 1000u;

    uint8_t p = (uint8_t)(inb(0x61) & 0xFCu);
    outb(0x61, p);
    outb(0x43, 0xB0u);
    outb(0x42, (uint8_t)(count & 0xFFu));
    outb(0x42, (uint8_t)((count >> 8) & 0xFFu));
    outb(0x61, (uint8_t)(p | 0x01u));

    uint64_t t0 = rdtsc();
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20u)) {
        if (++guard > 200000000u) return;
    }
    uint64_t t1 = rdtsc();
    outb(0x61, (uint8_t)(inb(0x61) & 0xFCu));

    uint64_t per_ms = (t1 - t0) / ms;
    if (per_ms >= 100000ULL && per_ms <= 20000000ULL) g_tsc_khz = per_ms;
}

/* Prefer CPUID 0x15, then PIT calibration, then a conservative fallback. */
static void detect_tsc_khz(void) {
    uint32_t maxleaf = 0, b0 = 0, c0 = 0, d0 = 0;
    cpuid_query(0, &maxleaf, &b0, &c0, &d0);
    if (maxleaf >= 0x15) {
        uint32_t denom = 0, numer = 0, crystal = 0, d = 0;
        cpuid_query(0x15, &denom, &numer, &crystal, &d);
        if (denom != 0 && numer != 0) {
            if (crystal == 0) crystal = 19200000u;
            uint64_t khz = ((uint64_t)crystal * (uint64_t)numer)
                           / ((uint64_t)denom * 1000ULL);
            if (khz >= 100000ULL && khz <= 20000000ULL) { g_tsc_khz = khz; return; }
        }
    }
    tsc_from_pit();
}

void kbd_init(void) {
    detect_tsc_khz();
    if (!g_tsc_khz) g_tsc_khz = 1100000ULL;
    g_rep_delay   = g_tsc_khz * 300ULL;
    g_rep_rate    = g_tsc_khz *  35ULL;
    g_rep_release = g_tsc_khz * 200ULL;
    for (int i = 0; i < 16 && (inb(0x64) & 0x01); i++) (void)inb(0x60);
    if (!kbd_wait_write()) return;
    outb(0x60, 0xF3);
    if (kbd_wait_read()) (void)inb(0x60);
    if (!kbd_wait_write()) return;
    outb(0x60, 0x00);
    if (kbd_wait_read()) (void)inb(0x60);
}

char kbd_poll(void) {
    return decode_event();
}

uint64_t kbd_tsc_per_ms(void) {
    return g_tsc_khz ? g_tsc_khz : 1100000ULL;
}

char kbd_getchar(void) {
    for (;;) {
        char c = decode_event();
        if (c) return c;
    }
}
