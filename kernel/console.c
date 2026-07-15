#include "console.h"
#include "framebuffer.h"
#include "util.h"

#define MARGIN  6
#define GLYPH_W FB_GLYPH_W
#define LINE_H  (FB_GLYPH_H + 2)
#define SCROLL_LINES 8

/* Draw in RAM and copy outward; framebuffer reads are intentionally avoided. */
#define SHADOW_MAX (16u * 1024u * 1024u)
static uint8_t shadow_mem[SHADOW_MAX];
static uint8_t wallpaper_mem[SHADOW_MAX];
static uint8_t terminal_bg_mem[SHADOW_MAX];
static fb_info shadow_fb;
static fb_info wallpaper_fb;
static fb_info terminal_bg_fb;
static int wallpaper_enabled;
static uint32_t terminal_transparency;

static const fb_info *C;
static const fb_info *D;
static int   col, row, cols, rows;
static uint32_t fg = 0xD8E0F0;
static uint32_t bg = 0x0B0F1A;
static int input_active, input_row, input_col, input_drawn;

#define CELL_MAX_COLS 256
#define CELL_MAX_ROWS 128
static uint32_t cell_cp[CELL_MAX_COLS * CELL_MAX_ROWS];
static uint32_t cell_color[CELL_MAX_COLS * CELL_MAX_ROWS];
#define SCROLLBACK_ROWS 1024
static uint32_t history_cp[SCROLLBACK_ROWS * CELL_MAX_COLS];
static uint32_t history_color[SCROLLBACK_ROWS * CELL_MAX_COLS];
static uint32_t history_head, history_count, scrollback_offset;

static uint32_t cell_index(int r, int c) {
    return (uint32_t)r * CELL_MAX_COLS + (uint32_t)c;
}

static void clear_cells(void) {
    for (uint32_t i = 0; i < CELL_MAX_COLS * CELL_MAX_ROWS; i++) {
        cell_cp[i] = 0; cell_color[i] = 0;
    }
}

static void clear_history(void) {
    history_head = history_count = scrollback_offset = 0;
}

static void push_history_row(int src_row) {
    if (src_row < 0 || src_row >= CELL_MAX_ROWS) return;
    uint32_t dst_row = history_head;
    for (int cc = 0; cc < CELL_MAX_COLS; cc++) {
        history_cp[dst_row * CELL_MAX_COLS + (uint32_t)cc] =
            cell_cp[cell_index(src_row, cc)];
        history_color[dst_row * CELL_MAX_COLS + (uint32_t)cc] =
            cell_color[cell_index(src_row, cc)];
    }
    history_head = (history_head + 1u) % SCROLLBACK_ROWS;
    if (history_count < SCROLLBACK_ROWS) history_count++;
}

static uint32_t history_index(uint32_t oldest_offset, int colnum) {
    uint32_t oldest = (history_head + SCROLLBACK_ROWS - history_count) % SCROLLBACK_ROWS;
    uint32_t row_index = (oldest + oldest_offset) % SCROLLBACK_ROWS;
    return row_index * CELL_MAX_COLS + (uint32_t)colnum;
}

static void present_rect(int x, int y, int w, int h) {
    if (D == C) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)C->width)  w = (int)C->width  - x;
    if (y + h > (int)C->height) h = (int)C->height - y;
    if (w <= 0 || h <= 0) return;
    uint32_t bypp = C->bpp / 8;
    uint32_t rowbytes = (uint32_t)w * bypp;
    for (int yy = 0; yy < h; yy++) {
        memcpy(C->addr + (uint32_t)(y + yy) * C->pitch + (uint32_t)x * bypp,
               D->addr + (uint32_t)(y + yy) * D->pitch + (uint32_t)x * bypp,
               rowbytes);
    }
}

static void present_all(void) {
    present_rect(0, 0, (int)C->width, (int)C->height);
}

static void restore_all(void) {
    if (wallpaper_enabled)
        memcpy(D->addr, wallpaper_fb.addr, D->pitch * D->height);
    else
        fb_clear(D, bg);
}

static uint32_t blend_color(uint32_t back, uint32_t front, uint32_t back_pct) {
    uint32_t front_pct = 100u - back_pct;
    uint32_t br = (back >> 16) & 255u, bgc = (back >> 8) & 255u, bb = back & 255u;
    uint32_t fr = (front >> 16) & 255u, fg2 = (front >> 8) & 255u, fb = front & 255u;
    uint32_t r = (br * back_pct + fr * front_pct + 50u) / 100u;
    uint32_t g = (bgc * back_pct + fg2 * front_pct + 50u) / 100u;
    uint32_t b = (bb * back_pct + fb * front_pct + 50u) / 100u;
    return (r << 16) | (g << 8) | b;
}

static void rebuild_terminal_background(void) {
    if (!wallpaper_enabled) return;
    terminal_bg_fb = wallpaper_fb;
    terminal_bg_fb.addr = terminal_bg_mem;
    if (wallpaper_fb.bpp == 32u) {
        for (uint32_t y = 0; y < wallpaper_fb.height; y++) {
            const uint32_t *src = (const uint32_t *)(const void *)(wallpaper_fb.addr + y * wallpaper_fb.pitch);
            uint32_t *dst = (uint32_t *)(void *)(terminal_bg_fb.addr + y * terminal_bg_fb.pitch);
            for (uint32_t x = 0; x < wallpaper_fb.width; x++)
                dst[x] = blend_color(src[x], bg, terminal_transparency);
        }
    } else fb_clear(&terminal_bg_fb, bg);
}

static void paint_text_background(int x, int y, int w, int h) {
    if (!wallpaper_enabled) { fb_fill_rect(D, x, y, w, h, bg); return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)D->width) w = (int)D->width - x;
    if (y + h > (int)D->height) h = (int)D->height - y;
    if (w <= 0 || h <= 0) return;
    uint32_t bypp = D->bpp / 8u, bytes = (uint32_t)w * bypp;
    for (int yy = 0; yy < h; yy++)
        memcpy(D->addr + (uint32_t)(y + yy) * D->pitch + (uint32_t)x * bypp,
               terminal_bg_fb.addr + (uint32_t)(y + yy) * terminal_bg_fb.pitch + (uint32_t)x * bypp,
               bytes);
}

static void redraw_cells(void) {
    restore_all();
    int rrmax = rows < CELL_MAX_ROWS ? rows : CELL_MAX_ROWS;
    int ccmax = cols < CELL_MAX_COLS ? cols : CELL_MAX_COLS;
    int64_t view_start = (int64_t)history_count - (int64_t)scrollback_offset;
    for (int rr = 0; rr < rrmax; rr++) {
        int64_t seq = view_start + rr;
        for (int cc = 0; cc < ccmax; cc++) {
            uint32_t cp = 0, color = 0;
            if (seq >= 0 && seq < (int64_t)history_count) {
                uint32_t i = history_index((uint32_t)seq, cc);
                cp = history_cp[i]; color = history_color[i];
            } else if (seq >= (int64_t)history_count) {
                int cr = (int)(seq - (int64_t)history_count);
                if (cr >= 0 && cr < rrmax) {
                    uint32_t i = cell_index(cr, cc);
                    cp = cell_cp[i]; color = cell_color[i];
                }
            }
            if (!cp) continue;
            int x = MARGIN + cc * GLYPH_W, y = MARGIN + rr * LINE_H;
            paint_text_background(x, y, GLYPH_W, LINE_H);
            fb_draw_glyph(D, x, y, cp, color);
        }
    }
    present_all();
}

static void erase_cell(void) {
    int x = MARGIN + col * GLYPH_W, y = MARGIN + row * LINE_H;
    if (row >= 0 && row < CELL_MAX_ROWS && col >= 0 && col < CELL_MAX_COLS) {
        uint32_t i = cell_index(row, col); cell_cp[i] = 32u; cell_color[i] = fg;
    }
    paint_text_background(x, y, GLYPH_W, LINE_H);
    present_rect(x, y, GLYPH_W, LINE_H);
}

static void scroll(int lines) {
    if (lines < 1) lines = 1;
    if (lines > rows) lines = rows;
    int rrmax = rows < CELL_MAX_ROWS ? rows : CELL_MAX_ROWS;
    int ccmax = cols < CELL_MAX_COLS ? cols : CELL_MAX_COLS;
    for (int rr = 0; rr < lines && rr < rrmax; rr++) push_history_row(rr);
    scrollback_offset = 0;
    for (int rr = 0; rr < rrmax - lines; rr++) {
        for (int cc = 0; cc < ccmax; cc++) {
            uint32_t dst = cell_index(rr, cc);
            uint32_t src = cell_index(rr + lines, cc);
            cell_cp[dst] = cell_cp[src];
            cell_color[dst] = cell_color[src];
        }
    }
    for (int rr = rrmax - lines; rr < rrmax; rr++) {
        if (rr < 0) continue;
        for (int cc = 0; cc < ccmax; cc++) {
            uint32_t i = cell_index(rr, cc); cell_cp[i] = 0; cell_color[i] = 0;
        }
    }

    redraw_cells();
}

static void newline(void) {
    col = 0;
    row++;
    if (row >= rows) {
        scroll(SCROLL_LINES);
        row -= SCROLL_LINES;
        if (row < 0) row = 0;
    }
}

void console_init(const fb_info *fb) {
    C  = fb;
    fg = 0xD8E0F0;
    bg = 0x0B0F1A;
    cols = ((int)fb->width  - 2 * MARGIN) / GLYPH_W;
    rows = ((int)fb->height - 2 * MARGIN) / LINE_H;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > CELL_MAX_COLS) cols = CELL_MAX_COLS;
    if (rows > CELL_MAX_ROWS) rows = CELL_MAX_ROWS;
    col = 0;
    row = 0;
    clear_cells();
    clear_history();

    if ((uint32_t)fb->pitch * fb->height <= SHADOW_MAX) {
        shadow_fb = *fb;
        shadow_fb.addr = shadow_mem;
        D = &shadow_fb;
    } else {
        D = C;
    }
    if (wallpaper_enabled && (wallpaper_fb.width != fb->width ||
        wallpaper_fb.height != fb->height || wallpaper_fb.bpp != fb->bpp))
        wallpaper_enabled = 0;
    restore_all();
    present_all();
}

int console_set_wallpaper(const uint8_t *pixels, uint32_t width, uint32_t height) {
    if (!C || !D || !pixels || !width || !height) return 0;
    uint32_t bytes = C->pitch * C->height;
    if (bytes > SHADOW_MAX) return 0;
    wallpaper_fb = *C;
    wallpaper_fb.addr = wallpaper_mem;
    fb_clear(&wallpaper_fb, bg);
    fb_blit_bgra_scaled(&wallpaper_fb, 0, 0, pixels, width, height,
                        C->width, C->height, 0, 0, C->width, C->height);
    wallpaper_enabled = 1;
    rebuild_terminal_background();
    clear_cells(); clear_history(); restore_all(); present_all(); col = 0; row = 0;
    return 1;
}

void console_disable_wallpaper(void) {
    wallpaper_enabled = 0;
    clear_cells(); clear_history(); fb_clear(D, bg); present_all(); col = 0; row = 0;
}

void console_set_colors(uint32_t f, uint32_t b) {
    fg = f;
    if (bg != b) { bg = b; rebuild_terminal_background(); }
}

void console_set_foreground(uint32_t f) { fg = f; }

int console_set_cursor(int column, int at_row) {
    if (column < 0 || column >= cols || at_row < 0 || at_row >= rows) return 0;
    console_scrollback_live();
    input_active = 0;
    col = column;
    row = at_row;
    return 1;
}

int console_columns(void) { return cols; }
int console_rows(void) { return rows; }

void console_set_transparency(uint32_t percent) {
    if (percent > 100u) percent = 100u;
    terminal_transparency = percent;
    rebuild_terminal_background();
    if (C && D) redraw_cells();
}

uint32_t console_get_transparency(void) { return terminal_transparency; }

void console_clear(void) {
    clear_cells();
    clear_history();
    restore_all();
    present_all();
    col = 0;
    row = 0;
    input_active = 0;
}

static void draw_input_cell(int offset, uint32_t cp, int cursor) {
    int cell = input_col + offset;
    int at_row = input_row + cell / cols;
    int at_col = cell % cols;
    int x = MARGIN + at_col * GLYPH_W;
    int y = MARGIN + at_row * LINE_H;
    uint32_t i = cell_index(at_row, at_col);

    cell_cp[i] = cp;
    cell_color[i] = fg;
    if (cursor) {
        fb_fill_rect(D, x, y, GLYPH_W, LINE_H, fg);
        if (cp != ' ') fb_draw_glyph(D, x, y, cp, bg);
    } else {
        paint_text_background(x, y, GLYPH_W, LINE_H);
        if (cp != ' ') fb_draw_glyph(D, x, y, cp, fg);
    }
    present_rect(x, y, GLYPH_W, LINE_H);
}

void console_input_begin(void) {
    console_scrollback_live();
    input_active = 1;
    input_row = row;
    input_col = col;
    input_drawn = 0;
}

void console_input_redraw(const char *text, int length, int cursor, int show_cursor) {
    if (!input_active) console_input_begin();
    if (scrollback_offset) {
        scrollback_offset = 0;
        redraw_cells();
    }
    if (length < 0) length = 0;
    if (cursor < 0) cursor = 0;
    if (cursor > length) cursor = length;

    int last = input_drawn > length ? input_drawn : length;
    if (cursor > last) last = cursor;
    int last_row = input_row + (input_col + last) / cols;
    if (last_row >= rows) {
        int shift = last_row - rows + 1;
        scroll(shift);
        input_row -= shift;
        if (input_row < 0) input_row = 0;
    }

    for (int i = 0; i <= last; i++) {
        uint32_t cp = i < length ? (uint8_t)text[i] : (uint32_t)' ';
        draw_input_cell(i, cp, show_cursor && i == cursor);
    }
    input_drawn = length;

    int end = input_col + length;
    if (length > 0 && end % cols == 0) {
        row = input_row + end / cols - 1;
        col = cols;
    } else {
        row = input_row + end / cols;
        col = end % cols;
    }
}

void console_input_end(void) {
    input_active = 0;
}

int console_row(void) { return row; }

int console_reserve_rows(int n) {
    if (n < 1) n = 1;
    if (n > rows) n = rows;
    if (row + n > rows) {
        int shift = row + n - rows;
        scroll(shift);
        row -= shift;
        if (row < 0) row = 0;
    }
    int start = row;
    row += n;
    if (row > rows) row = rows;
    col = 0;
    return start;
}

void console_draw_at(int at_row, int at_col, const char *s, uint32_t color) {
    if (scrollback_offset) { scrollback_offset = 0; redraw_cells(); }
    int x0 = MARGIN + at_col * GLYPH_W;
    int y  = MARGIN + at_row * LINE_H;
    int x  = x0;
    while (*s) {
        uint8_t b = (uint8_t)*s;
        uint32_t cp; int adv;
        if (b >= 0xF0)      { cp = (uint32_t)(b & 0x07); adv = 4; }
        else if (b >= 0xE0) { cp = (uint32_t)(b & 0x0F); adv = 3; }
        else if (b >= 0xC0) { cp = (uint32_t)(b & 0x1F); adv = 2; }
        else                { cp = b; adv = 1; }
        for (int j = 1; j < adv; j++) {
            uint8_t cc = (uint8_t)s[j];
            if (!cc || (cc & 0xC0) != 0x80) { adv = j; break; }
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        int atc = (x - MARGIN) / GLYPH_W;
        if (at_row >= 0 && at_row < CELL_MAX_ROWS && atc >= 0 && atc < CELL_MAX_COLS) {
            uint32_t i = cell_index(at_row, atc); cell_cp[i] = cp; cell_color[i] = color;
        }
        paint_text_background(x, y, GLYPH_W, LINE_H);
        fb_draw_glyph(D, x, y, cp, color);
        x += GLYPH_W;
        s += adv;
    }
    present_rect(x0, y, x - x0, LINE_H);
}

static uint32_t utf_cp  = 0;
static int      utf_rem = 0;

static void put_cp(uint32_t cp) {
    if (scrollback_offset) { scrollback_offset = 0; redraw_cells(); }
    if (col >= cols) newline();
    int x = MARGIN + col * GLYPH_W, y = MARGIN + row * LINE_H;
    if (row >= 0 && row < CELL_MAX_ROWS && col >= 0 && col < CELL_MAX_COLS) {
        uint32_t i = cell_index(row, col); cell_cp[i] = cp; cell_color[i] = fg;
    }
    paint_text_background(x, y, GLYPH_W, LINE_H);
    fb_draw_glyph(D, x, y, cp, fg);
    present_rect(x, y, GLYPH_W, LINE_H);
    col++;
}

void console_putc(char c) {
    uint8_t b = (uint8_t)c;

    if (utf_rem) {
        if ((b & 0xC0) == 0x80) {
            utf_cp = (utf_cp << 6) | (uint32_t)(b & 0x3F);
            if (--utf_rem == 0) put_cp(utf_cp);
            return;
        }
        utf_rem = 0;
    }

    if (b >= 0xF0) { utf_cp = (uint32_t)(b & 0x07); utf_rem = 3; return; }
    if (b >= 0xE0) { utf_cp = (uint32_t)(b & 0x0F); utf_rem = 2; return; }
    if (b >= 0xC0) { utf_cp = (uint32_t)(b & 0x1F); utf_rem = 1; return; }

    if (b == '\n') { newline(); return; }
    if (b == '\r') { col = 0; return; }
    if (b == '\t') { console_puts("    "); return; }
    if (b == '\b') {
        if (col > 0) col--;
        else if (row > 0) { row--; col = cols - 1; }
        erase_cell();
        return;
    }
    put_cp((uint32_t)b);
}

int console_scrollback_up(int lines) {
    if (lines < 1) lines = 1;
    uint32_t next = scrollback_offset + (uint32_t)lines;
    if (next > history_count) next = history_count;
    scrollback_offset = next;
    redraw_cells();
    return (int)scrollback_offset;
}

int console_scrollback_down(int lines) {
    if (lines < 1) lines = 1;
    if ((uint32_t)lines >= scrollback_offset) scrollback_offset = 0;
    else scrollback_offset -= (uint32_t)lines;
    redraw_cells();
    return (int)scrollback_offset;
}

void console_scrollback_live(void) {
    if (scrollback_offset) { scrollback_offset = 0; redraw_cells(); }
}

const fb_info *console_snapshot_framebuffer(void) {
    return D;
}

void console_puts(const char *s) {
    for (; *s; s++) console_putc(*s);
}
