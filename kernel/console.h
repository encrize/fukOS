#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>
#include "framebuffer.h"

/* Framebuffer text output. */
void console_init(const fb_info *fb);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *s);
void console_set_colors(uint32_t fg, uint32_t bg);
void console_set_foreground(uint32_t fg);
/* Position the next text output at zero-based column/row coordinates. */
int  console_set_cursor(int column, int row);
int  console_columns(void);
int  console_rows(void);

/* Wallpaper and terminal background. */
int console_set_wallpaper(const uint8_t *pixels, uint32_t width, uint32_t height);
void console_disable_wallpaper(void);

void console_set_transparency(uint32_t percent);
uint32_t console_get_transparency(void);

/* Editable shell input with wrapping and a block cursor. */
void console_input_begin(void);
void console_input_redraw(const char *text, int length, int cursor, int show_cursor);
void console_input_end(void);

int console_row(void);

int console_reserve_rows(int n);

void console_draw_at(int row, int col, const char *s, uint32_t color);

/* Scrollback navigation. */
int console_scrollback_up(int lines);
int console_scrollback_down(int lines);
void console_scrollback_live(void);

/* Returns the current console image in ordinary RAM. */
const fb_info *console_snapshot_framebuffer(void);

#endif
