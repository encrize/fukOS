#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "framebuffer.h"
#include "render.h"
#include "fat.h"
#include "image.h"
#include "util.h"
#include "io.h"
#include "pci.h"
#include "xhci.h"
#include "rtc.h"
#include "hda.h"
#include "heap.h"
#include "interrupts.h"
#include "panic.h"
#include "serial.h"

void doom_run(const fb_info *fb, uint64_t total_ram_bytes);

extern const unsigned int demo_image_width;
extern const unsigned int demo_image_height;

static const fb_info *FB;

static uint64_t TOTAL_RAM_BYTES;
static uint64_t SHELL_START_TSC;

extern uint8_t _kernel_end;

static uint8_t file_buf[16u * 1024u * 1024u];
static uint8_t pix_buf [16u * 1024u * 1024u] __attribute__((aligned(128)));
static uint8_t undo_buf[16u * 1024u * 1024u];
static uint32_t g_undo_n, g_undo_cur;
static int      g_undo_valid;

#define MAX_TEXTROWS 200000u
#define TXT_MARGIN   6
static uint32_t txt_row_off[MAX_TEXTROWS];
static uint32_t txt_row_len[MAX_TEXTROWS];

static char img_names[256][128];
static int  img_count;
static char wav_names[256][128];
static int  wav_count;
static char g_bg_track[128];
static uint32_t g_bg_total_seconds;
static fat_dirent g_bg_wavs[256];
static int g_bg_count, g_bg_index, g_bg_queue_active;
static int g_bg_repeat, g_bg_shuffle;

#define COL_TEXT   0xD8E0F0
#define COL_BG     0x0B0F1A
#define COL_PROMPT 0x7FE0A0
#define COL_TITLE  0x9FD0FF
#define COL_WARN   0xFFD27F
#define COL_DIR    0x9FE0A0
#define COL_LS_IMG 0xC98BFF
#define COL_LS_TXT 0xFFE24D
#define COL_LS_WAV 0xFF4F81
#define COL_LOGO   0xFF8F5C
#define COL_LABEL  0x7FE0A0
#define COL_BAR_OK   0x7FE0A0
#define COL_BAR_MID  0xFFD27F
#define COL_BAR_HOT  0xFF6B6B
#define FF_LOGO_COLS 42

static const char *const SHELL_COMMANDS[] = {
    "help", "clear", "ls", "cd", "pwd", "tree", "cat", "head", "wc",
    "hexdump", "play", "playlist", "bgplay", "bgpause", "bgresume",
    "bgstop", "bgstatus", "bgvolume", "bgrepeat", "bgshuffle", "bgnext",
    "bgprev", "audioout", "img", "photo", "screenshot", "res", "about",
    "reboot", "poweroff", "lspci", "usb", "diskinfo", "time", "touch",
    "mkdir", "rmdir", "rm", "cp", "mv", "echo", "edit", "fastfetch", "open", "matrix", "doom",
    "irqinfo", "panic-test", "clock"
};
#define SHELL_COMMAND_COUNT ((int)(sizeof SHELL_COMMANDS / sizeof SHELL_COMMANDS[0]))

/* Forward-declared so the F12 hotkey can be handled before the screenshot
   encoder is defined further down the file. */
static void capture_screenshot_auto(const fb_info *source);

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int cieq(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int starts_ci(const char *text, const char *prefix, int length) {
    for (int i = 0; i < length; i++) {
        if (!text[i] || lc(text[i]) != lc(prefix[i])) return 0;
    }
    return 1;
}

static int has_ext_ci(const char *name, const char *ext) {
    uint32_t n = 0, e = 0;
    while (name[n]) n++;
    while (ext[e]) e++;
    if (n < e) return 0;
    return cieq(name + n - e, ext);
}

static void num(uint32_t v, char *dst) {
    char n[16];
    kutoa(v, n);
    kstrcat(dst, n);
}

static void num2(uint32_t v, char *dst) {
    char n[3];
    n[0] = (char)('0' + (v / 10) % 10);
    n[1] = (char)('0' + v % 10);
    n[2] = 0;
    kstrcat(dst, n);
}

static void hex(uint32_t v, char *dst) {
    char n[16];
    khtoa(v, n);
    kstrcat(dst, n);
}

static void hex4(uint32_t v, char *dst) {
    char n[8];
    khtoa_fixed(v, 4, n);
    kstrcat(dst, n);
}

static void hex_byte(uint32_t v, char *dst) {
    char n[4];
    khtoa_fixed(v, 2, n);
    kstrcat(dst, n);
}

static void pad_to(char *dst, int width) {
    int n = (int)kstrlen(dst);
    while (n < width) { dst[n++] = ' '; }
    dst[n] = 0;
}

static uint32_t parse_uint(const char *s, int *ok) {
    uint32_t v = 0;
    int any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; any = 1; }
    if (ok) *ok = any && (*s == 0 || *s == ' ');
    return v;
}

static const char *img_kind(const uint8_t *buf, uint32_t got, image_t *probe) {
    if (got && image_probe(buf, got, probe)) {

        if (buf[0] == 'I' && buf[1] == 'M' && buf[2] == 'G' && buf[3] == '1')
            return "IMG1";
        return "BMP";
    }
    return 0;
}

static const char *const FASTFETCH_LOGO[] = {
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣤⣤⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣾⣿⣿⣿⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣼⣿⣿⣿⡄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⢀⣾⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣼⣿⣿⣿⣿⣿⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⣾⣿⣿⣿⣿⣿⣿⣧⠀⠀⠀⠀⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣧⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⣼⣿⣿⣿⣿⣿⣿⣿⣿⣇⠀⠀⠀⠀⠀⠀⠀⢀⣿⣿⣿⣿⣿⣿⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡄⠀⠀⠀⠀⠀⠀⣼⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⢀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣧⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡄⠀⠀⠀⢀⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣇⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⢀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠈⣿⣿⣿⣿⣿⣿⠟⠉⠀⠀⠀⠙⢿⣿⣿⣿⣿⣿⣿⣿⡿⠋⠀⠀⠙⢻⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⣿⣿⣿⣿⣿⠃⠀⠀⠀⠀⣠⣄⠀⢻⣿⣿⣿⣿⣿⡿⠀⣠⣄⠀⠀⠀⢻⣿⣿⣿⣿⣿⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⣾⣿⣿⣿⣿⠀⠀⠀⠀⠰⣿⣿⠀⢸⣿⣿⣿⣿⣿⡇⠀⣿⣿⡇⠀⠀⢸⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⣿⣿⣿⣿⣿⣄⠀⠀⠀⠀⠙⠃⠀⣼⣿⣿⣿⣿⣿⣇⠀⠙⠛⠁⠀⠀⣼⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⣿⣿⣿⣿⣿⣿⣷⣤⣄⣀⣠⣤⣾⣿⣿⣿⣿⣽⣿⣿⣦⣄⣀⣀⣤⣾⣿⣿⣿⣿⠃⠀⠀⢀⣀⠀⠀",
    "⠰⡶⠶⠶⠶⠿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡟⠛⠉⠉⠙⠛⠋⠀",
    "⠀⠀⢀⣀⣠⣤⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠷⠶⠶⠶⢤⣤⣀⠀",
    "⠀⠛⠋⠉⠁⠀⣀⣴⡿⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣯⣤⣀⡀⠀⠀⠀⠀⠘⠃",
    "⠀⠀⢀⣤⡶⠟⠉⠁⠀⠀⠉⠛⠿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠿⠟⠉⠀⠀⠀⠉⠙⠳⠶⣄⡀⠀⠀",
    "⠀⠀⠙⠁⠀⠀⠀⠀⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡏⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠁⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢠⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀ ⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣼⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⠀⣸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠀⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⣰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⢀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⣸⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀",
};
#define FASTFETCH_LOGO_LINES ((int)(sizeof(FASTFETCH_LOGO) / sizeof(FASTFETCH_LOGO[0])))

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}

static void ltrim(char *s) {
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) { char *d = s; while (*p) *d++ = *p++; *d = 0; }
}

static void cpu_vendor(char *out13) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(out13 + 0, &b, 4);
    memcpy(out13 + 4, &d, 4);
    memcpy(out13 + 8, &c, 4);
    out13[12] = 0;
}

static void cpu_brand(char *out49) {
    uint32_t a, b, c, d;
    uint32_t words[12];
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000004u) { out49[0] = 0; return; }
    cpuid(0x80000002u, &words[0], &words[1], &words[2], &words[3]);
    cpuid(0x80000003u, &words[4], &words[5], &words[6], &words[7]);
    cpuid(0x80000004u, &words[8], &words[9], &words[10], &words[11]);
    memcpy(out49, words, 48);
    out49[48] = 0;
    ltrim(out49);
}

static void human_size(uint64_t bytes, char *dst) {
    if (bytes >= (1024ull * 1024ull)) {
        num((uint32_t)(bytes >> 20), dst);
        kstrcat(dst, " MiB");
    } else {
        num((uint32_t)(bytes >> 10), dst);
        kstrcat(dst, " KiB");
    }
}

static void ff_row(int r, int c, const char *label, const char *value,
                    uint32_t label_color, uint32_t value_color) {
    console_draw_at(r, c, label, label_color);
    console_draw_at(r, c + (int)kstrlen(label), value, value_color);
}

static void append_time(char *line, uint32_t seconds);
static void cmd_edit(const char *arg);

static void ff_bar(char *out, int width, uint32_t used, uint32_t total) {
    uint32_t pct = total ? (used * 100u) / total : 0;
    if (pct > 100u) pct = 100u;
    int filled = (int)((pct * (uint32_t)width) / 100u);
    out[0] = 0; kstrcat(out, "[");
    for (int i = 0; i < width; i++) kstrcat(out, i < filled ? "#" : ".");
    kstrcat(out, "] "); num(pct, out); kstrcat(out, "%");
}

static void append_hms(char *line, uint32_t seconds) {
    uint32_t h = seconds / 3600u;
    uint32_t m = (seconds / 60u) % 60u;
    uint32_t s2 = seconds % 60u;
    num2(h, line); kstrcat(line, ":"); num2(m, line); kstrcat(line, ":"); num2(s2, line);
}

static uint32_t uptime_seconds(void) {
    uint64_t dt = rdtsc() - SHELL_START_TSC;
    return (uint32_t)(dt / 1100000000ULL);
}

static void cmd_fastfetch(void) {
    char line[192];
    char bar[64];

    int panel_col = FF_LOGO_COLS + 2;
    int text_rows = 21;
    int block_rows = FASTFETCH_LOGO_LINES > text_rows ? FASTFETCH_LOGO_LINES : text_rows;
    int top = console_reserve_rows(block_rows + 1);

    for (int i = 0; i < FASTFETCH_LOGO_LINES; i++)
        console_draw_at(top + i, 0, FASTFETCH_LOGO[i], COL_LOGO);

    int r = top;
    console_draw_at(r, panel_col, "fukOS live system status", COL_TITLE); r++;
    console_draw_at(r, panel_col, "------------------------------------------------------------", COL_TEXT); r++;

    ff_row(r, panel_col, "OS:         ", "fukOS (x86, i686, Limine)", COL_LABEL, COL_TEXT); r++;
    ff_row(r, panel_col, "Kernel:     ", "freestanding C + custom drivers", COL_LABEL, COL_TEXT); r++;

    {
        char vendor[13], brand[49]; cpu_vendor(vendor); cpu_brand(brand);
        line[0] = 0; kstrcat(line, brand[0] ? brand : vendor);
        ff_row(r, panel_col, "CPU:        ", line, COL_LABEL, COL_TEXT); r++;
    }

    line[0] = 0; append_hms(line, uptime_seconds());
    ff_row(r, panel_col, "Uptime:     ", line, COL_LABEL, COL_TEXT); r++;

    line[0] = 0; num(FB->width, line); kstrcat(line, "x"); num(FB->height, line);
    kstrcat(line, "  "); num(FB->bpp, line); kstrcat(line, "bpp");
    ff_row(r, panel_col, "Display:    ", line, COL_LABEL, COL_TEXT); r++;

    {
        uint32_t used = (uint32_t)&_kernel_end - 0x100000u;
        uint64_t total64 = TOTAL_RAM_BYTES;
        uint32_t total_mib = total64 ? (uint32_t)(total64 >> 20) : 0;
        uint32_t used_mib = used >> 20;
        line[0] = 0; ff_bar(bar, 24, used_mib, total_mib ? total_mib : 1);
        kstrcat(line, bar); kstrcat(line, "  "); human_size(used, line); kstrcat(line, " / ");
        if (total64) human_size(total64, line); else kstrcat(line, "unknown");
        ff_row(r, panel_col, "RAM:        ", line, COL_LABEL, COL_TEXT); r++;
    }

    {
        kheap_info_t hi; kheap_get_info(&hi);
        line[0] = 0;
        if (kheap_ready()) {
            ff_bar(bar, 24, hi.used_bytes >> 20, hi.total_bytes ? (hi.total_bytes >> 20) : 1);
            kstrcat(line, bar); kstrcat(line, "  "); human_size(hi.used_bytes, line); kstrcat(line, " used / ");
            human_size(hi.total_bytes, line); kstrcat(line, "  largest "); human_size(hi.largest_free_bytes, line);
        } else kstrcat(line, "unavailable");
        ff_row(r, panel_col, "Heap:       ", line, COL_LABEL, kheap_ready() ? COL_TEXT : COL_WARN); r++;
    }

    {
        fat_debug_t d; fat_debug_info(&d);
        line[0] = 0;
        if (d.mounted) {
            uint64_t total = (uint64_t)d.total_sectors * d.bytes_per_sec;
            uint32_t freeb = fat_free_bytes();
            uint32_t used_mib = total > freeb ? (uint32_t)((total - freeb) >> 20) : 0;
            uint32_t total_mib = (uint32_t)(total >> 20);
            ff_bar(bar, 24, used_mib, total_mib ? total_mib : 1);
            kstrcat(line, bar); kstrcat(line, "  ");
            kstrcat(line, d.backend == 2 ? "USB xHCI " : d.backend == 1 ? "ATA " : "RAM ");
            kstrcat(line, d.is_fat32 ? "FAT32, free " : "FAT16, free "); human_size(freeb, line);
        } else kstrcat(line, "not mounted");
        ff_row(r, panel_col, "Disk:       ", line, COL_LABEL, d.mounted ? COL_TEXT : COL_WARN); r++;
    }

    line[0] = 0;
    kstrcat(line, hda_output_is_headphones() ? "headphones" : "speaker/line-out");
    kstrcat(line, "  mode=");
    int mode = hda_get_output_mode(); kstrcat(line, mode == 0 ? "auto" : mode == 1 ? "speaker" : "headphones");
    kstrcat(line, "  volume="); num(hda_bg_get_volume(), line); kstrcat(line, "%");
    ff_row(r, panel_col, "Audio:      ", line, COL_LABEL, COL_TEXT); r++;

    line[0] = 0;
    if (hda_bg_is_active()) {
        kstrcat(line, hda_bg_is_paused() ? "PAUSED  " : "PLAYING ");
        kstrcat(line, g_bg_track[0] ? g_bg_track : "background stream");
        kstrcat(line, "  "); append_time(line, (uint32_t)(hda_bg_played_bytes() / 192000u));
        kstrcat(line, " / "); append_time(line, g_bg_total_seconds);
    } else kstrcat(line, g_bg_queue_active ? "switching tracks" : "not playing");
    ff_row(r, panel_col, "bgplay:     ", line, COL_LABEL, hda_bg_is_active() ? COL_LS_WAV : COL_TEXT); r++;

    {
        rtc_time_t t; rtc_read(&t); line[0] = 0;
        num(t.year, line); kstrcat(line, "-"); num2(t.month, line); kstrcat(line, "-"); num2(t.day, line);
        kstrcat(line, "  "); num2(t.hour, line); kstrcat(line, ":"); num2(t.minute, line); kstrcat(line, ":"); num2(t.second, line);
        ff_row(r, panel_col, "RTC:        ", line, COL_LABEL, COL_TEXT); r++;
    }

}

static void cmd_help(void) {
    console_puts("Available commands:\n");
    console_puts("  help              show this help\n");
    console_puts("  ls / dir          list files & folders in the current directory\n");
    console_puts("  cd <dir>          enter a folder ('cd ..' up, 'cd /' root)\n");
    console_puts("  pwd               print the current directory\n");
    console_puts("  tree              show entries as a directory tree\n");
    console_puts("  cat <file>        show a text file, or an image's info\n");
    console_puts("  head <file>       show the first ten lines\n");
    console_puts("  wc <file>         count lines, words and bytes\n");
    console_puts("  hexdump <file>    show the first 256 bytes in hexadecimal\n");
    console_puts("  play <file.wav>   play WAV + auto-next (Space pause, arrows seek/volume)\n");
    console_puts("  play *.wav        play all WAV files in current directory\n");
    console_puts("  playlist          same as 'play *.wav'; R repeat, S shuffle, Esc stop\n");
    console_puts("  bgplay <file.wav> start one background track (keeps playing in edit)\n");
    console_puts("  bgplay *.wav      queue every WAV in the current directory\n");
    console_puts("  bgrepeat          toggle repeating the current background track\n");
    console_puts("  bgshuffle         toggle random next tracks in background queue\n");
    console_puts("  bgnext / bgprev   switch background queue track immediately\n");
    console_puts("  Shift+Up/Down     scroll shell output; PgUp/PgDn scroll a page\n");
    console_puts("  bgpause/bgresume  pause or resume background music\n");
    console_puts("  bgstop/bgstatus   stop or inspect background music\n");
    console_puts("  bgvolume <0-100>  set background music volume\n");
    console_puts("  audioout [mode]   auto/speaker/headphones HDA output selection\n");
    console_puts("  img/photo [file]  full-screen gallery; n/p navigate, m fill, i info\n");
    console_puts("  open <file>       open by type: text->edit, bmp->photo, wav->play\n");
    console_puts("  matrix            green digital-rain screensaver (Q/Esc exits)\n");
    console_puts("  screenshot [bmp]  save the current screen as a 24-bit BMP\n");
    console_puts("  F12               instant screenshot from anywhere (shot1.bmp, shot2...)\n");
    console_puts("  clear / cls       clear the screen\n");
    console_puts("  res               show screen resolution\n");
    console_puts("  about             about fukOS\n");
    console_puts("  reboot            restart the computer\n");
    console_puts("  poweroff          turn the computer off (ACPI)\n");
    console_puts("  lspci             list PCI devices (bus:slot.func, ids, class)\n");
    console_puts("  usb               look for a USB (xHCI) controller on the PCI bus\n");
    console_puts("  diskinfo          show mounted FAT geometry + storage backend (debug)\n");
    console_puts("  time / date       show the current date & time (CMOS RTC)\n");
    console_puts("  clock             full-screen flip clock (Q/Esc/Enter exits)\n");
    console_puts("  touch <file>      create a new empty file\n");
    console_puts("  mkdir <name>      create a new folder\n");
    console_puts("  rmdir <folder>    delete an empty folder\n");
    console_puts("  rm <file>         delete a file\n");
    console_puts("  cp <src> <dst>    copy a file\n");
    console_puts("  mv <src> <dst>    move or rename a file, including across folders\n");
    console_puts("  echo <txt> > <f>  write text to a file  (>> to append)\n");
    console_puts("  edit <file>       nano editor: arrows, Ctrl+K del-line, Ctrl+Z undo, Ctrl+Q quit, ESC save\n");
    console_puts("  fastfetch / ff    show a system-info screen (CPU, RAM, display, storage...)\n");
    console_puts("  heaptest          test kernel heap alloc/free/realloc/calloc\n");
    console_puts("  irqinfo           show timer IRQ and COM1 status\n");
    console_puts("  panic-test confirm  trigger a deliberate diagnostic panic\n");
    console_puts("  doom              play DOOM! (arrows move, Ctrl fire, Space use, Esc menu)\n");
    console_puts("\nTip: plug the stick into a PC and just copy files/folders onto\n");
    console_puts("it - they show up here (it is a normal FAT drive).\n");
}

static int heap_pattern_ok(uint8_t *p, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) if (p[i] != (uint8_t)(seed + i * 13u)) return 0;
    return 1;
}

static void heap_fill_pattern(uint8_t *p, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 13u);
}

static void cmd_heaptest(void) {
    console_puts("heaptest: starting...\n");
    if (!kheap_ready()) { console_puts("heaptest: heap is not initialized.\n"); return; }

    kheap_info_t before, after;
    kheap_get_info(&before);
    void *a = kheap_alloc(1234);
    void *b = kheap_alloc(65536);
    void *c = kheap_calloc(257, 17);
    if (!a || !b || !c) {
        console_puts("heaptest: FAIL allocation returned NULL.\n");
        if (a) kheap_free(a);
        if (b) kheap_free(b);
        if (c) kheap_free(c);
        return;
    }

    heap_fill_pattern((uint8_t *)a, 1234, 0x11);
    heap_fill_pattern((uint8_t *)b, 65536, 0x22);
    uint8_t *cp = (uint8_t *)c;
    for (uint32_t i = 0; i < 257u * 17u; i++) {
        if (cp[i] != 0) {
            console_puts("heaptest: FAIL calloc did not zero memory.\n");
            kheap_free(a); kheap_free(b); kheap_free(c); return;
        }
    }

    void *a2 = kheap_realloc(a, 4096);
    if (!a2 || !heap_pattern_ok((uint8_t *)a2, 1234, 0x11) ||
        !heap_pattern_ok((uint8_t *)b, 65536, 0x22)) {
        console_puts("heaptest: FAIL realloc/growth corrupted data.\n");
        if (a2) kheap_free(a2); else kheap_free(a);
        kheap_free(b); kheap_free(c); return;
    }
    a = a2;

    kheap_free(b);
    void *d = kheap_alloc(32768);
    void *e = kheap_alloc(32768);
    if (!d || !e) {
        console_puts("heaptest: FAIL split/reuse after free.\n");
        kheap_free(a); if (d) kheap_free(d); if (e) kheap_free(e); kheap_free(c); return;
    }
    heap_fill_pattern((uint8_t *)d, 32768, 0x33);
    heap_fill_pattern((uint8_t *)e, 32768, 0x44);

    void *d2 = kheap_realloc(d, 16384);
    if (!d2 || !heap_pattern_ok((uint8_t *)d2, 16384, 0x33)) {
        console_puts("heaptest: FAIL realloc/shrink corrupted data.\n");
        if (d2) kheap_free(d2); else kheap_free(d);
        kheap_free(e); kheap_free(a); kheap_free(c); return;
    }
    d = d2;

    kheap_free(c); kheap_free(e); kheap_free(d); kheap_free(a);
    kheap_get_info(&after);

    char line[160]; line[0] = 0;
    kstrcat(line, "heaptest: OK. before free="); human_size(before.free_bytes, line);
    kstrcat(line, " largest="); human_size(before.largest_free_bytes, line);
    kstrcat(line, "; after free="); human_size(after.free_bytes, line);
    kstrcat(line, " largest="); human_size(after.largest_free_bytes, line);
    kstrcat(line, " allocs="); num(after.allocations, line); kstrcat(line, "\n");
    console_puts(line);
}

static void cmd_irqinfo(void) {
    char line[96];
    line[0] = 0;
    kstrcat(line, "PIT ticks: ");
    num(interrupts_ticks(), line);
    kstrcat(line, " (100 Hz)  COM1: ");
    kstrcat(line, serial_ready() ? "ready" : "unavailable");
    kstrcat(line, "\n");
    console_puts(line);
}

static void cmd_panic_test(const char *arg) {
    if (!streq(arg, "confirm")) {
        console_puts("This command intentionally halts the kernel.\n");
        console_puts("Run 'panic-test confirm' to test the panic screen and serial log.\n");
        return;
    }
    __asm__ volatile ("int $3");
    kernel_panic("breakpoint handler returned unexpectedly");
}

static void print_pci_dev(const pci_dev_t *d) {
    char line[128];
    line[0] = 0;
    num(d->bus, line);   kstrcat(line, ":");
    num(d->slot, line);  kstrcat(line, ".");
    num(d->func, line);  kstrcat(line, "  ");
    kstrcat(line, "ven="); hex4(d->vendor_id, line);
    kstrcat(line, " dev="); hex4(d->device_id, line);
    kstrcat(line, "  class="); hex(d->class_code, line);
    kstrcat(line, "."); hex(d->subclass, line);
    kstrcat(line, "."); hex(d->prog_if, line);
    if (d->class_code == 0x04 && d->subclass == 0x03)
        kstrcat(line, "  (High Definition Audio)");
    if (d->class_code == 0x0C && d->subclass == 0x03) {
        if (d->prog_if == 0x30)      kstrcat(line, "  (xHCI/USB3)");
        else if (d->prog_if == 0x20) kstrcat(line, "  (EHCI/USB2)");
        else if (d->prog_if == 0x00) kstrcat(line, "  (UHCI/USB1)");
        else                          kstrcat(line, "  (USB ctrl)");
    }
    kstrcat(line, "\n");
    console_puts(line);
}

static void cmd_lspci(void) {
    int n = pci_scan(print_pci_dev);
    char line[64];
    line[0] = 0;
    num((uint32_t)n, line);
    kstrcat(line, " device(s) found.\n");
    console_puts(line);
}

static const char *port_speed_name(uint32_t portsc) {
    uint32_t speed = (portsc >> 10) & 0xF;
    switch (speed) {
        case 1: return "Full-speed (12 Mbps)";
        case 2: return "Low-speed (1.5 Mbps)";
        case 3: return "High-speed (480 Mbps)";
        case 4: return "SuperSpeed (5 Gbps)";
        case 5: return "SuperSpeedPlus (10 Gbps)";
        default: return "unknown speed";
    }
}

static void print_port(int port_num, uint32_t portsc) {
    char line[96];
    line[0] = 0;
    kstrcat(line, "  port ");
    num((uint32_t)port_num, line);
    kstrcat(line, ": connected, ");
    kstrcat(line, port_speed_name(portsc));
    kstrcat(line, "\n");
    console_puts(line);
}

static void print_protocol(int major, int port_offset, int port_count) {
    char line[96];
    line[0] = 0;
    kstrcat(line, "  major="); num((uint32_t)major, line);
    kstrcat(line, major == 2 ? " (USB2)" : (major == 3 ? " (USB3/SuperSpeed)" : " (?)"));
    kstrcat(line, "  ports "); num((uint32_t)port_offset, line);
    kstrcat(line, ".."); num((uint32_t)(port_offset + port_count - 1), line);
    kstrcat(line, "\n");
    console_puts(line);
}

static void cmd_diskinfo(void) {
    fat_debug_t d;
    fat_debug_info(&d);
    char line[128];

    if (!d.mounted) {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("No filesystem mounted.\n");
        console_set_colors(COL_TEXT, COL_BG);
        return;
    }

    line[0] = 0;
    kstrcat(line, "Backend: ");
    kstrcat(line, d.backend == 2 ? "USB (xHCI)" : d.backend == 1 ? "ATA" : d.backend == 3 ? "RAM image" : "none");
    kstrcat(line, "\n");
    console_puts(line);

    line[0] = 0; kstrcat(line, "FAT type: "); kstrcat(line, d.is_fat32 ? "FAT32" : "FAT16"); kstrcat(line, "\n");
    console_puts(line);

    line[0] = 0; kstrcat(line, "Bytes/sector: "); num(d.bytes_per_sec, line); kstrcat(line, "   Sectors/cluster: "); num(d.sectors_per_cluster, line); kstrcat(line, "\n");
    console_puts(line);

    line[0] = 0; kstrcat(line, "Reserved secs: "); num(d.reserved_sectors, line); kstrcat(line, "   FATs: "); num(d.num_fats, line); kstrcat(line, "   FAT size (secs): "); num(d.fat_size_sectors, line); kstrcat(line, "\n");
    console_puts(line);

    line[0] = 0; kstrcat(line, "Root dir secs: "); num(d.root_dir_sectors, line); kstrcat(line, "   Data start sec: "); num(d.data_start_sector, line); kstrcat(line, "\n");
    console_puts(line);

    line[0] = 0; kstrcat(line, "Total sectors: "); num(d.total_sectors, line); kstrcat(line, "   Root cluster: "); num(d.root_cluster, line); kstrcat(line, "   Part LBA: "); num(d.part_lba, line); kstrcat(line, "\n");
    console_puts(line);

    if (d.backend == 2) {
        line[0] = 0;
        kstrcat(line, "xHCI ready: "); kstrcat(line, xhci_msc_ready() ? "yes" : "NO");
        kstrcat(line, "   block size: "); num(xhci_msc_block_size(), line);
        kstrcat(line, "   block count: "); num(xhci_msc_block_count(), line);
        kstrcat(line, "\n");
        console_puts(line);
    }

    line[0] = 0; kstrcat(line, "Free space: "); num(fat_free_bytes(), line); kstrcat(line, " bytes\n");
    console_puts(line);
}

static void cmd_usb(void) {
    pci_dev_t d;
    if (!pci_find_class(0x0C, 0x03, 0x30, &d)) {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("No xHCI (USB3) controller found on the PCI bus.\n");
        console_set_colors(COL_TEXT, COL_BG);
        return;
    }

    char line[128];
    line[0] = 0;
    kstrcat(line, "xHCI controller at ");
    num(d.bus, line); kstrcat(line, ":");
    num(d.slot, line); kstrcat(line, ".");
    num(d.func, line);
    kstrcat(line, "  ven="); hex4(d.vendor_id, line);
    kstrcat(line, " dev="); hex4(d.device_id, line);
    kstrcat(line, "\n");
    console_puts(line);

    uint64_t bar0 = pci_bar_address(d.bus, d.slot, d.func, 0);
    pci_enable_device(d.bus, d.slot, d.func);

    line[0] = 0;
    kstrcat(line, "MMIO base (BAR0): 0x");
    hex((uint32_t)(bar0 >> 32), line);
    hex((uint32_t)bar0, line);
    kstrcat(line, "\n");
    console_puts(line);
    console_puts("Memory Space + Bus Master enabled.\n");

    if (bar0 == 0) {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("BAR0 is an I/O-space BAR or empty -- cannot map registers.\n");
        console_set_colors(COL_TEXT, COL_BG);
        return;
    }

    xhci_hc_t hc;
    int ok = xhci_init(bar0, &hc);

    line[0] = 0;
    kstrcat(line, "slots="); num(hc.max_slots, line);
    kstrcat(line, " ports="); num(hc.max_ports, line);
    kstrcat(line, " ctxsize="); num(hc.context_size, line);
    kstrcat(line, "\n");
    console_puts(line);

    if (!ok) {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("Controller did not reach the Running state (timed out).\n");
        console_set_colors(COL_TEXT, COL_BG);
        return;
    }
    console_set_colors(COL_PROMPT, COL_BG);
    console_puts("Controller is running.\n");
    console_set_colors(COL_TEXT, COL_BG);

    int n = xhci_scan_ports(&hc, print_port);
    line[0] = 0;
    num((uint32_t)n, line);
    kstrcat(line, " port(s) report a connected device.\n");
    console_puts(line);

    console_puts("Port protocol ranges (which ports are USB2 vs USB3):\n");
    xhci_scan_protocols(&hc, print_protocol);

    console_puts("Raw port status:\n");
    for (uint32_t p = 1; p <= hc.max_ports; p++) {
        uint32_t portsc = xhci_portsc(&hc, (int)p);
        uint32_t pls = (portsc >> 5) & 0xF;
        line[0] = 0;
        kstrcat(line, "  port "); num(p, line);
        kstrcat(line, ": raw=0x"); hex(portsc, line);
        kstrcat(line, " ccs="); num(portsc & 1u, line);
        kstrcat(line, " pp=");  num((portsc >> 9) & 1u, line);
        kstrcat(line, " pls="); num(pls, line);
        kstrcat(line, " (");
        switch (pls) {
            case 0:  kstrcat(line, "U0"); break;
            case 2:  kstrcat(line, "U2"); break;
            case 4:  kstrcat(line, "Disabled"); break;
            case 5:  kstrcat(line, "RxDetect"); break;
            case 7:  kstrcat(line, "Polling"); break;
            case 15: kstrcat(line, "Resume"); break;
            default: kstrcat(line, "other"); break;
        }
        kstrcat(line, ")\n");
        console_puts(line);
    }

    console_puts("Probing connected devices (Enable Slot + Address Device + GET_DESCRIPTOR):\n");
    for (uint32_t p = 1; p <= hc.max_ports; p++) {
        uint32_t ps = xhci_portsc(&hc, (int)p);
        if (!(ps & 1u)) continue;
        xhci_dev_info_t info;
        int pok = xhci_probe_port(&hc, (int)p, &info);
        line[0] = 0;
        kstrcat(line, "  port "); num(p, line); kstrcat(line, ": ");
        if (!pok) {
            kstrcat(line, "probe failed at stage "); num(info.fail_stage, line);
            switch (info.fail_stage) {
                case 1: kstrcat(line, " (lost CCS)"); break;
                case 2: kstrcat(line, " (port reset did not reach Enabled)"); break;
                case 3: kstrcat(line, " (Enable Slot cmd failed)"); break;
                case 4: kstrcat(line, " (Address Device cmd failed)"); break;
                case 5: kstrcat(line, " (GET_DESCRIPTOR transfer failed)"); break;
                default: kstrcat(line, " (unknown)"); break;
            }
            kstrcat(line, " detail=0x"); hex(info.fail_detail, line);
            kstrcat(line, "\n");
            console_puts(line);
            continue;
        }
        kstrcat(line, "class="); hex4(info.usb_class, line);
        kstrcat(line, " sub="); hex4(info.usb_subclass, line);
        kstrcat(line, " vid="); hex4(info.vendor_id, line);
        kstrcat(line, " pid="); hex4(info.product_id, line);
        kstrcat(line, "\n");
        console_puts(line);

        if (info.if_class == 0x08) {
            line[0] = 0;
            kstrcat(line, "        --> Mass Storage! ifclass="); hex4(info.if_class, line);
            kstrcat(line, " sub="); hex4(info.if_subclass, line);
            kstrcat(line, " proto="); hex4(info.if_proto, line);
            kstrcat(line, "\n");
            console_puts(line);
            line[0] = 0;
            kstrcat(line, "            bulk IN=0x"); hex(info.bulk_in_ep, line);
            kstrcat(line, " (mps "); num(info.bulk_in_mps, line);
            kstrcat(line, ")  bulk OUT=0x"); hex(info.bulk_out_ep, line);
            kstrcat(line, " (mps "); num(info.bulk_out_mps, line);
            kstrcat(line, ")\n");
            console_puts(line);
            if (info.if_subclass == 0x06 && info.if_proto == 0x50) {
                console_puts("            SCSI transparent + Bulk-Only Transport -- ready for BOT!\n");
            }

            if (info.storage_ok) {
                line[0] = 0;
                kstrcat(line, "            SCSI OK  vendor='"); kstrcat(line, info.inq_vendor);
                kstrcat(line, "' product='"); kstrcat(line, info.inq_product);
                kstrcat(line, "'\n");
                console_puts(line);
                line[0] = 0;
                kstrcat(line, "            capacity: "); num(info.block_count, line);
                kstrcat(line, " x "); num(info.block_size, line);
                kstrcat(line, "-byte blocks\n");
                console_puts(line);
                line[0] = 0;
                kstrcat(line, "            LBA0: ");
                for (int b = 0; b < 16; b++) { hex(info.sector0[b], line); kstrcat(line, " "); }
                kstrcat(line, "\n");
                console_puts(line);
                console_puts(info.boot_sig_ok
                    ? "            boot signature 0x55AA present -- valid boot sector!\n"
                    : "            (no 0x55AA at offset 510)\n");
            } else {
                line[0] = 0;
                kstrcat(line, "            SCSI read FAILED at stage "); num(info.storage_stage, line);
                kstrcat(line, " (1=cfg 2=inquiry 3=capacity 4=read)\n");
                console_puts(line);
            }
        }
    }
}

static void cmd_res(void) {
    char line[64];
    line[0] = 0;
    kstrcat(line, "Resolution: ");
    num(FB->width,  line); kstrcat(line, "x");
    num(FB->height, line); kstrcat(line, " @ ");
    num(FB->bpp,    line); kstrcat(line, " bpp\n");
    console_puts(line);
}

static void print_cwd(void) {
    char p[256]; p[0] = 0;
    fat_cwd_path(p, sizeof p);
    console_puts(p);
}

static int looks_like_text(uint32_t got);

static void cmd_ls(void) {
    if (!fat_mounted()) {
        console_puts("No storage found. 'img' shows the built-in demo image.\n");
        char line[80];
        line[0] = 0;
        kstrcat(line, "Built-in: demo  ");
        num(demo_image_width, line); kstrcat(line, "x");
        num(demo_image_height, line); kstrcat(line, "  (fallback image)\n");
        console_puts(line);
        return;
    }

    uint32_t count = fat_dir_count();
    console_puts("Directory: "); print_cwd(); console_puts("\n");
    if (count == 0) { console_puts("  (empty)\n"); return; }

    char hdr[96];
    hdr[0] = 0;
    kstrcat(hdr, "  #  NAME");            pad_to(hdr, 31);
    kstrcat(hdr, "TYPE");                 pad_to(hdr, 38);
    kstrcat(hdr, "SIZE");                 pad_to(hdr, 48);
    kstrcat(hdr, "DIMENSIONS\n");
    console_puts(hdr);

    for (uint32_t i = 0; i < count; i++) {
        fat_dirent e;
        if (!fat_dir_get(i, &e)) continue;

        char line[160];
        line[0] = 0;
        kstrcat(line, "  ");
        num(i, line);
        kstrcat(line, "  ");
        kstrcat(line, e.name);
        pad_to(line, 31);

        if (e.is_dir) {
            kstrcat(line, "<DIR>");   pad_to(line, 38);
            kstrcat(line, "-");       pad_to(line, 48);
            kstrcat(line, "-\n");
            console_set_colors(COL_DIR, COL_BG);
            console_puts(line);
            console_set_colors(COL_TEXT, COL_BG);
            continue;
        }

        image_t probe;
        uint32_t got = fat_read_file(e.name, file_buf,
                                     e.size < 4096 ? e.size : 4096);
        const char *kind = img_kind(file_buf, got, &probe);
        int is_wav = has_ext_ci(e.name, ".wav");
        kstrcat(line, kind ? kind : (is_wav ? "WAV" : "file"));
        pad_to(line, 38);
        num(e.size, line); kstrcat(line, "B");
        pad_to(line, 48);
        if (kind) { num(probe.width, line); kstrcat(line, "x"); num(probe.height, line); }
        else      { kstrcat(line, "-"); }
        kstrcat(line, "\n");
        uint32_t col = kind ? COL_LS_IMG
                            : (is_wav ? COL_LS_WAV
                                      : (looks_like_text(got) ? COL_LS_TXT : COL_TEXT));
        console_set_colors(col, COL_BG);
        console_puts(line);
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_cd(const char *arg) {
    if (!fat_mounted()) { console_puts("cd: no storage mounted.\n"); return; }
    if (!arg || !*arg) { print_cwd(); console_puts("\n"); return; }
    if (!fat_chdir(arg)) {
        console_puts("cd: no such directory: ");
        console_puts(arg);
        console_puts("\n");
    }
}

static void print_as_text(uint32_t got) {
    uint32_t printable = 0, cap = got < 8192 ? got : 8192;
    for (uint32_t i = 0; i < cap; i++) {
        uint8_t c = file_buf[i];
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127)) printable++;
    }
    if (cap == 0 || printable * 100u / cap < 85u) {
        console_puts("(binary file - not text or a known image)\n");
        return;
    }
    console_puts("----------------------------------------\n");
    for (uint32_t i = 0; i < cap; i++) {
        char c = (char)file_buf[i];
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || (file_buf[i] >= 32 && file_buf[i] < 127)) {
            char s[2] = { c, 0 };
            console_puts(s);
        }
    }
    if (got > cap) console_puts("\n... (truncated)\n");
    else           console_puts("\n----------------------------------------\n");
}

static int looks_like_text(uint32_t got) {
    uint32_t cap = got < 65536u ? got : 65536u;
    if (cap == 0) return 0;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < cap; i++) {
        uint8_t c = file_buf[i];
        if (c == 0) return 0;
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 32) bad++;
    }
    return (bad * 100u / cap) < 5u;
}

static void draw_text_row(int y, uint32_t off, uint32_t len, int text_cols) {
    int col = 0;
    uint32_t k = 0;
    while (k < len && col < text_cols) {
        uint8_t c = file_buf[off + k];
        if (c == '\r') { k++; continue; }
        if (c == '\t') { col += 4 - (col & 3); k++; continue; }
        uint32_t cp = c;
        int adv = 1;
        if (c >= 0xF0)      { cp = (uint32_t)(c & 0x07); adv = 4; }
        else if (c >= 0xE0) { cp = (uint32_t)(c & 0x0F); adv = 3; }
        else if (c >= 0xC0) { cp = (uint32_t)(c & 0x1F); adv = 2; }
        for (int j = 1; j < adv; j++) {
            if (k + (uint32_t)j >= len) { adv = j; break; }
            uint8_t cc = file_buf[off + k + (uint32_t)j];
            if ((cc & 0xC0) != 0x80) { adv = j; break; }
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        fb_draw_glyph(FB, TXT_MARGIN + col * FB_GLYPH_W, y, cp, COL_TEXT);
        col++;
        k += (uint32_t)adv;
    }
}

static void view_text(const char *name, uint32_t got) {
    int lh  = FB_GLYPH_H + 2;
    int top = lh + 6;
    int bot = lh + 6;
    int text_cols = ((int)FB->width - 2 * TXT_MARGIN) / FB_GLYPH_W;
    int text_rows = ((int)FB->height - top - bot) / lh;
    if (text_cols < 1) text_cols = 1;
    if (text_rows < 1) text_rows = 1;

    uint32_t nrows = 0, i = 0;
    while (i < got && nrows < MAX_TEXTROWS) {
        uint32_t start = i;
        int col = 0;
        while (i < got) {
            uint8_t c = file_buf[i];
            if (c == '\n') break;
            if (c == '\r') { i++; continue; }
            if ((c & 0xC0) == 0x80) { i++; continue; }
            int adv = (c == '\t') ? (4 - (col & 3)) : 1;
            if (col + adv > text_cols) break;
            col += adv; i++;
        }
        txt_row_off[nrows] = start;
        txt_row_len[nrows] = i - start;
        nrows++;
        if (i < got && file_buf[i] == '\n') i++;
    }
    if (nrows == 0) { txt_row_off[0] = 0; txt_row_len[0] = 0; nrows = 1; }
    int truncated = (i < got);

    int topr = 0, redraw = 1;
    for (;;) {
        if (redraw) {
            fb_clear(FB, COL_BG);
            char title[160]; title[0] = 0;
            kstrcat(title, "TEXT  "); kstrcat(title, name);
            fb_draw_string(FB, TXT_MARGIN, 3, title, COL_TITLE);

            for (int r = 0; r < text_rows && (uint32_t)(topr + r) < nrows; r++)
                draw_text_row(top + r * lh, txt_row_off[topr + r],
                              txt_row_len[topr + r], text_cols);

            uint32_t last = (uint32_t)topr + (uint32_t)text_rows;
            if (last > nrows) last = nrows;
            char st[200]; st[0] = 0;
            kstrcat(st, "lines "); num((uint32_t)topr + 1, st);
            kstrcat(st, "-"); num(last, st);
            kstrcat(st, "/"); num(nrows, st);
            if (truncated) kstrcat(st, " (truncated)");
            kstrcat(st, "   j/k or Up/Down, Space/b page, g/G ends, q quit");
            fb_fill_rect(FB, 0, (int)FB->height - bot, (int)FB->width, bot, 0x141A28);
            fb_draw_string(FB, TXT_MARGIN, (int)FB->height - lh, st, COL_PROMPT);
            redraw = 0;
        }

        char c = kbd_getchar();
        if (c == KEY_F12) { capture_screenshot_auto(FB); continue; }
        int old = topr;
        int page = text_rows - 1; if (page < 1) page = 1;
        if (c == 'q' || c == 'Q' || c == 27 || c == '\n') break;
        else if (c == 'j' || c == 'J' || c == KEY_DOWN) topr++;
        else if (c == 'k' || c == 'K' || c == KEY_UP)   topr--;
        else if (c == ' ' || c == 'f' || c == KEY_PGDN) topr += page;
        else if (c == 'b' || c == 'B' || c == KEY_PGUP) topr -= page;
        else if (c == 'g' || c == KEY_HOME)             topr = 0;
        else if (c == 'G' || c == KEY_END)              topr = (int)nrows - text_rows;

        int maxtop = (int)nrows - text_rows; if (maxtop < 0) maxtop = 0;
        if (topr > maxtop) topr = maxtop;
        if (topr < 0) topr = 0;
        if (topr != old) redraw = 1;
    }

    console_init(FB);
    console_set_colors(COL_TEXT, COL_BG);
}

static void cmd_cat(const char *arg) {
    if (!arg || !*arg) {
        console_puts("Usage: cat <file>   (see 'ls' for names)\n");
        return;
    }

    char namebuf[128];
    const char *name = arg;
    int ok; uint32_t n = parse_uint(arg, &ok);
    if (ok) {
        fat_dirent d;
        if (fat_dir_get(n, &d) && !d.is_dir) {
            int k = 0; for (; d.name[k] && k < 127; k++) namebuf[k] = d.name[k];
            namebuf[k] = 0; name = namebuf;
        }
    }

    uint32_t got = fat_read_file(name, file_buf, sizeof file_buf);
    if (!got) {
        console_puts("cat: no such file: ");
        console_puts(arg);
        console_puts("\n");
        return;
    }

    char line[160];
    line[0] = 0; kstrcat(line, "File: "); kstrcat(line, name); kstrcat(line, "\n");
    console_puts(line);

    image_t probe;
    const char *kind = img_kind(file_buf, got, &probe);
    if (kind) {
        line[0] = 0;
        kstrcat(line, "Type: "); kstrcat(line, kind); kstrcat(line, "  ");
        num(probe.width, line);  kstrcat(line, "x");
        num(probe.height, line); kstrcat(line, " @ ");
        num(probe.bpp, line);    kstrcat(line, " bpp\n");
        console_puts(line);
        console_puts("Hint: type 'img "); console_puts(name);
        console_puts("' to view it.\n");
    } else if (looks_like_text(got)) {
        view_text(name, got);
    } else {
        print_as_text(got);
    }
}

static uint16_t wav_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t wav_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int wav_fourcc(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

typedef struct {
    fat_file file;
    uint32_t data_offset, data_bytes;
    uint32_t source_rate, frames_total, frame_index, play_base_frame;
    uint16_t channels, bits, block_align, sample_bytes;
    int status_row;
    char track_name[128];
    uint32_t phase;
    int16_t cur_l, cur_r, next_l, next_r;
    int primed;
    uint8_t cache[64u * 1024u];
    uint32_t cache_pos, cache_len;
} wav_stream;

static wav_stream g_wav_stream;

static int16_t wav_read_sample(const uint8_t *q, uint16_t bits) {
    int32_t v;
    if (bits == 8u) v = ((int32_t)q[0] - 128) << 8;
    else if (bits == 16u) v = (int16_t)wav_u16(q);
    else if (bits == 24u) {
        v = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                      ((uint32_t)q[2] << 16));
        if (v & 0x00800000) v |= (int32_t)0xFF000000u;
        v >>= 8;
    } else { v = (int32_t)wav_u32(q); v >>= 16; }
    return (int16_t)v;
}

static int16_t wav_lerp(int16_t a, int16_t b, uint32_t phase) {
    int32_t delta = (int32_t)b - (int32_t)a;
    uint32_t magnitude = delta < 0 ? (uint32_t)(-delta) : (uint32_t)delta;
    int32_t step = (int32_t)((magnitude * phase) / 48000u);
    return (int16_t)((int32_t)a + (delta < 0 ? -step : step));
}

static int wav_file_read_exact(fat_file *f, uint8_t *dst, uint32_t bytes) {
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t n = fat_read(f, dst + done, bytes - done);
        if (!n) return 0;
        done += n;
    }
    return 1;
}

static int wav_stream_parse(wav_stream *w) {
    uint8_t riff[12];
    if (!wav_file_read_exact(&w->file, riff, sizeof riff) ||
        !wav_fourcc(riff, "RIFF") || !wav_fourcc(riff + 8, "WAVE")) return 0;

    uint16_t format = 0;
    uint32_t off = 12u;
    while (off + 8u <= w->file.size) {
        uint8_t hdr[8];
        if (!fat_seek(&w->file, off) || !wav_file_read_exact(&w->file, hdr, 8u)) return 0;
        uint32_t len = wav_u32(hdr + 4u), body = off + 8u;
        if (body > w->file.size || len > w->file.size - body) return 0;
        if (wav_fourcc(hdr, "fmt ") && len >= 16u) {
            uint8_t fmt[40]; uint32_t take = len < sizeof fmt ? len : sizeof fmt;
            if (!fat_seek(&w->file, body) || !wav_file_read_exact(&w->file, fmt, take)) return 0;
            format = wav_u16(fmt);
            w->channels = wav_u16(fmt + 2u);
            w->source_rate = wav_u32(fmt + 4u);
            w->block_align = wav_u16(fmt + 12u);
            w->bits = wav_u16(fmt + 14u);
            if (format == 0xFFFEu && len >= 40u && wav_u32(fmt + 24u) == 1u)
                format = 1u;
        } else if (wav_fourcc(hdr, "data")) {
            w->data_offset = body; w->data_bytes = len;
        }
        uint32_t next = body + len + (len & 1u);
        if (next <= off) return 0;
        off = next;
        if (format && w->data_offset) break;
    }
    if (format != 1u || (w->channels != 1u && w->channels != 2u) ||
        (w->bits != 8u && w->bits != 16u && w->bits != 24u && w->bits != 32u) ||
        w->source_rate < 4000u || w->source_rate > 192000u || !w->data_offset)
        return 0;
    w->sample_bytes = w->bits / 8u;
    if (!w->block_align || w->block_align > 32u ||
        w->block_align < w->channels * w->sample_bytes) return 0;
    w->frames_total = w->data_bytes / w->block_align;
    if (!w->frames_total || !fat_seek(&w->file, w->data_offset)) return 0;
    return 1;
}

static int wav_stream_open(const char *name, wav_stream *w) {
    memset(w, 0, sizeof *w);
    if (!fat_open(name, &w->file)) return 0;
    return wav_stream_parse(w);
}

static int wav_stream_open_entry(const fat_dirent *entry, wav_stream *w) {
    if (!entry || entry->is_dir || entry->first_cluster < 2u || !entry->size) return 0;
    memset(w, 0, sizeof *w);
    w->file.first_cluster = entry->first_cluster;
    w->file.size = entry->size;
    w->file.position = 0;
    w->file.cluster = entry->first_cluster;
    w->file.cluster_offset = 0;
    return wav_stream_parse(w);
}

static uint32_t wav_cached_read(wav_stream *w, uint8_t *dst, uint32_t bytes) {
    uint32_t done = 0;
    while (done < bytes) {
        if (w->cache_pos == w->cache_len) {
            w->cache_len = fat_read(&w->file, w->cache, sizeof w->cache);
            w->cache_pos = 0;
            if (!w->cache_len) break;
        }
        uint32_t avail = w->cache_len - w->cache_pos;
        uint32_t take = bytes - done < avail ? bytes - done : avail;
        memcpy(dst + done, w->cache + w->cache_pos, take);
        w->cache_pos += take; done += take;
    }
    return done;
}

static int wav_next_frame(wav_stream *w, int16_t *left, int16_t *right) {
    uint8_t frame[32];
    if (wav_cached_read(w, frame, w->block_align) != w->block_align) return 0;
    *left = wav_read_sample(frame, w->bits);
    *right = w->channels == 2u ? wav_read_sample(frame + w->sample_bytes, w->bits)
                               : *left;
    return 1;
}

static uint32_t wav_stream_fill(void *context, int16_t *dst, uint32_t max_bytes) {
    wav_stream *w = (wav_stream *)context;
    if (!w->primed) {
        if (!wav_next_frame(w, &w->cur_l, &w->cur_r)) return 0;
        if (w->frames_total > 1u) {
            if (!wav_next_frame(w, &w->next_l, &w->next_r)) return 0;
        } else { w->next_l = w->cur_l; w->next_r = w->cur_r; }
        w->primed = 1;
    }
    uint32_t capacity = max_bytes / 4u, made = 0;
    while (made < capacity && w->frame_index < w->frames_total) {
        dst[made * 2u] = wav_lerp(w->cur_l, w->next_l, w->phase);
        dst[made * 2u + 1u] = wav_lerp(w->cur_r, w->next_r, w->phase);
        made++;
        w->phase += w->source_rate;
        while (w->phase >= 48000u && w->frame_index < w->frames_total) {
            w->phase -= 48000u;
            w->frame_index++;
            if (w->frame_index >= w->frames_total) break;
            w->cur_l = w->next_l; w->cur_r = w->next_r;
            if (w->frame_index + 1u < w->frames_total) {
                if (!wav_next_frame(w, &w->next_l, &w->next_r)) {
                    w->frame_index = w->frames_total; break;
                }
            } else { w->next_l = w->cur_l; w->next_r = w->cur_r; }
        }
    }
    return made * 4u;
}

static int wav_stream_seek_seconds(void *context, int32_t seconds,
                                   uint64_t played_bytes) {
    wav_stream *w = (wav_stream *)context;
    uint64_t base_ms = ((uint64_t)w->play_base_frame * 1000u) / w->source_rate;
    uint64_t played_ms = (played_bytes * 1000u) / 192000u;
    int64_t target = (int64_t)(base_ms + played_ms) + (int64_t)seconds * 1000;
    uint64_t total_ms = ((uint64_t)w->frames_total * 1000u) / w->source_rate;
    if (target < 0) target = 0;
    if ((uint64_t)target >= total_ms) target = total_ms > 1u ? (int64_t)(total_ms - 1u) : 0;
    uint32_t frame = (uint32_t)(((uint64_t)target * w->source_rate) / 1000u);
    if (frame >= w->frames_total) frame = w->frames_total - 1u;
    uint64_t byte_off = (uint64_t)w->data_offset + (uint64_t)frame * w->block_align;
    if (byte_off > 0xFFFFFFFFu || !fat_seek(&w->file, (uint32_t)byte_off)) return 0;
    w->frame_index = frame;
    w->play_base_frame = frame;
    w->phase = 0;
    w->primed = 0;
    w->cache_pos = w->cache_len = 0;
    return 1;
}

static void append_time(char *line, uint32_t seconds) {
    num(seconds / 60u, line); kstrcat(line, ":"); num2(seconds % 60u, line);
}

static void wav_stream_status(void *context, uint64_t played_bytes, int paused,
                              uint8_t volume, int repeat, int shuffle) {
    wav_stream *w = (wav_stream *)context;
    uint64_t base_ms = ((uint64_t)w->play_base_frame * 1000u) / w->source_rate;
    uint64_t now_ms = base_ms + (played_bytes * 1000u) / 192000u;
    uint64_t total_ms = ((uint64_t)w->frames_total * 1000u) / w->source_rate;
    if (now_ms > total_ms) now_ms = total_ms;
    uint32_t cur = (uint32_t)(now_ms / 1000u), total = (uint32_t)(total_ms / 1000u);
    uint32_t filled = total ? (cur * 32u) / total : 0u;
    if (filled > 32u) filled = 32u;

    char line[180]; line[0] = 0;
    kstrcat(line, paused ? "PAUSED  " : "PLAYING "); kstrcat(line, w->track_name);
    kstrcat(line, "  [");
    for (uint32_t i = 0; i < 32u; i++) kstrcat(line, i < filled ? "#" : ".");
    kstrcat(line, "] "); append_time(line, cur); kstrcat(line, " / "); append_time(line, total);
    pad_to(line, 155);
    console_draw_at(w->status_row, 0, line, paused ? COL_WARN : COL_LS_WAV);

    line[0] = 0; kstrcat(line, "Vol "); num(volume, line); kstrcat(line, "%  ");
    kstrcat(line, repeat ? "Repeat ON  " : "Repeat off  ");
    kstrcat(line, shuffle ? "Shuffle ON  " : "Shuffle off  ");
    kstrcat(line, "Space pause | Left/Right -/+10s | Up/Down volume | R repeat | S shuffle | Esc stop");
    pad_to(line, 155);
    console_draw_at(w->status_row + 1, 0, line, COL_TEXT);
}

static int collect_wavs(void) {
    wav_count = 0;
    uint32_t count = fat_dir_count();
    for (uint32_t i = 0; i < count && wav_count < 256; i++) {
        fat_dirent e;
        if (!fat_dir_get(i, &e) || e.is_dir || !has_ext_ci(e.name, ".wav")) continue;
        int k = 0; for (; e.name[k] && k < 127; k++) wav_names[wav_count][k] = e.name[k];
        wav_names[wav_count][k] = 0; wav_count++;
    }
    return wav_count;
}

static void cmd_play(const char *arg) {
    g_bg_queue_active = 0;
    if (hda_bg_is_active()) hda_bg_stop();
    if (!fat_mounted()) { console_puts("play: no FAT storage mounted.\n"); return; }
    int count = collect_wavs();
    if (!count) { console_puts("play: no .wav files in current directory.\n"); return; }

    int current = 0;
    int all = !arg || !*arg || cieq(arg, "*.wav");
    if (!all) {
        int found = -1;
        for (int i = 0; i < count; i++) if (cieq(wav_names[i], arg)) { found = i; break; }
        if (found < 0) { console_puts("play: WAV not found: "); console_puts(arg); console_puts("\n"); return; }
        current = found;
    }

    int status_row = console_reserve_rows(2);
    for (;;) {
        if (!wav_stream_open(wav_names[current], &g_wav_stream)) {
            console_puts("play: unsupported or damaged PCM WAV: ");
            console_puts(wav_names[current]); console_puts("\n");
            if (++current >= count) break;
            continue;
        }
        g_wav_stream.status_row = status_row;
        int k = 0; for (; wav_names[current][k] && k < 127; k++)
            g_wav_stream.track_name[k] = wav_names[current][k];
        g_wav_stream.track_name[k] = 0;
        wav_stream_status(&g_wav_stream, 0, 0, 70, 0, 0);

        hda_info_t hi;
        int result = hda_play_stream_48k(wav_stream_fill, wav_stream_seek_seconds,
                                         wav_stream_status, &g_wav_stream, &hi);
        if (!result) {
            console_set_colors(COL_WARN, COL_BG);
            console_puts("play: "); console_puts(hda_last_error()); console_puts("\n");
            console_set_colors(COL_TEXT, COL_BG); break;
        }
        if (result == 2) { console_puts("Playback stopped.\n"); break; }
        if (hi.repeat_enabled) continue;
        if (hi.shuffle_enabled && count > 1) {
            int next = (int)(rdtsc() % (uint32_t)count);
            if (next == current) next = (next + 1) % count;
            current = next;
        } else {
            current++;
            if (current >= count) break;
        }
    }
    console_puts("Playlist finished.\n");
}

static int collect_bg_wavs(void) {
    g_bg_count = 0;
    uint32_t count = fat_dir_count();
    for (uint32_t i = 0; i < count && g_bg_count < 256; i++) {
        fat_dirent e;
        if (!fat_dir_get(i, &e) || e.is_dir || !has_ext_ci(e.name, ".wav")) continue;
        g_bg_wavs[g_bg_count++] = e;
    }
    return g_bg_count;
}

static void bg_track_finished(void *context);

static int bg_start_current(void) {
    if (!g_bg_queue_active || g_bg_count <= 0 || g_bg_index < 0 ||
        g_bg_index >= g_bg_count) return 0;
    fat_dirent *entry = &g_bg_wavs[g_bg_index];
    if (!wav_stream_open_entry(entry, &g_wav_stream)) return 0;
    int k = 0; for (; entry->name[k] && k < 127; k++) g_bg_track[k] = entry->name[k];
    g_bg_track[k] = 0;
    g_bg_total_seconds = g_wav_stream.source_rate
        ? g_wav_stream.frames_total / g_wav_stream.source_rate : 0u;
    hda_info_t info;
    return hda_bg_start_48k(wav_stream_fill, wav_stream_seek_seconds,
                            bg_track_finished, &g_wav_stream, &info);
}

static void bg_track_finished(void *context) {
    (void)context;
    if (!g_bg_queue_active || g_bg_count <= 0) return;
    if (!g_bg_repeat) {
        if (g_bg_shuffle && g_bg_count > 1) {
            int next = (int)(rdtsc() % (uint32_t)g_bg_count);
            if (next == g_bg_index) next = (next + 1) % g_bg_count;
            g_bg_index = next;
        } else {
            g_bg_index++;
            if (g_bg_index >= g_bg_count) { g_bg_queue_active = 0; return; }
        }
    }

    for (int tries = 0; tries < g_bg_count; tries++) {
        if (bg_start_current()) return;
        g_bg_index = (g_bg_index + 1) % g_bg_count;
    }
    g_bg_queue_active = 0;
}

static void cmd_bgplay(const char *arg) {
    if (!arg || !*arg) { console_puts("Usage: bgplay <file.wav|*.wav>\n"); return; }
    if (!fat_mounted()) { console_puts("bgplay: no FAT storage mounted.\n"); return; }
    hda_bg_stop(); g_bg_queue_active = 0;
    if (!collect_bg_wavs()) {
        console_puts("bgplay: no WAV files in current directory.\n"); return;
    }
    if (cieq(arg, "*.wav")) {
        g_bg_index = 0;
    } else {
        g_bg_index = -1;
        for (int i = 0; i < g_bg_count; i++)
            if (cieq(g_bg_wavs[i].name, arg)) { g_bg_index = i; break; }
        if (g_bg_index < 0) {
            console_puts("bgplay: WAV not found: "); console_puts(arg); console_puts("\n");
            return;
        }

        fat_dirent selected = g_bg_wavs[g_bg_index];
        g_bg_wavs[0] = selected; g_bg_count = 1; g_bg_index = 0;
    }
    g_bg_queue_active = 1;
    if (!bg_start_current()) {
        g_bg_queue_active = 0;
        console_set_colors(COL_WARN, COL_BG);
        console_puts("bgplay: "); console_puts(hda_last_error()); console_puts("\n");
        console_set_colors(COL_TEXT, COL_BG); return;
    }
    console_puts("Background queue started: ");
    char line[48]; line[0] = 0; num((uint32_t)g_bg_count, line);
    kstrcat(line, g_bg_count == 1 ? " track\n" : " tracks\n"); console_puts(line);
    console_puts("Music will keep playing in shell and edit.\n");
}

static void cmd_bgstatus(void) {
    if (!hda_bg_is_active()) {
        console_puts(g_bg_queue_active ? "Background queue is switching tracks.\n"
                                       : "Background music is not playing.\n");
        return;
    }
    uint32_t seconds = (uint32_t)(hda_bg_played_bytes() / 192000u);
    char line[224]; line[0] = 0;
    kstrcat(line, hda_bg_is_paused() ? "Background PAUSED: " : "Background PLAYING: ");
    kstrcat(line, g_bg_track); kstrcat(line, "  ");
    append_time(line, seconds); kstrcat(line, " / "); append_time(line, g_bg_total_seconds);
    kstrcat(line, "  volume="); num(hda_bg_get_volume(), line); kstrcat(line, "%");
    kstrcat(line, g_bg_repeat ? "  repeat=ON" : "  repeat=off");
    kstrcat(line, g_bg_shuffle ? "  shuffle=ON" : "  shuffle=off");
    kstrcat(line, "  queue="); num((uint32_t)(g_bg_index + 1), line);
    kstrcat(line, "/"); num((uint32_t)g_bg_count, line); kstrcat(line, "\n");
    console_puts(line);
}

static void cmd_bgvolume(const char *arg) {
    int ok = 0; uint32_t value = parse_uint(arg, &ok);
    if (!ok || value > 100u) { console_puts("Usage: bgvolume <0-100>\n"); return; }
    hda_bg_set_volume((uint8_t)value);
    console_puts("Background volume: ");
    char line[16]; line[0] = 0; num(value, line); kstrcat(line, "%\n"); console_puts(line);
}

static void cmd_bgswitch(int direction) {
    if (!g_bg_queue_active || g_bg_count <= 0) {
        console_puts("Background queue is not active. Start it with 'bgplay *.wav'.\n");
        return;
    }
    hda_bg_stop();
    if (direction > 0 && g_bg_shuffle && g_bg_count > 1) {
        int next = (int)(rdtsc() % (uint32_t)g_bg_count);
        if (next == g_bg_index) next = (next + 1) % g_bg_count;
        g_bg_index = next;
    } else {
        g_bg_index += direction;
        if (g_bg_index >= g_bg_count) g_bg_index = 0;
        if (g_bg_index < 0) g_bg_index = g_bg_count - 1;
    }
    for (int tries = 0; tries < g_bg_count; tries++) {
        if (bg_start_current()) {
            console_puts(direction > 0 ? "Next background track: "
                                       : "Previous background track: ");
            console_puts(g_bg_track); console_puts("\n");
            return;
        }
        g_bg_index += direction;
        if (g_bg_index >= g_bg_count) g_bg_index = 0;
        if (g_bg_index < 0) g_bg_index = g_bg_count - 1;
    }
    g_bg_queue_active = 0;
    console_puts("No playable WAV left in background queue.\n");
}

static int mode_value(const char *arg, int current, int *ok) {
    *ok = 1;
    if (!arg || !*arg) return !current;
    if (cieq(arg, "on") || cieq(arg, "1")) return 1;
    if (cieq(arg, "off") || cieq(arg, "0")) return 0;
    *ok = 0; return current;
}

static void cmd_bgrepeat(const char *arg) {
    int ok; g_bg_repeat = mode_value(arg, g_bg_repeat, &ok);
    if (!ok) { console_puts("Usage: bgrepeat [on|off]\n"); return; }
    console_puts(g_bg_repeat ? "Background repeat: ON\n" : "Background repeat: off\n");
}

static void cmd_bgshuffle(const char *arg) {
    int ok; g_bg_shuffle = mode_value(arg, g_bg_shuffle, &ok);
    if (!ok) { console_puts("Usage: bgshuffle [on|off]\n"); return; }
    console_puts(g_bg_shuffle ? "Background shuffle: ON\n" : "Background shuffle: off\n");
}

static void cmd_audioout(const char *arg) {
    if (arg && *arg) {
        int mode = -1;
        if (cieq(arg, "auto")) mode = 0;
        else if (cieq(arg, "speaker") || cieq(arg, "speakers")) mode = 1;
        else if (cieq(arg, "headphones") || cieq(arg, "headphone") || cieq(arg, "hp")) mode = 2;
        if (mode < 0) {
            console_puts("Usage: audioout [auto|speaker|headphones]\n"); return;
        }
        hda_set_output_mode(mode);

        if (hda_bg_is_active() && g_bg_queue_active) {
            hda_bg_stop();
            if (!bg_start_current())
                console_puts("audioout: failed to restart current background track.\n");
        }
    }
    int mode = hda_get_output_mode();
    console_puts("Audio output mode: ");
    console_puts(mode == 0 ? "auto" : (mode == 1 ? "speaker" : "headphones"));
    console_puts("  selected: ");
    console_puts(hda_output_is_headphones() ? "headphones" : "speaker/line-out");
    console_puts("  jack: ");
    console_puts(hda_headphones_present() ? "plugged" : "not detected");
    console_puts("  pin=");
    char line[16]; line[0] = 0; num(hda_output_pin(), line); kstrcat(line, "\n");
    console_puts(line);
}

static int collect_images(void) {
    img_count = 0;
    if (!fat_mounted()) return 0;
    uint32_t count = fat_dir_count();
    for (uint32_t i = 0; i < count && img_count < 256; i++) {
        fat_dirent e;
        if (!fat_dir_get(i, &e) || e.is_dir) continue;
        image_t probe;
        uint32_t got = fat_read_file(e.name, file_buf, 4096);
        if (got && image_probe(file_buf, got, &probe)) {
            int k = 0;
            for (; e.name[k] && k < 127; k++) img_names[img_count][k] = e.name[k];
            img_names[img_count][k] = 0;
            img_count++;
        }
    }
    return img_count;
}

static int load_image_named(const char *name, image_t *img) {
    uint32_t got = fat_read_file(name, file_buf, sizeof file_buf);
    if (!got) return 0;
    return image_decode(file_buf, got, pix_buf, sizeof pix_buf, img);
}

static int fit_zoom(const fb_info *fb, const image_t *img) {
    int lh     = FB_GLYPH_H + 2;
    int area_w = (int)fb->width;
    int area_h = (int)fb->height - 2 * (2 * lh + 12);
    if (area_w < 1) area_w = 1;
    if (area_h < 1) area_h = 1;
    if (img->width == 0 || img->height == 0) return 100;
    int zw = area_w * 100 / (int)img->width;
    int zh = area_h * 100 / (int)img->height;
    int z  = zw < zh ? zw : zh;
    if (z > 100) z = 100;
    if (z < 5)   z = 5;
    return z;
}

static void view_gallery(int start) {
    int count = img_count;
    if (count <= 0) {
        render_demo(FB);
        kbd_getchar();
        console_init(FB);
        console_set_colors(COL_TEXT, COL_BG);
        return;
    }

    int idx = (start < 0 || start >= count) ? 0 : start;
    image_t img;
    int have  = load_image_named(img_names[idx], &img);
    int zoom  = have ? fit_zoom(FB, &img) : 100;
    int pan_x = 0, pan_y = 0;
    int clean_view = 1;
    int fill_screen = 0;

    for (;;) {
        if (have) {
            char title[128];
            title[0] = 0;
            kstrcat(title, "fukOS viewer  -  ");
            kstrcat(title, img_names[idx]);
            kstrcat(title, "  (");
            num(idx + 1, title); kstrcat(title, "/");
            num((uint32_t)count, title); kstrcat(title, ")");

            char cap[128];
            cap[0] = 0;
            kstrcat(cap, "Loaded '");
            kstrcat(cap, img_names[idx]);
            kstrcat(cap, "'.");
            if (clean_view)
                render_photo_fullscreen(FB, &img, fill_screen, pan_x, pan_y);
            else
                render_view_zoom(FB, &img, title, cap, zoom, pan_x, pan_y);
        } else {
            render_message(FB, "fukOS viewer  -  decode error",
                           "Could not decode this file as an image.");
        }

        char c = kbd_getchar();
        if (c == KEY_F12) { capture_screenshot_auto(FB); continue; }
        if (c == 'q' || c == 'Q' || c == '\n' || c == 27) break;

        if (c == 'i' || c == 'I') {
            clean_view = !clean_view;
        } else if (c == 'm' || c == 'M') {
            fill_screen = !fill_screen;
            pan_x = pan_y = 0;
        } else if (c == 'n' || c == 'N' || c == ' ' || c == 'p' || c == 'P') {
            if (c == 'p' || c == 'P') idx = (idx + count - 1) % count;
            else                      idx = (idx + 1) % count;
            have  = load_image_named(img_names[idx], &img);
            zoom  = have ? fit_zoom(FB, &img) : 100;
            pan_x = pan_y = 0;
        } else if (!clean_view && have && (c == '+' || c == '=')) {
            int step = zoom / 4; if (step < 1) step = 1;
            zoom += step; if (zoom > 800) zoom = 800;
        } else if (!clean_view && have && (c == '-' || c == '_')) {
            int step = zoom / 5; if (step < 1) step = 1;
            zoom -= step; if (zoom < 5) zoom = 5;
        } else if (!clean_view && have && (c == 'f' || c == 'F' || c == '0')) {
            zoom = fit_zoom(FB, &img); pan_x = pan_y = 0;
        } else if (have && (c == 'd' || c == 'D' || c == 'l')) {
            pan_x -= 48;
        } else if (have && (c == 'a' || c == 'A' || c == 'h')) {
            pan_x += 48;
        } else if (have && (c == 's' || c == 'S' || c == 'j')) {
            pan_y -= 48;
        } else if (have && (c == 'w' || c == 'W' || c == 'k')) {
            pan_y += 48;
        }
    }

    console_init(FB);
    console_set_colors(COL_TEXT, COL_BG);
}

static void cmd_img(const char *arg) {
    int start = 0;
    collect_images();
    if (img_count == 0) { view_gallery(-1); return; }

    if (arg && *arg) {
        char target[128]; target[0] = 0;
        int ok; uint32_t n = parse_uint(arg, &ok);
        if (ok) {
            fat_dirent d;
            if (fat_dir_get(n, &d) && !d.is_dir) {
                int k = 0; for (; d.name[k] && k < 127; k++) target[k] = d.name[k];
                target[k] = 0;
            }
        }
        if (!target[0]) {
            int k = 0; for (; arg[k] && k < 127; k++) target[k] = arg[k];
            target[k] = 0;
        }
        int found = -1;
        for (int i = 0; i < img_count; i++)
            if (cieq(img_names[i], target)) { found = i; break; }
        if (found < 0) {
            console_puts("img: no such image: ");
            console_puts(arg);
            console_puts("\n");
            return;
        }
        start = found;
    }
    view_gallery(start);
}

static int two_tokens(const char *arg, char *a, char *b, int cap) {
    int i = 0; while (arg[i] == ' ') i++;
    int n = 0; while (arg[i] && arg[i] != ' ' && n < cap - 1) a[n++] = arg[i++]; a[n] = 0;
    while (arg[i] == ' ') i++;
    n = 0; while (arg[i] && arg[i] != ' ' && n < cap - 1) b[n++] = arg[i++]; b[n] = 0;
    return (a[0] && b[0]);
}

static void cmd_pwd(void) {
    char path[256];
    fat_cwd_path(path, sizeof path);
    console_puts(path);
    console_puts("\n");
}

static void cmd_tree(void) {
    if (!fat_mounted()) { console_puts("tree: no storage mounted.\n"); return; }
    char path[256];
    fat_cwd_path(path, sizeof path);
    console_puts(path);
    console_puts("\n");
    uint32_t count = fat_dir_count();
    for (uint32_t i = 0; i < count; i++) {
        fat_dirent entry;
        if (!fat_dir_get(i, &entry)) continue;
        console_puts(i + 1 == count ? "`-- " : "|-- ");
        console_puts(entry.name);
        if (entry.is_dir) console_puts("/");
        console_puts("\n");
    }
}

static void cmd_head(const char *arg) {
    if (!arg || !*arg) { console_puts("Usage: head <file>\n"); return; }
    uint32_t got = fat_read_file(arg, file_buf, sizeof file_buf);
    if (!got) { console_puts("head: cannot read file.\n"); return; }
    uint32_t lines = 0;
    for (uint32_t i = 0; i < got && lines < 10; i++) {
        console_putc((char)file_buf[i]);
        if (file_buf[i] == '\n') lines++;
    }
    if (got && file_buf[got - 1] != '\n') console_putc('\n');
}

static void cmd_wc(const char *arg) {
    if (!arg || !*arg) { console_puts("Usage: wc <file>\n"); return; }
    fat_file file;
    if (!fat_open(arg, &file)) { console_puts("wc: cannot open file.\n"); return; }
    uint32_t bytes = 0, lines = 0, words = 0;
    int in_word = 0;
    for (;;) {
        uint32_t got = fat_read(&file, file_buf, 4096);
        if (!got) break;
        bytes += got;
        for (uint32_t i = 0; i < got; i++) {
            uint8_t c = file_buf[i];
            if (c == '\n') lines++;
            int space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            if (!space && !in_word) words++;
            in_word = !space;
        }
    }
    char out[96]; out[0] = 0;
    num(lines, out); kstrcat(out, " lines  ");
    num(words, out); kstrcat(out, " words  ");
    num(bytes, out); kstrcat(out, " bytes  ");
    kstrcat(out, arg); kstrcat(out, "\n");
    console_puts(out);
}

static char hex_digit(uint8_t value) {
    return value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
}

static void cmd_hexdump(const char *arg) {
    if (!arg || !*arg) { console_puts("Usage: hexdump <file>\n"); return; }
    uint32_t got = fat_read_file(arg, file_buf, 256);
    if (!got) { console_puts("hexdump: cannot read file.\n"); return; }
    for (uint32_t off = 0; off < got; off += 16) {
        char line[96]; int p = 0;
        for (int shift = 12; shift >= 0; shift -= 4)
            line[p++] = hex_digit((uint8_t)((off >> shift) & 0xF));
        line[p++] = ':'; line[p++] = ' ';
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < got) {
                uint8_t b = file_buf[off + i];
                line[p++] = hex_digit(b >> 4); line[p++] = hex_digit(b & 0xF);
            } else { line[p++] = ' '; line[p++] = ' '; }
            line[p++] = ' ';
        }
        line[p++] = ' ';
        for (uint32_t i = 0; i < 16 && off + i < got; i++) {
            uint8_t b = file_buf[off + i];
            line[p++] = (b >= 32 && b < 127) ? (char)b : '.';
        }
        line[p++] = '\n'; line[p] = 0;
        console_puts(line);
    }
}

static void cmd_mv(const char *arg) {
    char source[128], destination[128];
    if (!two_tokens(arg, source, destination, sizeof source)) {
        console_puts("Usage: mv <source-path> <destination-path>\n");
        return;
    }
    if (fat_move_file(source, destination)) {
        console_puts("Moved "); console_puts(source);
        console_puts(" -> "); console_puts(destination); console_puts("\n");
    } else {
        console_puts("mv: move failed; destination must include a file name.\n");
    }
}

static void put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

/* Encodes the current framebuffer into file_buf as a 24-bit BMP.
   Returns the total byte count, or 0 if the display is too large to fit. */
static uint32_t encode_screenshot_bmp(const fb_info *source) {
    if (!source || !source->addr) return 0;
    uint32_t stride = (source->width * 3u + 3u) & ~3u;
    uint64_t total64 = 54ull + (uint64_t)stride * source->height;
    if (total64 > sizeof file_buf) return 0;
    uint32_t total = (uint32_t)total64;
    memset(file_buf, 0, total);
    file_buf[0] = 'B'; file_buf[1] = 'M';
    put_u32(file_buf + 2, total);
    put_u32(file_buf + 10, 54);
    put_u32(file_buf + 14, 40);
    put_u32(file_buf + 18, source->width);
    put_u32(file_buf + 22, source->height);
    put_u16(file_buf + 26, 1);
    put_u16(file_buf + 28, 24);
    put_u32(file_buf + 34, stride * source->height);

    uint32_t bytes_per_pixel = source->bpp / 8u;
    if (bytes_per_pixel < 2u || bytes_per_pixel > 4u) return 0;
    for (uint32_t out_y = 0; out_y < source->height; out_y++) {
        uint32_t src_y = source->height - 1u - out_y;
        const uint8_t *src = source->addr + src_y * source->pitch;
        uint8_t *dst = file_buf + 54u + out_y * stride;
        for (uint32_t x = 0; x < source->width; x++) {
            const uint8_t *pixel = src + x * bytes_per_pixel;
            if (source->bpp == 32 || source->bpp == 24) {
                dst[x * 3u + 0] = pixel[0];
                dst[x * 3u + 1] = pixel[1];
                dst[x * 3u + 2] = pixel[2];
            } else if (source->bpp == 16) {
                uint16_t c = (uint16_t)(pixel[0] | (pixel[1] << 8));
                dst[x * 3u + 0] = (uint8_t)((c & 0x1F) * 255u / 31u);
                dst[x * 3u + 1] = (uint8_t)(((c >> 5) & 0x3F) * 255u / 63u);
                dst[x * 3u + 2] = (uint8_t)(((c >> 11) & 0x1F) * 255u / 31u);
            }
        }
    }
    return total;
}

static void cmd_screenshot(const char *arg) {
    if (!fat_mounted()) { console_puts("screenshot: no storage mounted.\n"); return; }
    char auto_name[32];
    const char *name = arg && *arg ? arg : 0;
    if (!name) {
        for (uint32_t i = 1; i <= 9999u; i++) {
            auto_name[0] = 0;
            kstrcat(auto_name, "shot"); num(i, auto_name); kstrcat(auto_name, ".bmp");
            if (!fat_exists(auto_name)) { name = auto_name; break; }
        }
        if (!name) { console_puts("screenshot: no free shot number.\n"); return; }
    } else if (fat_exists(name)) {
        console_puts("screenshot: refusing to overwrite existing file: ");
        console_puts(name); console_puts("\n");
        return;
    }
    /* The physical framebuffer is WC/MMIO memory. Pixel-by-pixel reads from
       it are extremely slow on Apollo Lake. The console already keeps the
       exact visible image in its RAM shadow specifically to avoid reads from
       video memory, so encode the shell screenshot from that shadow. */
    uint32_t total = encode_screenshot_bmp(console_snapshot_framebuffer());
    if (!total) { console_puts("screenshot: display is too large.\n"); return; }
    {
        char progress[64]; progress[0] = 0;
        kstrcat(progress, "screenshot: writing ");
        num(total, progress);
        kstrcat(progress, " bytes...\n");
        console_puts(progress);
    }
    /* Avoid a complete FAT scan before every screenshot. Allocation below
       reports disk-full and rolls back a partial chain itself. */
    if (fat_write_file(name, file_buf, total)) {
        console_puts("Screenshot saved to "); console_puts(name); console_puts("\n");
    } else {
        uint8_t sk = 0xFFu, asc = 0xFFu, ascq = 0xFFu;
        uint8_t stage = 0, raw_cc = 0;
        uint8_t reset_ep_cc = 0xFFu, reset_deq_cc = 0xFFu;
        xhci_msc_last_sense(&sk, &asc, &ascq);
        xhci_msc_last_stage(&stage, &raw_cc);
        xhci_msc_last_reset_result(&reset_ep_cc, &reset_deq_cc);
        char line[400];
        line[0] = 0;
        kstrcat(line, "screenshot: write failed (storage I/O error");
        if (sk != 0xFFu) {
            kstrcat(line, ", SCSI sense ");
            hex_byte(sk, line);
            kstrcat(line, "/");
            hex_byte(asc, line);
            kstrcat(line, "/");
            hex_byte(ascq, line);
            if (sk == 0x07u) kstrcat(line, " - drive is write-protected");
            else if (sk == 0x02u) kstrcat(line, " - drive not ready");
            else if (sk == 0x03u) kstrcat(line, " - medium error");
        } else {
            kstrcat(line, ", stage ");
            num(stage, line);
            kstrcat(line, " cc ");
            hex_byte(raw_cc, line);
            kstrcat(line, " (sense unavailable)");
        }
        kstrcat(line, ", reset ep_cc ");
        hex_byte(reset_ep_cc, line);
        kstrcat(line, " deq_cc ");
        hex_byte(reset_deq_cc, line);
        uint32_t usbsts = xhci_msc_last_usbsts();
        kstrcat(line, " usbsts ");
        hex4(usbsts & 0xFFFFu, line);
        if (usbsts & 1u) kstrcat(line, " [HCH:controller halted]");
        if (usbsts & 4u) kstrcat(line, " [HSE:host system error]");
        uint32_t portsc = xhci_msc_last_portsc();
        if (portsc != 0xFFFFFFFFu) {
            kstrcat(line, " portsc ");
            hex4((portsc >> 16) & 0xFFFFu, line);
            hex4(portsc & 0xFFFFu, line);
            if (!(portsc & 1u)) kstrcat(line, " [CCS:disconnected]");
            if (portsc & (1u << 17)) kstrcat(line, " [CSC:connect-change]");
            if (portsc & (1u << 18)) kstrcat(line, " [PEC:disabled-change]");
            if (portsc & (1u << 19)) kstrcat(line, " [WRC:warm-reset-change]");
            if (portsc & (1u << 20)) kstrcat(line, " [OCC:overcurrent-change]");
            if (portsc & (1u << 21)) kstrcat(line, " [PRC:reset-change]");
            if (portsc & (1u << 22)) kstrcat(line, " [PLC:link-change]");
            if (portsc & (1u << 23)) kstrcat(line, " [CEC:config-error-change]");
        }
        uint8_t slot_state = 0xFFu, ep_out_state = 0xFFu, ep_in_state = 0xFFu;
        xhci_msc_last_ep_diag(&slot_state, &ep_out_state, &ep_in_state);
        kstrcat(line, " slot");
        hex_byte(slot_state, line);
        kstrcat(line, " epout");
        hex_byte(ep_out_state, line);
        kstrcat(line, " epin");
        hex_byte(ep_in_state, line);
        kstrcat(line, ").\n");
        console_puts(line);
    }
}

/* F12 global hotkey: silent capture with an auto-numbered name (shot1.bmp,
   shot2.bmp, ...), used from the shell prompt, editor, gallery, and pager
   so it never disturbs their own screen/redraw state. */
static void capture_screenshot_auto(const fb_info *source) {
    if (!fat_mounted()) return;
    if (!source) source = console_snapshot_framebuffer();
    uint32_t total = encode_screenshot_bmp(source);
    if (!total) return;
    char name[32];
    for (uint32_t i = 1; i <= 9999; i++) {
        name[0] = 0;
        kstrcat(name, "shot"); num(i, name); kstrcat(name, ".bmp");
        if (!fat_exists(name)) { fat_write_file(name, file_buf, total); return; }
    }
}

static int is_text_file(const char *name) {
    return has_ext_ci(name, ".txt") || has_ext_ci(name, ".md") ||
           has_ext_ci(name, ".c") || has_ext_ci(name, ".h") ||
           has_ext_ci(name, ".asm") || has_ext_ci(name, ".s") ||
           has_ext_ci(name, ".conf") || has_ext_ci(name, ".cfg") ||
           has_ext_ci(name, ".ini") || has_ext_ci(name, ".log");
}

static void cmd_open(const char *arg) {
    if (!arg || !*arg) { console_puts("Usage: open <file>\n"); return; }
    if (!fat_mounted()) { console_puts("open: no storage mounted.\n"); return; }
    if (!fat_exists(arg)) { console_puts("open: not found: "); console_puts(arg); console_puts("\n"); return; }
    if (is_text_file(arg)) { cmd_edit(arg); return; }
    if (has_ext_ci(arg, ".bmp") || has_ext_ci(arg, ".img")) { cmd_img(arg); return; }
    if (has_ext_ci(arg, ".wav")) { cmd_play(arg); return; }
    if (has_ext_ci(arg, ".dsg")) {
        uint32_t got = fat_read_file(arg, file_buf, 64);
        console_puts("DOOM save file: "); console_puts(arg); console_puts("\n");
        if (got) { console_puts("Description: "); for (uint32_t i = 0; i < got && i < 24 && file_buf[i]; i++) console_putc((char)file_buf[i]); console_puts("\n"); }
        return;
    }
    cmd_cat(arg);
}

static void cmd_matrix(void) {
    if (!FB) return;
    fb_clear(FB, 0x000000);
    int cols = (int)(FB->width / FB_GLYPH_W);
    int rows = (int)(FB->height / FB_GLYPH_H);
    if (cols < 1 || rows < 1) return;
    uint16_t drops[192];
    if (cols > 192) cols = 192;
    uint32_t seed = (uint32_t)rdtsc();
    for (int x = 0; x < cols; x++) { seed = seed * 1664525u + 1013904223u; drops[x] = (uint16_t)(seed % (uint32_t)rows); }
    for (;;) {
        xhci_idle_drain(); hda_bg_poll();
        char c = kbd_poll();
        if (c == 'q' || c == 'Q' || c == 27 || c == '\n') break;
        for (int x = 0; x < cols; x++) {
            seed = seed * 1664525u + 1013904223u;
            int y = drops[x] % rows;
            char ch = (char)('0' + ((seed >> 16) % 10));
            fb_fill_rect(FB, x * FB_GLYPH_W, y * FB_GLYPH_H, FB_GLYPH_W, FB_GLYPH_H, 0x000000);
            fb_draw_char(FB, x * FB_GLYPH_W, y * FB_GLYPH_H, ch, 0x7FFF7F);
            int tail = y - 8; if (tail < 0) tail += rows;
            fb_fill_rect(FB, x * FB_GLYPH_W, tail * FB_GLYPH_H, FB_GLYPH_W, FB_GLYPH_H, 0x000000);
            drops[x] = (uint16_t)((y + 1) % rows);
        }
       for (volatile int spin = 0; spin < 2400000; spin++) { }
    }
    console_init(FB); console_set_colors(COL_TEXT, COL_BG);
}

static void cmd_touch(const char *arg) {
    if (!fat_mounted()) { console_puts("touch: no storage mounted.\n"); return; }
    if (!arg || !*arg) { console_puts("Usage: touch <file>\n"); return; }
    if (fat_touch(arg)) { console_puts("Created "); console_puts(arg); console_puts("\n"); }
    else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("touch: failed (disk full or read-only?)\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_mkdir(const char *arg) {
    if (!fat_mounted()) { console_puts("mkdir: no storage mounted.\n"); return; }
    if (!arg || !*arg) { console_puts("Usage: mkdir <name>\n"); return; }
    if (fat_mkdir(arg)) { console_puts("Created folder "); console_puts(arg); console_puts("\n"); }
    else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("mkdir: failed (name exists, disk full or read-only?)\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_rmdir(const char *arg) {
    if (!fat_mounted()) { console_puts("rmdir: no storage mounted.\n"); return; }
    if (!arg || !*arg) { console_puts("Usage: rmdir <folder>\n"); return; }
    if (fat_rmdir(arg)) { console_puts("Removed folder "); console_puts(arg); console_puts("\n"); }
    else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("rmdir: failed (not empty, not a folder, or not found): "); console_puts(arg); console_puts("\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_rm(const char *arg) {
    if (!fat_mounted()) { console_puts("rm: no storage mounted.\n"); return; }
    if (!arg || !*arg) { console_puts("Usage: rm <file>\n"); return; }
    if (fat_delete_file(arg)) { console_puts("Deleted "); console_puts(arg); console_puts("\n"); }
    else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("rm: no such file (or it is a folder): "); console_puts(arg); console_puts("\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_cp(const char *arg) {
    if (!fat_mounted()) { console_puts("cp: no storage mounted.\n"); return; }
    char src[128], dst[128];
    if (!two_tokens(arg, src, dst, 128)) { console_puts("Usage: cp <source> <dest>\n"); return; }
    uint32_t got = fat_read_file(src, file_buf, sizeof file_buf);
    if (got == 0) {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("cp: cannot read source (missing or empty): "); console_puts(src); console_puts("\n");
        console_set_colors(COL_TEXT, COL_BG);
        return;
    }
    if (fat_write_file(dst, file_buf, got)) {
        console_puts("Copied "); console_puts(src); console_puts(" -> "); console_puts(dst); console_puts("\n");
    } else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("cp: write failed (disk full or read-only?)\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_echo(char *arg) {
    if (!arg) arg = (char *)"";
    char *redir = 0; int append = 0;
    for (char *p = arg; *p; p++) {
        if (p[0] == '>') { redir = p; append = (p[1] == '>'); break; }
    }
    if (!redir) { console_puts(arg); console_puts("\n"); return; }
    if (!fat_mounted()) { console_puts("echo: no storage mounted.\n"); return; }

    char *fname = redir + (append ? 2 : 1);
    *redir = 0;
    while (*fname == ' ') fname++;
    int tlen = 0; while (arg[tlen]) tlen++;
    while (tlen > 0 && arg[tlen - 1] == ' ') arg[--tlen] = 0;
    int fl = 0; while (fname[fl]) fl++;
    while (fl > 0 && fname[fl - 1] == ' ') fname[--fl] = 0;
    if (!*fname) { console_puts("Usage: echo <text> > <file>   (>> to append)\n"); return; }

    uint32_t base = 0;
    if (append) base = fat_read_file(fname, file_buf, sizeof file_buf - 256);
    uint32_t p = base;
    for (int i = 0; arg[i] && p < sizeof file_buf - 2; i++) file_buf[p++] = (uint8_t)arg[i];
    file_buf[p++] = '\n';
    if (fat_write_file(fname, file_buf, p)) {
        console_puts(append ? "Appended to " : "Wrote "); console_puts(fname); console_puts("\n");
    } else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("echo: write failed (disk full or read-only?)\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static const fb_info *g_dst;
static int  g_ed_cx, g_ed_cy;
static uint32_t g_ed_ccp;
static uint32_t g_ed_cli;
static uint32_t g_ed_ls;

static uint32_t ed_line_start(uint32_t off) {
    while (off > 0 && file_buf[off - 1] != '\n') off--;
    return off;
}
static uint32_t ed_line_end(uint32_t off, uint32_t n) {
    while (off < n && file_buf[off] != '\n') off++;
    return off;
}

static int ed_seq_len(uint8_t b) {
    if (b >= 0xF0) return 4;
    if (b >= 0xE0) return 3;
    if (b >= 0xC0) return 2;
    return 1;
}

static int ed_decode(uint32_t off, uint32_t n, uint32_t *cp) {
    uint8_t b = file_buf[off];
    int len = ed_seq_len(b);
    uint32_t v;
    if (len == 4)      v = (uint32_t)(b & 0x07);
    else if (len == 3) v = (uint32_t)(b & 0x0F);
    else if (len == 2) v = (uint32_t)(b & 0x1F);
    else               { *cp = b; return 1; }
    for (int j = 1; j < len; j++) {
        if (off + (uint32_t)j >= n) { *cp = b; return 1; }
        uint8_t cc = file_buf[off + (uint32_t)j];
        if ((cc & 0xC0) != 0x80) { *cp = b; return 1; }
        v = (v << 6) | (uint32_t)(cc & 0x3F);
    }
    *cp = v;
    return len;
}

static uint32_t ed_next_ch(uint32_t off, uint32_t n) {
    if (off >= n) return n;
    uint32_t nx = off + (uint32_t)ed_seq_len(file_buf[off]);
    return nx > n ? n : nx;
}
static uint32_t ed_prev_ch(uint32_t off) {
    if (off == 0) return 0;
    off--;
    while (off > 0 && (file_buf[off] & 0xC0) == 0x80) off--;
    return off;
}

static uint32_t ed_col_of(uint32_t ls, uint32_t cur) {
    uint32_t col = 0;
    for (uint32_t i = ls; i < cur; i++)
        if ((file_buf[i] & 0xC0) != 0x80) col++;
    return col;
}

static uint32_t ed_off_at_col(uint32_t ls, uint32_t le, uint32_t n, uint32_t col) {
    uint32_t off = ls, c = 0;
    while (off < le && c < col) { off = ed_next_ch(off, n); c++; }
    return off;
}

static void ed_present(int y0, int y1) {
    if (g_dst->addr == FB->addr) return;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)FB->height) y1 = (int)FB->height;
    if (y1 <= y0) return;

    uint32_t off = (uint32_t)y0 * FB->pitch;
    uint32_t len = (uint32_t)(y1 - y0) * FB->pitch;
    memcpy(FB->addr + off, g_dst->addr + off, len);
}

static void ed_present_rect(int x0, int y0, int x1, int y1) {
    if (g_dst->addr == FB->addr) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)FB->width)  x1 = (int)FB->width;
    if (y1 > (int)FB->height) y1 = (int)FB->height;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t bypp     = FB->bpp / 8;
    uint32_t xoff     = (uint32_t)x0 * bypp;
    uint32_t rowbytes = (uint32_t)(x1 - x0) * bypp;
    for (int y = y0; y < y1; y++) {
        uint32_t o = (uint32_t)y * FB->pitch + xoff;
        memcpy(FB->addr + o, g_dst->addr + o, rowbytes);
    }
}

static uint32_t ed_count_lines_before(uint32_t cur) {
    uint32_t cli = 0;
    for (uint32_t i = 0; i < cur; i++) if (file_buf[i] == '\n') cli++;
    return cli;
}

static int ed_layout(uint32_t n, uint32_t cur, uint32_t cli, uint32_t *p_top, uint32_t *p_coloff) {
    int cols = (int)(FB->width  / FB_GLYPH_W);
    int rows = (int)(FB->height / FB_GLYPH_H);
    int trows = rows - 2; if (trows < 1) trows = 1;

    uint32_t ls = ed_line_start(cur), ccol = ed_col_of(ls, cur);
    g_ed_ls = ls; g_ed_cli = cli;

    uint32_t top = *p_top, coloff = *p_coloff, otop = top, ocol = coloff;

    { const int CHUNK = 1;
      if (cli < top) {
          top = (cli > (uint32_t)CHUNK) ? cli - (uint32_t)CHUNK : 0;
      } else if (cli >= top + (uint32_t)trows) {
          uint32_t nt = cli + (uint32_t)CHUNK + 1;
          top = (nt > (uint32_t)trows) ? nt - (uint32_t)trows : 0;
      } }
    if (ccol < coloff) coloff = ccol;
    if (ccol >= coloff + (uint32_t)cols) coloff = ccol - (uint32_t)cols + 1;
    *p_top = top; *p_coloff = coloff;

    g_ed_cx  = 4 + (int)(ccol - coloff) * FB_GLYPH_W;
    g_ed_cy  = ((int)(cli - top) + 1) * FB_GLYPH_H + 2;
    if (cur < n && file_buf[cur] != '\n') {
        uint32_t cp; ed_decode(cur, n, &cp);
        g_ed_ccp = (cp == '\t') ? (uint32_t)' ' : cp;
    } else {
        g_ed_ccp = ' ';
    }
    return (top != otop || coloff != ocol);
}

static int ed_draw_status(uint32_t n, uint32_t cur, uint32_t cli) {

    int rows = (int)(FB->height / FB_GLYPH_H);
    uint32_t ls = ed_line_start(cur), ccol = ed_col_of(ls, cur);
    int sy = (rows - 1) * FB_GLYPH_H;
    char st[320]; st[0] = 0;
    kstrcat(st, " [BUILD v6] ESC save  Ctrl+Q quit  Ctrl+F find  Ctrl+Z undo  Ctrl+K del-line   Ln ");
    num(cli + 1, st); kstrcat(st, ", Col "); num(ccol + 1, st);
    kstrcat(st, "   "); num(n, st); kstrcat(st, " bytes");

    int w = 4 + (int)kstrlen(st) * FB_GLYPH_W + 2 * FB_GLYPH_W;
    if (w > (int)FB->width) w = (int)FB->width;
    fb_fill_rect(g_dst, 0, sy, w, FB_GLYPH_H, 0x18314A);
    fb_draw_string(g_dst, 4, sy + 3, st, COL_PROMPT);
    return w;
}

static int ed_draw_row_fast(uint32_t n, uint32_t coloff, uint32_t top, int from_col) {
    int cols = (int)(FB->width / FB_GLYPH_W);
    int r  = (int)(g_ed_cli - top);
    int y0 = (r + 1) * FB_GLYPH_H;
    if (from_col < 0) from_col = 0;
    int y = y0 + 2, x = 4 + from_col * FB_GLYPH_W;
    uint32_t le = ed_line_end(g_ed_ls, n), pos = g_ed_ls;

    for (uint32_t s = 0; s < coloff + (uint32_t)from_col && pos < le; s++)
        pos = ed_next_ch(pos, n);
    int cx = from_col;
    for (; cx < cols && pos < le; cx++) {
        uint32_t cp; int l = ed_decode(pos, n, &cp);
        if (cp == '\t') cp = ' ';

        fb_fill_rect(g_dst, x, y0, FB_GLYPH_W, FB_GLYPH_H, COL_BG);
        fb_draw_glyph(g_dst, x, y, cp, COL_TEXT);
        x += FB_GLYPH_W;
        pos += (uint32_t)l;
    }

    if (cx < cols) {
        fb_fill_rect(g_dst, x, y0, FB_GLYPH_W, FB_GLYPH_H, COL_BG);
        return x + FB_GLYPH_W;
    }
    return x;
}

static void ed_draw_all(const char *fname, uint32_t n, uint32_t cur,
                        uint32_t top, uint32_t coloff) {
    int cols = (int)(FB->width  / FB_GLYPH_W);
    int rows = (int)(FB->height / FB_GLYPH_H);
    int trows = rows - 2; if (trows < 1) trows = 1;

    fb_fill_rect(g_dst, 0, 0, (int)FB->width, (int)FB->height, COL_BG);

    fb_fill_rect(g_dst, 0, 0, (int)FB->width, FB_GLYPH_H, 0x18314A);
    char bar[320]; bar[0] = 0;
    kstrcat(bar, " edit  "); kstrcat(bar, fname);
    fb_draw_string(g_dst, 4, 3, bar, COL_TITLE);

    uint32_t off = 0;
    for (uint32_t l = 0; l < top && off < n; off++) if (file_buf[off] == '\n') l++;
    for (int r = 0; r < trows; r++) {
        uint32_t le = ed_line_end(off, n);
        int y = (r + 1) * FB_GLYPH_H + 2, x = 4;
        uint32_t pos = off;
        for (uint32_t s = 0; s < coloff && pos < le; s++) pos = ed_next_ch(pos, n);
        for (int cx = 0; cx < cols && pos < le; cx++) {
            uint32_t cp; int l = ed_decode(pos, n, &cp);
            if (cp == '\t') cp = ' ';
            fb_draw_glyph(g_dst, x, y, cp, COL_TEXT);
            x += FB_GLYPH_W;
            pos += (uint32_t)l;
        }
        off = (le < n) ? le + 1 : n;
    }

    {
        int sy = (rows - 1) * FB_GLYPH_H;
        fb_fill_rect(g_dst, 0, sy, (int)FB->width, FB_GLYPH_H, 0x18314A);
    }
    ed_draw_status(n, cur, g_ed_cli);
}

static void ed_draw_rows(uint32_t n, uint32_t top, uint32_t coloff, int r0, int r1) {
    int cols  = (int)(FB->width  / FB_GLYPH_W);
    int rows  = (int)(FB->height / FB_GLYPH_H);
    int trows = rows - 2; if (trows < 1) trows = 1;
    if (r0 < 0) r0 = 0;
    if (r1 > trows) r1 = trows;
    if (r1 <= r0) return;

    uint32_t off = 0, target = top + (uint32_t)r0;
    for (uint32_t l = 0; l < target && off < n; off++) if (file_buf[off] == '\n') l++;
    for (int r = r0; r < r1; r++) {
        uint32_t le = ed_line_end(off, n);
        int y0 = (r + 1) * FB_GLYPH_H;
        fb_fill_rect(g_dst, 0, y0, (int)FB->width, FB_GLYPH_H, COL_BG);
        int y = y0 + 2, x = 4;
        uint32_t pos = off;
        for (uint32_t s = 0; s < coloff && pos < le; s++) pos = ed_next_ch(pos, n);
        for (int cx = 0; cx < cols && pos < le; cx++) {
            uint32_t cp; int l = ed_decode(pos, n, &cp);
            if (cp == '\t') cp = ' ';
            fb_draw_glyph(g_dst, x, y, cp, COL_TEXT);
            x += FB_GLYPH_W;
            pos += (uint32_t)l;
        }
        off = (le < n) ? le + 1 : n;
    }
}

static void ed_cursor(int on) {
    uint32_t fg = on ? COL_BG   : COL_TEXT;
    uint32_t bg = on ? COL_TEXT : COL_BG;
    fb_fill_rect(g_dst, g_ed_cx, g_ed_cy, FB_GLYPH_W, FB_GLYPH_H, bg);
    if (g_ed_ccp != ' ') fb_draw_glyph(g_dst, g_ed_cx, g_ed_cy, g_ed_ccp, fg);
}

static void ed_snapshot(uint32_t n, uint32_t cur) {
    memcpy(undo_buf, file_buf, n);
    g_undo_n = n; g_undo_cur = cur; g_undo_valid = 1;
}

typedef enum { UNDO_NONE = 0, UNDO_INSERT, UNDO_DELETE, UNDO_DELLINE } undo_run_t;
static undo_run_t g_undo_run;

static void ed_snapshot_if_needed(uint32_t n, uint32_t cur, undo_run_t kind) {
    if (g_undo_run != kind) {
        ed_snapshot(n, cur);
        g_undo_run = kind;
    }
}

static char g_ed_search[64];

/* Case-insensitive substring search over file_buf, wrapping around the end
   of the buffer. Starts scanning at 'from'. */
static int ed_find_next(uint32_t n, uint32_t from, const char *needle, uint32_t *out_pos) {
    uint32_t nl = 0; while (needle[nl]) nl++;
    if (nl == 0 || nl > n || n == 0) return 0;
    for (uint32_t offset = 0; offset < n; offset++) {
        uint32_t i = (from + offset) % n;
        if (i + nl > n) continue;
        uint32_t k = 0;
        for (; k < nl; k++) {
            char a = (char)file_buf[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == nl) { *out_pos = i; return 1; }
    }
    return 0;
}

/* Draws a "Find:" prompt on the status row and reads a search term,
   pre-filled with the last search. Enter searches forward from just past
   cur (wrapping around); ESC cancels. Returns 1 and sets *out_pos on a
   match, 0 otherwise. */
static int ed_search_prompt(uint32_t n, uint32_t cur, uint32_t *out_pos) {
    char buf[64];
    int len = 0;
    while (g_ed_search[len] && len < (int)sizeof buf - 1) { buf[len] = g_ed_search[len]; len++; }
    buf[len] = 0;

    int rows = (int)(FB->height / FB_GLYPH_H);
    int sy = (rows - 1) * FB_GLYPH_H;

    for (;;) {
        char line[96]; line[0] = 0;
        kstrcat(line, " Find: "); kstrcat(line, buf);
        fb_fill_rect(g_dst, 0, sy, (int)FB->width, FB_GLYPH_H, 0x18314A);
        fb_draw_string(g_dst, 4, sy + 3, line, COL_PROMPT);
        ed_present(sy, sy + FB_GLYPH_H);

        char c = kbd_getchar();
        if (c == '\n') {
            if (len == 0) return 0;
            buf[len] = 0;
            int k = 0; while (buf[k] && k < (int)sizeof g_ed_search - 1) { g_ed_search[k] = buf[k]; k++; }
            g_ed_search[k] = 0;
            uint32_t from = ed_next_ch(cur, n);
            if (from >= n) from = 0;
            return ed_find_next(n, from, buf, out_pos);
        }
        if (c == 27) return 0;
        if (c == '\b') { if (len > 0) len--; buf[len] = 0; continue; }
        if ((unsigned char)c >= 32 && (unsigned char)c <= 126 && len < (int)sizeof buf - 1) {
            buf[len++] = c; buf[len] = 0;
        }
    }
}

/* Full-screen editor with incremental layout and partial presentation. */
static void cmd_edit(const char *arg) {
    if (!fat_mounted()) { console_puts("edit: no storage mounted.\n"); return; }
    if (!arg || !*arg) { console_puts("Usage: edit <file>\n"); return; }

    const uint32_t CAP = sizeof file_buf - 1;
    uint32_t n = fat_read_file(arg, file_buf, CAP);
    uint32_t cur = 0, top = 0, coloff = 0, cli = 0;

    g_undo_valid = 0;
    g_undo_run = UNDO_NONE;

    fb_info shadow = *FB;
    if ((uint32_t)FB->pitch * FB->height <= sizeof pix_buf) shadow.addr = pix_buf;
    g_dst = (shadow.addr == pix_buf) ? &shadow : FB;

    ed_layout(n, cur, cli, &top, &coloff);
    ed_draw_all(arg, n, cur, top, coloff);
    ed_cursor(1);
    ed_present(0, (int)FB->height);

    int save = 1;

    for (;;) {

        /* Blocking input avoids slow port 0x80 delay loops on real hardware. */
        char c = kbd_getchar();

        int old_cx = g_ed_cx, old_cy = g_ed_cy;
        uint32_t old_cli0 = cli;
        ed_cursor(0);

        int tchg = 0, any_multiline = 0, full = 0;
        uint32_t batch_min_cli = cli;
        int want_exit = 0, exit_save = 1, nkeys = 0;

        for (;;) {
            int single_line = 0;
            uint32_t pre_cli = cli;

            if (c == 27 || c == 0x18) { want_exit = 1; exit_save = 1; break; }
            if (c == 0x11)            { want_exit = 1; exit_save = 0; break; }

            if (c == KEY_LEFT)  {
            if (cur > 0) { uint32_t p = ed_prev_ch(cur);
                           if (file_buf[p] == '\n') cli--;
                           cur = p; }
        }
        else if (c == KEY_RIGHT) {
            if (cur < n) { if (file_buf[cur] == '\n') cli++;
                           cur = ed_next_ch(cur, n); }
        }
        else if (c == KEY_HOME)  { cur = ed_line_start(cur); }
        else if (c == KEY_END)   { cur = ed_line_end(cur, n); }
        else if (c == KEY_PGUP || c == KEY_PGDN) {

            int page = (int)(FB->height / FB_GLYPH_H) - 2; if (page < 1) page = 1;
            uint32_t s = ed_line_start(cur), col = ed_col_of(s, cur);
            if (c == KEY_PGUP) {
                for (int k = 0; k < page && s > 0; k++) { s = ed_line_start(s - 1); cli--; }
            } else {
                for (int k = 0; k < page; k++) {
                    uint32_t e = ed_line_end(s, n);
                    if (e >= n) break;
                    s = e + 1; cli++;
                }
            }
            uint32_t e = ed_line_end(s, n);
            cur = ed_off_at_col(s, e, n, col);
        }
        else if (c == KEY_UP) {
            uint32_t s = ed_line_start(cur), col = ed_col_of(s, cur);
            if (s == 0) { cur = 0; cli = 0; }
            else { uint32_t ps = ed_line_start(s - 1);
                   cur = ed_off_at_col(ps, s - 1, n, col); cli--; }
        }
        else if (c == KEY_DOWN) {
            uint32_t s = ed_line_start(cur), col = ed_col_of(s, cur);
            uint32_t e = ed_line_end(cur, n);
            if (e >= n) cur = n;
            else { uint32_t ns = e + 1, ne = ed_line_end(ns, n);
                   cur = ed_off_at_col(ns, ne, n, col); cli++; }
        }
        else if (c == '\b') {
            if (cur > 0) { uint32_t p = ed_prev_ch(cur), cnt = cur - p;
                           if (file_buf[p] != '\n') single_line = 1; else cli--;
                           ed_snapshot_if_needed(n, cur, UNDO_DELETE);
                           kmemmove(file_buf + p, file_buf + p + cnt, n - (p + cnt));
                           n -= cnt; cur = p; tchg = 1; }
        }
        else if (c == KEY_DEL) {
            if (cur < n) { uint32_t nx = ed_next_ch(cur, n), cnt = nx - cur;
                           if (file_buf[cur] != '\n') single_line = 1;
                           ed_snapshot_if_needed(n, cur, UNDO_DELETE);
                           kmemmove(file_buf + cur, file_buf + cur + cnt, n - (cur + cnt));
                           n -= cnt; tchg = 1; }
        }
        else if (c == 0x0B) {
            uint32_t s = ed_line_start(cur), e = ed_line_end(cur, n);
            uint32_t to = (e < n) ? e + 1 : e, cnt = to - s;
            ed_snapshot_if_needed(n, cur, UNDO_DELLINE);
            kmemmove(file_buf + s, file_buf + s + cnt, n - (s + cnt));
            n -= cnt; cur = (s < n) ? s : n; tchg = 1;
        }
        else if (c == 0x1A) {
            if (g_undo_valid) { memcpy(file_buf, undo_buf, g_undo_n);
                                n = g_undo_n; cur = (g_undo_cur <= n) ? g_undo_cur : n;
                                g_undo_valid = 0; g_undo_run = UNDO_NONE; tchg = 1; full = 1;
                                cli = ed_count_lines_before(cur);   }
        }
        else if (c == 0x06) {
            uint32_t found_pos;
            if (ed_search_prompt(n, cur, &found_pos)) {
                cur = found_pos;
                cli = ed_count_lines_before(cur);
            }
            full = 1;
        }
        else if (c == KEY_F12) {
            capture_screenshot_auto(g_dst);
        }
        else if (c == '\t') {

            int can = (int)(CAP - n); if (can > 4) can = 4;
            if (can > 0) {
                ed_snapshot_if_needed(n, cur, UNDO_INSERT);
                kmemmove(file_buf + cur + (uint32_t)can, file_buf + cur, n - cur);
                for (int k = 0; k < can; k++) file_buf[cur + (uint32_t)k] = ' ';
                n += (uint32_t)can; cur += (uint32_t)can;
                tchg = 1; single_line = 1;
            }
        }
        else if (c == '\n' || (c >= 32 && c <= 126)) {
            if (n < CAP) { ed_snapshot_if_needed(n, cur, UNDO_INSERT);
                           kmemmove(file_buf + cur + 1, file_buf + cur, n - cur);
                           file_buf[cur] = (uint8_t)c; n++; cur++; tchg = 1;
                           if (c != '\n') single_line = 1; else cli++; }
        }

            if (tchg && !single_line) any_multiline = 1;
            if (pre_cli < batch_min_cli) batch_min_cli = pre_cli;
            if (cli      < batch_min_cli) batch_min_cli = cli;

            if (++nkeys >= 64) break;
            c = kbd_poll();
            if (c == 0) break;
        }

        if (want_exit) { save = exit_save; break; }

        uint32_t otop = top, ocol = coloff;
        int scrolled = ed_layout(n, cur, cli, &top, &coloff);

        int rows  = (int)(FB->height / FB_GLYPH_H);
        int trows = rows - 2; if (trows < 1) trows = 1;
        if (full) {

            ed_draw_all(arg, n, cur, top, coloff);
            ed_cursor(1);
            ed_present(0, (int)FB->height);
        } else if (scrolled) {
            int delta = (int)top - (int)otop;
            int ad = delta < 0 ? -delta : delta;

            if (g_dst != FB && !tchg && coloff == ocol && ad > 0 && ad < trows) {
                int ty0 = FB_GLYPH_H;
                int th  = trows * FB_GLYPH_H;
                int as  = ad * FB_GLYPH_H;
                uint32_t p = FB->pitch;
                if (delta > 0) {
                    kmemmove(g_dst->addr + (uint32_t)ty0 * p,
                             g_dst->addr + (uint32_t)(ty0 + as) * p,
                             (uint32_t)(th - as) * p);
                    ed_draw_rows(n, top, coloff, trows - ad, trows);
                } else {
                    kmemmove(g_dst->addr + (uint32_t)(ty0 + as) * p,
                             g_dst->addr + (uint32_t)ty0 * p,
                             (uint32_t)(th - as) * p);
                    ed_draw_rows(n, top, coloff, 0, ad);
                }
                ed_cursor(1);
                ed_present(ty0, ty0 + th);
            } else {
                ed_draw_all(arg, n, cur, top, coloff);
                ed_cursor(1);
                ed_present(0, (int)FB->height);
                }
        } else if (tchg && (any_multiline || g_ed_cli != old_cli0)) {

            uint32_t minc = batch_min_cli;
            if (old_cli0 < minc) minc = old_cli0;
            int r0 = (int)minc - (int)top;
            if (r0 < 0) r0 = 0;
            ed_draw_rows(n, top, coloff, r0, trows);
            ed_cursor(1);
            ed_present((r0 + 1) * FB_GLYPH_H, (int)FB->height);
        } else if (tchg) {

            int cx0   = (old_cx < g_ed_cx) ? old_cx : g_ed_cx;
            int fromc = (cx0 - 4) / FB_GLYPH_W;
            int xend  = ed_draw_row_fast(n, coloff, top, fromc);
            ed_cursor(1);
            int ry  = ((int)(g_ed_cli - top) + 1) * FB_GLYPH_H;
            int cx1 = g_ed_cx + FB_GLYPH_W;
            if (xend > cx1) cx1 = xend;
            ed_present_rect(cx0, ry, cx1, ry + FB_GLYPH_H);
        } else {

            ed_cursor(1);
            ed_present_rect(old_cx, old_cy, old_cx + FB_GLYPH_W, old_cy + FB_GLYPH_H);
            ed_present_rect(g_ed_cx, g_ed_cy, g_ed_cx + FB_GLYPH_W, g_ed_cy + FB_GLYPH_H);
        }
    }

    console_clear();
    if (!save) {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("Exited without saving.\n");
        console_set_colors(COL_TEXT, COL_BG);
    } else if (fat_write_file(arg, file_buf, n)) {
        console_set_colors(COL_PROMPT, COL_BG);
        char l[320]; l[0] = 0; kstrcat(l, "Saved "); kstrcat(l, arg);
        kstrcat(l, " ("); num(n, l); kstrcat(l, " bytes)\n");
        console_puts(l);
        console_set_colors(COL_TEXT, COL_BG);
    } else {
        console_set_colors(COL_WARN, COL_BG);
        console_puts("edit: save failed (disk full or read-only?)\n");
        console_set_colors(COL_TEXT, COL_BG);
    }
}

static void cmd_about(void) {
    console_puts("fukOS - a tiny hobby operating system.\n");
    console_puts("VBE graphics + PS/2 keyboard + ATA/USB(xHCI) + FAT16/32 storage.\n");
    console_puts("Reads files & folders from a real FAT filesystem: copy files\n");
    console_puts("onto the USB stick from any PC and browse them here.\n");
    console_puts("Boots via Limine (BIOS + UEFI, Multiboot2).\n");
    console_puts("My website - encrize.vip\n");
}

static void cmd_time(void) {
    rtc_time_t t;
    rtc_read(&t);
    char line[80];
    line[0] = 0;
    num(t.year, line);    kstrcat(line, "-");
    num2(t.month, line);  kstrcat(line, "-");
    num2(t.day, line);    kstrcat(line, "  ");
    num2(t.hour, line);   kstrcat(line, ":");
    num2(t.minute, line); kstrcat(line, ":");
    num2(t.second, line); kstrcat(line, "\n");
    console_puts(line);
    console_set_colors(COL_WARN, COL_BG);
    console_puts("(hardware RTC clock - typically UTC)\n");
    console_set_colors(COL_TEXT, COL_BG);
}

static const char *const CLOCK_DIGITS[10][7] = {
    {"#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"},
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."},
    {"#####", "....#", "....#", "#####", "#....", "#....", "#####"},
    {"#####", "....#", "....#", "#####", "....#", "....#", "#####"},
    {"#...#", "#...#", "#...#", "#####", "....#", "....#", "....#"},
    {"#####", "#....", "#....", "#####", "....#", "....#", "#####"},
    {"#####", "#....", "#....", "#####", "#...#", "#...#", "#####"},
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."},
    {"#####", "#...#", "#...#", "#####", "#...#", "#...#", "#####"},
    {"#####", "#...#", "#...#", "#####", "....#", "....#", "#####"}
};

static void clock_digit(int digit, int x, int y, int scale) {
    if (digit < 0 || digit > 9 || scale < 1) return;
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (CLOCK_DIGITS[digit][row][col] == '#')
                fb_fill_rect(FB, x + col * scale, y + row * scale,
                             scale, scale, 0xF7F7FAu);
        }
    }
}

static void clock_card(int x, int y, int w, int h, int left, int right) {
    fb_fill_rect(FB, x - 3, y - 3, w + 6, h + 6, 0x08090Du);
    fb_fill_rect(FB, x, y, w, h, 0x292A32u);
    fb_fill_rect(FB, x + 5, y + 5, w - 10, h / 2 - 5, 0x30313Au);
    fb_fill_rect(FB, x + 5, y + h / 2, w - 10, h / 2 - 5, 0x24252Du);

    int sx = (w - 36) / 11;
    int sy = (h - 48) / 7;
    int scale = sx < sy ? sx : sy;
    if (scale < 2) scale = 2;
    int total_w = 11 * scale;
    int dx = x + (w - total_w) / 2;
    int dy = y + (h - 7 * scale) / 2;
    clock_digit(left, dx, dy, scale);
    clock_digit(right, dx + 6 * scale, dy, scale);

    fb_fill_rect(FB, x, y + h / 2 - 2, w, 4, 0x111218u);
    fb_fill_rect(FB, x + 7, y + h / 2 - 1, w - 14, 1, 0x454650u);
}

static void draw_clock_face(const rtc_time_t *t) {
    fb_clear(FB, 0x080A10u);
    int margin = (int)FB->width / 24;
    if (margin < 18) margin = 18;
    int colon_w = (int)FB->width / 14;
    if (colon_w < 40) colon_w = 40;
    int card_w = ((int)FB->width - 2 * margin - colon_w) / 2;
    int card_h = (int)FB->height * 68 / 100;
    int card_y = ((int)FB->height - card_h) / 2 - 20;
    if (card_y < 18) card_y = 18;

    clock_card(margin, card_y, card_w, card_h, t->hour / 10, t->hour % 10);
    int second_x = margin + card_w + colon_w;
    clock_card(second_x, card_y, card_w, card_h, t->minute / 10, t->minute % 10);

    int cx = margin + card_w + colon_w / 2;
    int dot = colon_w / 6;
    if (dot < 8) dot = 8;
    fb_fill_rect(FB, cx - dot / 2, card_y + card_h / 3 - dot / 2,
                 dot, dot, 0xF7F7FAu);
    fb_fill_rect(FB, cx - dot / 2, card_y + card_h * 2 / 3 - dot / 2,
                 dot, dot, 0xF7F7FAu);

    char footer[96]; footer[0] = 0;
    num(t->year, footer); kstrcat(footer, "-"); num2(t->month, footer);
    kstrcat(footer, "-"); num2(t->day, footer);
    kstrcat(footer, "   Q / Esc / Enter to exit");
    int footer_x = ((int)FB->width - (int)kstrlen(footer) * FB_GLYPH_W) / 2;
    if (footer_x < 8) footer_x = 8;
    fb_draw_string(FB, footer_x, (int)FB->height - FB_GLYPH_H - 18,
                   footer, 0xAEB2C0u);
}

static void cmd_clock(void) {
    uint8_t previous_hour = 0xFFu;
    uint8_t previous_minute = 0xFFu;
    for (;;) {
        rtc_time_t now;
        rtc_read(&now);
        if (now.hour != previous_hour || now.minute != previous_minute) {
            draw_clock_face(&now);
            previous_hour = now.hour;
            previous_minute = now.minute;
        }

        xhci_idle_drain();
        hda_bg_poll();
        char c = kbd_poll();
        if (c == KEY_F12) { capture_screenshot_auto(FB); continue; }
        if (c == 'q' || c == 'Q' || c == 27 || c == '\n') break;
    }
    console_init(FB);
    console_set_colors(COL_TEXT, COL_BG);
}

static void reboot(void) {

    outb(0xCF9, 0x02);
    outb(0xCF9, 0x0E);
    for (volatile int i = 0; i < 1000000; i++) { }

    for (int i = 0; i < 10000; i++) { if ((inb(0x64) & 0x02) == 0) break; }
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) { }

    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idt0 = { 0, 0 };
    __asm__ volatile ("lidt %0" :: "m"(idt0));
    __asm__ volatile ("int3");

    for (;;) __asm__ volatile ("hlt");
}

static const uint8_t *g_acpi_rsdp;

static int acpi_sig_eq(const uint8_t *p, const char *s, int n) {
    for (int i = 0; i < n; i++) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}
static int acpi_csum_ok(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0; for (uint32_t i = 0; i < len; i++) sum += p[i]; return sum == 0;
}
static const uint8_t *acpi_find_rsdp(void) {
    uint32_t ebda = (uint32_t)(*(volatile uint16_t *)0x40E) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000)
        for (uint32_t x = ebda; x < ebda + 1024; x += 16) {
            const uint8_t *p = (const uint8_t *)x;
            if (acpi_sig_eq(p, "RSD PTR ", 8) && acpi_csum_ok(p, 20)) return p;
        }
    for (uint32_t x = 0xE0000; x < 0x100000; x += 16) {
        const uint8_t *p = (const uint8_t *)x;
        if (acpi_sig_eq(p, "RSD PTR ", 8) && acpi_csum_ok(p, 20)) return p;
    }
    return 0;
}

static int acpi_poweroff(void) {

    const uint8_t *rsdp = g_acpi_rsdp;
    if (!rsdp || !acpi_sig_eq(rsdp, "RSD PTR ", 8)) rsdp = acpi_find_rsdp();
    if (!rsdp) return 0;

    const uint8_t *rsdt = 0; int entry64 = 0;
    if (rsdp[15] >= 2) {
        uint64_t xsdt = (uint64_t)(*(const uint32_t *)(rsdp + 24)) |
                        ((uint64_t)(*(const uint32_t *)(rsdp + 28)) << 32);
        if (xsdt && xsdt < 0x100000000ULL) { rsdt = (const uint8_t *)(uintptr_t)xsdt; entry64 = 1; }
    }
    if (!rsdt) {
        uint32_t r = *(const uint32_t *)(rsdp + 16);
        if (!r) return 0;
        rsdt = (const uint8_t *)(uintptr_t)r;
    }

    uint32_t rlen = *(const uint32_t *)(rsdt + 4);
    if (rlen < 36 || rlen > 0x10000) return 0;
    uint32_t n = (rlen - 36) / (entry64 ? 8 : 4);
    const uint8_t *fadt = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t p = entry64
            ? ((uint64_t)(*(const uint32_t *)(rsdt + 36 + i * 8)) |
               ((uint64_t)(*(const uint32_t *)(rsdt + 36 + i * 8 + 4)) << 32))
            : (uint64_t)(*(const uint32_t *)(rsdt + 36 + i * 4));
        if (!p || p >= 0x100000000ULL) continue;
        const uint8_t *sdt = (const uint8_t *)(uintptr_t)p;
        if (acpi_sig_eq(sdt, "FACP", 4)) { fadt = sdt; break; }
    }
    if (!fadt) return 0;

    uint32_t smi_cmd     = *(const uint32_t *)(fadt + 48);
    uint8_t  acpi_enable = fadt[52];
    uint32_t pm1a_cnt    = *(const uint32_t *)(fadt + 64);
    uint32_t pm1b_cnt    = *(const uint32_t *)(fadt + 68);
    uint32_t dsdt_addr   = *(const uint32_t *)(fadt + 40);
    if (dsdt_addr == 0 && rsdp[15] >= 2)
        dsdt_addr = (uint32_t)(*(const uint32_t *)(fadt + 140));
    if (!dsdt_addr) return 0;

    const uint8_t *dsdt = (const uint8_t *)(uintptr_t)dsdt_addr;
    if (!acpi_sig_eq(dsdt, "DSDT", 4)) return 0;
    uint32_t dlen = *(const uint32_t *)(dsdt + 4);
    if (dlen < 36 || dlen > 0x200000) return 0;

    const uint8_t *s5 = 0;
    for (uint32_t i = 36; i + 5 < dlen; i++) {
        if (acpi_sig_eq(dsdt + i, "_S5_", 4) && dsdt[i + 4] == 0x12 &&
            ((dsdt[i - 1] == 0x08) || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == 0x5C))) {
            s5 = dsdt + i; break;
        }
    }
    if (!s5) return 0;

    const uint8_t *p = s5 + 5;
    p += ((*p & 0xC0) >> 6) + 2;
    if (*p == 0x0A) p++;
    uint8_t slp_a = *p; p++;
    if (*p == 0x0A) p++;
    uint8_t slp_b = *p;

    if (pm1a_cnt) {
        const uint16_t SLP_EN = (uint16_t)(1u << 13);
        if (smi_cmd && acpi_enable && !(inw((uint16_t)pm1a_cnt) & 1u)) {
            outb((uint16_t)smi_cmd, acpi_enable);
            for (int i = 0; i < 1000000; i++) if (inw((uint16_t)pm1a_cnt) & 1u) break;
        }
        outw((uint16_t)pm1a_cnt, (uint16_t)(((uint16_t)slp_a << 10) | SLP_EN));
        if (pm1b_cnt) outw((uint16_t)pm1b_cnt, (uint16_t)(((uint16_t)slp_b << 10) | SLP_EN));
        return 1;
    }

    uint32_t fadt_len = *(const uint32_t *)(fadt + 4);
    if (fadt_len >= 129) {
        uint8_t  space = fadt[116];
        uint64_t addr  = (uint64_t)(*(const uint32_t *)(fadt + 116 + 4)) |
                         ((uint64_t)(*(const uint32_t *)(fadt + 116 + 8)) << 32);
        if (addr) {
            uint8_t val = (uint8_t)(((uint32_t)slp_a << 2) | (1u << 5));
            if (space == 1) outb((uint16_t)addr, val);
            else            *(volatile uint8_t *)(uintptr_t)addr = val;
            return 1;
        }
    }
    return 0;
}

static void poweroff(void) {
    console_puts("Shutting down...\n");
    acpi_poweroff();

    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    console_puts("Power-off is not supported by this firmware; it is now safe to\n"
                 "hold the power button to turn the machine off.\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* Command history ring. */
#define HIST_MAX 32
static char hist[HIST_MAX][128];
static int  hist_count;
static int  hist_head;

static void hist_add(const char *s) {
    if (s[0] == 0) return;
    if (hist_count > 0) {
        int last = (hist_head - 1 + HIST_MAX) % HIST_MAX;
        if (streq(hist[last], s)) return;
    }
    int i = 0;
    for (; s[i] && i < 127; i++) hist[hist_head][i] = s[i];
    hist[hist_head][i] = 0;
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
}

static const char *hist_get(int back) {
    if (back < 1 || back > hist_count) return 0;
    return hist[(hist_head - back + HIST_MAX) % HIST_MAX];
}

static void line_copy(char *dst, const char *src, int max, int *length) {
    int n = 0;
    while (src[n] && n < max - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    *length = n;
}

static int common_prefix_ci(const char *a, const char *b) {
    int n = 0;
    while (a[n] && b[n] && lc(a[n]) == lc(b[n])) n++;
    return n;
}

static void apply_completion(char *buf, int max, int *length, int *cursor,
                             int start, int end, const char *replacement, int add_suffix,
                             char suffix) {
    int old = end - start;
    int rep = (int)kstrlen(replacement);
    int extra = add_suffix ? 1 : 0;
    int delta = rep + extra - old;
    if (*length + delta >= max) return;
    kmemmove(buf + start + rep + extra, buf + end,
             (uint32_t)(*length - end + 1));
    for (int i = 0; i < rep; i++) buf[start + i] = replacement[i];
    if (add_suffix) buf[start + rep] = suffix;
    *length += delta;
    *cursor = start + rep + extra;
}

static void complete_line(char *buf, int max, int *length, int *cursor) {
    int start = *cursor;
    while (start > 0 && buf[start - 1] != ' ') start--;
    int end = *cursor;
    while (end < *length && buf[end] != ' ') end++;
    int prefix_len = *cursor - start;
    int command = 1;
    for (int i = 0; i < start; i++) if (buf[i] != ' ') { command = 0; break; }

    char common[128];
    common[0] = 0;
    int matches = 0;
    int single_dir = 0;

    if (command) {
        for (int i = 0; i < SHELL_COMMAND_COUNT; i++) {
            const char *name = SHELL_COMMANDS[i];
            if (!starts_ci(name, buf + start, prefix_len)) continue;
            if (matches == 0) {
                int ignored;
                line_copy(common, name, sizeof common, &ignored);
            } else {
                int n = common_prefix_ci(common, name);
                common[n] = 0;
            }
            matches++;
        }
    } else if (fat_mounted()) {
        uint32_t count = fat_dir_count();
        for (uint32_t i = 0; i < count; i++) {
            fat_dirent entry;
            if (!fat_dir_get(i, &entry) || !starts_ci(entry.name, buf + start, prefix_len)) continue;
            if (matches == 0) {
                int ignored;
                line_copy(common, entry.name, sizeof common, &ignored);
                single_dir = entry.is_dir;
            } else {
                int n = common_prefix_ci(common, entry.name);
                common[n] = 0;
            }
            matches++;
        }
    }

    if (matches == 0) return;
    int common_len = (int)kstrlen(common);
    if (common_len < prefix_len) return;
    int suffix = matches == 1;
    apply_completion(buf, max, length, cursor, start, end, common, suffix,
                     command || !single_dir ? ' ' : '/');
}

static void readline(char *buf, int max) {
    char draft[128];
    int n = 0;
    int cursor = 0;
    int history_pos = 0;
    buf[0] = 0;
    draft[0] = 0;
    console_input_begin();
    console_input_redraw(buf, n, cursor, 1);

    for (;;) {
        char c = kbd_getchar();
        if (c == KEY_F12) {
            capture_screenshot_auto(0);
            continue;
        }
        if (c == '\t') {
            complete_line(buf, max, &n, &cursor);
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if (c == '\n') {
            console_input_redraw(buf, n, cursor, 0);
            console_input_end();
            console_putc('\n');
            hist_add(buf);
            return;
        }
        if (c == '\b') {
            if (cursor > 0) {
                kmemmove(buf + cursor - 1, buf + cursor, (uint32_t)(n - cursor + 1));
                cursor--;
                n--;
            }
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if (c == KEY_DEL) {
            if (cursor < n) {
                kmemmove(buf + cursor, buf + cursor + 1, (uint32_t)(n - cursor));
                n--;
            }
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if (c == KEY_LEFT) {
            if (cursor > 0) cursor--;
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if (c == KEY_RIGHT) {
            if (cursor < n) cursor++;
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if (c == KEY_HOME || c == KEY_END) {
            cursor = c == KEY_HOME ? 0 : n;
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if (c == KEY_SCROLL_UP || c == KEY_PGUP) {
            console_scrollback_up(c == KEY_PGUP ? 20 : 1);
            continue;
        }
        if (c == KEY_SCROLL_DOWN || c == KEY_PGDN) {
            console_scrollback_down(c == KEY_PGDN ? 20 : 1);
            continue;
        }
        if (c == KEY_UP || c == KEY_DOWN) {
            console_scrollback_live();
            if (history_pos == 0 && c == KEY_UP) {
                int unused;
                line_copy(draft, buf, (int)sizeof(draft), &unused);
            }
            int np = history_pos + (c == KEY_UP ? 1 : -1);
            if (np < 0) np = 0;
            if (np > hist_count) np = hist_count;
            if (np == history_pos) continue;
            history_pos = np;
            line_copy(buf, history_pos ? hist_get(history_pos) : draft, max, &n);
            cursor = n;
            console_input_redraw(buf, n, cursor, 1);
            continue;
        }
        if ((unsigned char)c < 32 || (unsigned char)c > 126) continue;
        if (n < max - 1) {
            kmemmove(buf + cursor + 1, buf + cursor, (uint32_t)(n - cursor + 1));
            buf[cursor++] = c;
            n++;
            console_input_redraw(buf, n, cursor, 1);
        }
    }
}

static char *split_arg(char *line) {
    char *p = line;
    while (*p && *p != ' ') p++;
    if (*p == 0) return p;
    *p++ = 0;
    while (*p == ' ') p++;
    return p;
}

static char g_config_message[640];

static uint32_t config_uint(const char *s, int *ok) {
    uint32_t v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s - '0'); s++; any = 1; }
    while (*s == ' ' || *s == '\t') s++;
    *ok = any && *s == 0;
    return v;
}

static int load_image_path(const char *path, image_t *image) {
    char tmp[256]; int n = 0;
    while (path[n] && n < 255) { tmp[n] = path[n]; n++; }
    tmp[n] = 0;
    fat_cwd_reset();
    char *part = tmp;
    for (;;) {
        char *slash = part;
        while (*slash && *slash != '/') slash++;
        if (!*slash) break;
        *slash = 0;
        if (*part && !fat_chdir(part)) { fat_cwd_reset(); return 0; }
        part = slash + 1;
    }
    uint32_t got = fat_read_file(part, file_buf, sizeof file_buf);
    int ok = got && image_decode(file_buf, got, pix_buf, sizeof pix_buf, image);
    fat_cwd_reset();
    return ok;
}

static int load_wallpaper_path(const char *path) {
    image_t image;
    return load_image_path(path, &image) &&
           console_set_wallpaper(image.pixels, image.width, image.height);
}

static int show_boot_image(const char *path, uint32_t seconds, int fill) {
    image_t image;
    if (!load_image_path(path, &image)) return 0;
    render_photo_fullscreen(FB, &image, fill, 0, 0);
    rtc_time_t started, now;
    rtc_read(&started);
    uint32_t start_s = (uint32_t)started.hour * 3600u +
                       (uint32_t)started.minute * 60u + started.second;
    for (;;) {
        rtc_read(&now);
        uint32_t now_s = (uint32_t)now.hour * 3600u +
                         (uint32_t)now.minute * 60u + now.second;
        uint32_t elapsed = now_s >= start_s ? now_s - start_s
                                            : 86400u - start_s + now_s;
        if (elapsed >= seconds) break;
        xhci_idle_drain();
        hda_bg_poll();
        if (kbd_poll()) break;
    }
    console_init(FB);
    return 1;
}

static void load_fuko_config(void) {
    g_config_message[0] = 0;
    if (!fat_mounted()) return;
    fat_cwd_reset();
    uint32_t got = fat_read_file("fuko.conf", file_buf, 4095u);
    if (!got) return;
    file_buf[got] = 0;
    char wallpaper[256]; wallpaper[0] = 0;
    char boot_image[256]; boot_image[0] = 0;
    uint32_t boot_seconds = 3u;
    int boot_fill = 0;
    uint32_t transparency = 0; int have_transparency = 0;
    int audio_output = -1;
    char *p = (char *)file_buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) { *p++ = 0; while (*p == '\n' || *p == '\r') p++; }
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        char *eq = line; while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq++ = 0;
        char *kend = line; while (*kend) kend++;
        while (kend > line && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = 0;
        while (*eq == ' ' || *eq == '\t') eq++;
        char *vend = eq; while (*vend) vend++;
        while (vend > eq && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = 0;
        if (cieq(line, "WALLPAPER")) {
            int i = 0; while (eq[i] && i < 255) { wallpaper[i] = eq[i]; i++; }
            wallpaper[i] = 0;
        } else if (cieq(line, "BOOT_IMAGE")) {
            int i = 0; while (eq[i] && i < 255) { boot_image[i] = eq[i]; i++; }
            boot_image[i] = 0;
        } else if (cieq(line, "BOOT_IMAGE_SECONDS")) {
            int ok = 0; uint32_t value = config_uint(eq, &ok);
            if (ok) boot_seconds = value > 30u ? 30u : value;
        } else if (cieq(line, "BOOT_IMAGE_MODE")) {
            boot_fill = cieq(eq, "fill");
        } else if (cieq(line, "TERMINAL_TRANSPARENCY")) {
            int ok = 0; uint32_t value = config_uint(eq, &ok);
            if (ok) { transparency = value > 100u ? 100u : value; have_transparency = 1; }
        } else if (cieq(line, "AUDIO_OUTPUT")) {
            if (cieq(eq, "auto")) audio_output = 0;
            else if (cieq(eq, "speaker") || cieq(eq, "speakers")) audio_output = 1;
            else if (cieq(eq, "headphones") || cieq(eq, "headphone") || cieq(eq, "hp")) audio_output = 2;
        }
    }
    if (have_transparency) console_set_transparency(transparency);
    if (audio_output >= 0) hda_set_output_mode(audio_output);
    if (boot_image[0] && boot_seconds > 0u) {
        if (show_boot_image(boot_image, boot_seconds, boot_fill)) {
            kstrcat(g_config_message, "fuko.conf: boot image loaded: ");
            kstrcat(g_config_message, boot_image); kstrcat(g_config_message, "\n");
        } else {
            kstrcat(g_config_message, "fuko.conf: cannot load boot image: ");
            kstrcat(g_config_message, boot_image); kstrcat(g_config_message, "\n");
        }
    }
    if (wallpaper[0]) {
        if (load_wallpaper_path(wallpaper)) {
            kstrcat(g_config_message, "fuko.conf: wallpaper loaded: ");
            kstrcat(g_config_message, wallpaper); kstrcat(g_config_message, "\n");
        } else {
            kstrcat(g_config_message, "fuko.conf: cannot load wallpaper: ");
            kstrcat(g_config_message, wallpaper); kstrcat(g_config_message, "\n");
        }
    }
    if (have_transparency) {
        kstrcat(g_config_message, "fuko.conf: terminal transparency ");
        num(transparency, g_config_message); kstrcat(g_config_message, "%\n");
    }
    if (audio_output >= 0) {
        kstrcat(g_config_message, "fuko.conf: audio output ");
        kstrcat(g_config_message, audio_output == 0 ? "auto" :
                (audio_output == 1 ? "speaker" : "headphones"));
        kstrcat(g_config_message, "\n");
    }
}

void shell_run(const fb_info *fb, uint64_t total_ram_bytes, const uint8_t *acpi_rsdp) {
    FB = fb;
    TOTAL_RAM_BYTES = total_ram_bytes;
    SHELL_START_TSC = rdtsc();
    g_acpi_rsdp = acpi_rsdp;
    console_init(fb);
    kbd_init();

    if (!fat_mounted()) fat_mount_ata(0);

    if (!fat_mounted()) {
        pci_dev_t d;
        if (pci_find_class(0x0C, 0x03, 0x30, &d)) {
            uint64_t bar0 = pci_bar_address(d.bus, d.slot, d.func, 0);
            pci_enable_device(d.bus, d.slot, d.func);
            if (bar0 != 0 && xhci_storage_mount(bar0)) fat_mount_usb_auto();
        }
    }

    load_fuko_config();
    console_set_colors(COL_TITLE, COL_BG);
    console_puts("fukOS shell ready.\n");
    if (g_config_message[0]) console_puts(g_config_message);
    console_set_colors(COL_TEXT, COL_BG);

    if (fat_mounted()) {
        uint32_t count = fat_dir_count();
        char msg[96];
        msg[0] = 0;
        kstrcat(msg, "FAT storage mounted: ");
        num(count, msg);
        kstrcat(msg, " item(s) in root. 'ls' list, 'cd' folders, 'img' view.\n");
        console_puts(msg);
        console_puts("Copy files onto the stick from any PC - they appear here.\n\n");
    } else {
        console_puts("No storage disk found. Type 'help'. 'img' shows the demo.\n\n");
    }

    char line[128];
    for (;;) {
        console_set_colors(COL_PROMPT, COL_BG);
        console_puts("root@fukOS:");
        if (fat_mounted()) print_cwd();
        console_puts("> ");
        console_set_colors(COL_TEXT, COL_BG);

        readline(line, (int)sizeof(line));

        char *start = line;
        while (*start == ' ') start++;
        if (*start == 0) continue;

        char *arg = split_arg(start);
        char *cmd = start;

        if      (streq(cmd, "help") || streq(cmd, "?"))    cmd_help();
        else if (streq(cmd, "clear") || streq(cmd, "cls")) console_clear();
        else if (streq(cmd, "ls") || streq(cmd, "dir"))    cmd_ls();
        else if (streq(cmd, "cd"))                         cmd_cd(arg);
        else if (streq(cmd, "pwd"))                        cmd_pwd();
        else if (streq(cmd, "tree"))                       cmd_tree();
        else if (streq(cmd, "cat") || streq(cmd, "type"))  cmd_cat(arg);
        else if (streq(cmd, "head"))                       cmd_head(arg);
        else if (streq(cmd, "wc"))                         cmd_wc(arg);
        else if (streq(cmd, "hexdump"))                    cmd_hexdump(arg);
        else if (streq(cmd, "play") || streq(cmd, "wav"))  cmd_play(arg);
        else if (streq(cmd, "playlist"))                    cmd_play("*.wav");
        else if (streq(cmd, "bgplay"))                      cmd_bgplay(arg);
        else if (streq(cmd, "bgpause")) {
            hda_bg_set_paused(1); cmd_bgstatus();
        }
        else if (streq(cmd, "bgresume")) {
            hda_bg_set_paused(0); cmd_bgstatus();
        }
        else if (streq(cmd, "bgstop")) {
            g_bg_queue_active = 0; hda_bg_stop();
            console_puts("Background music stopped.\n");
        }
        else if (streq(cmd, "bgstatus"))                    cmd_bgstatus();
        else if (streq(cmd, "bgvolume"))                    cmd_bgvolume(arg);
        else if (streq(cmd, "bgrepeat"))                    cmd_bgrepeat(arg);
        else if (streq(cmd, "bgshuffle"))                   cmd_bgshuffle(arg);
        else if (streq(cmd, "bgnext"))                      cmd_bgswitch(1);
        else if (streq(cmd, "bgprev"))                      cmd_bgswitch(-1);
        else if (streq(cmd, "audioout"))                    cmd_audioout(arg);
        else if (streq(cmd, "img") || streq(cmd, "photo") || streq(cmd, "show") ||
                 streq(cmd, "image"))                         cmd_img(arg);
        else if (streq(cmd, "open"))                       cmd_open(arg);
        else if (streq(cmd, "matrix"))                     cmd_matrix();
        else if (streq(cmd, "screenshot"))                 cmd_screenshot(arg);
        else if (streq(cmd, "res"))                        cmd_res();
        else if (streq(cmd, "about"))                      cmd_about();
        else if (streq(cmd, "reboot")) { console_puts("Rebooting...\n"); reboot(); }
        else if (streq(cmd, "poweroff") || streq(cmd, "shutdown") ||
                 streq(cmd, "off"))                        poweroff();
        else if (streq(cmd, "lspci"))                      cmd_lspci();
        else if (streq(cmd, "usb") || streq(cmd, "xhci"))  cmd_usb();
        else if (streq(cmd, "diskinfo"))                   cmd_diskinfo();
        else if (streq(cmd, "time") || streq(cmd, "date")) cmd_time();
        else if (streq(cmd, "clock"))                       cmd_clock();
        else if (streq(cmd, "touch"))                      cmd_touch(arg);
        else if (streq(cmd, "mkdir") || streq(cmd, "md"))  cmd_mkdir(arg);
        else if (streq(cmd, "rmdir") || streq(cmd, "rd"))  cmd_rmdir(arg);
        else if (streq(cmd, "rm") || streq(cmd, "del"))    cmd_rm(arg);
        else if (streq(cmd, "cp") || streq(cmd, "copy"))   cmd_cp(arg);
        else if (streq(cmd, "mv") || streq(cmd, "move") ||
                 streq(cmd, "rename"))                     cmd_mv(arg);
        else if (streq(cmd, "echo"))                       cmd_echo(arg);
        else if (streq(cmd, "edit") || streq(cmd, "nano")) cmd_edit(arg);
        else if (streq(cmd, "fastfetch") || streq(cmd, "ff")) cmd_fastfetch();
        else if (streq(cmd, "heaptest"))                   cmd_heaptest();
        else if (streq(cmd, "irqinfo"))                    cmd_irqinfo();
        else if (streq(cmd, "panic-test"))                 cmd_panic_test(arg);
        else if (streq(cmd, "doom"))                       doom_run(FB, TOTAL_RAM_BYTES);
        else {
            console_set_colors(COL_WARN, COL_BG);
            console_puts("Unknown command: ");
            console_puts(cmd);
            console_puts("\n");
            console_set_colors(COL_TEXT, COL_BG);
            console_puts("Type 'help' for the list of commands.\n");
        }
    }
}
