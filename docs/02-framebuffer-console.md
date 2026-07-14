# Framebuffer and console

## Framebuffer interface

`kernel/framebuffer.c` implements pixel fills, glyph drawing, string drawing, image blits, and scaling. The active mode is supplied by Limine. Rendering code uses the actual width, height, pitch, and bits per pixel; it must not assume 800×600 or tightly packed scanlines.

The primary hardware mode is 1366×768. All coordinate and clipping logic must remain valid for that non-round width.

## Write combining

`fb_enable_write_combining()` configures an available variable MTRR for the linear framebuffer when supported. This improves sequential writes, but framebuffer reads may still be expensive. Rendering paths therefore treat the visible framebuffer as a write-only destination.

## Shadow framebuffer

The console draws into a RAM shadow when the framebuffer fits within `SHADOW_MAX`. Updates are copied to the visible framebuffer with bounded write-only rectangles. Full-screen redraws use `present_all()`; cursor and character updates use small rectangles.

If the display is too large for the static shadow, rendering falls back to the real framebuffer. Common paths should still avoid unnecessary full-screen work in that mode.

## Logical text cells

The console keeps code points and colors in a logical cell grid. This allows the screen to be reconstructed over a fixed wallpaper without moving or duplicating wallpaper pixels during scrolling.

The cell model also supports:

- UTF-8 decoding for output
- line wrapping
- destructive backspace
- arbitrary-position drawing for layouts
- 1024 rows of scrollback
- editable shell input with a block cursor

## Wallpaper and transparency

A wallpaper is decoded once and scaled to the current framebuffer. `terminal_bg_mem` stores a pre-blended background for text cells. Changing transparency rebuilds this buffer once; normal glyph updates copy the prepared background rather than blending every frame.

## Scrollback

When live rows scroll off the screen, they are copied into a ring buffer. Shift+Up and Shift+Down move by one row; Page Up and Page Down move by a larger step. Returning to live output reconstructs the visible cell grid over the fixed wallpaper.

## Shell input rendering

The shell line editor records the row and column where input begins. `console_input_redraw()` redraws the command, clears stale trailing cells, handles wrapping, and paints the current cell with inverted colors. If wrapped input reaches the last screen row, the console scrolls first and adjusts the input anchor.

## Performance rules

- Never read visible framebuffer pixels in a hot path.
- Present only the modified rectangle when possible.
- Keep wallpaper compositing out of per-key processing.
- Do not insert port-I/O delays between keyboard polls.
- Preserve pitch-aware copying for all framebuffer modes.
