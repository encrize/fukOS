# Images and Doom

## Image decoding

`kernel/image.c` supports:

- uncompressed 24-bit BMP;
- uncompressed 32-bit BMP;
- the compact legacy IMG1 raw format.

BMP parsing validates signature, DIB fields, dimensions, compression, pixel offset, row stride, and output capacity. BMP rows are converted to top-down BGRA pixels.

PNG and JPEG are intentionally not supported in this version.

## Photo viewer

The `img`, `photo`, `show`, `image`, and `open` commands use the shared pixel buffer to decode and display images.

The viewer starts in a clean borderless full-screen mode:

- `n` / Space: next image;
- `p`: previous image;
- `m`: toggle fit/fill crop mode;
- `i`: toggle information/zoom interface;
- `q`, Escape, or Enter: return to shell.

## Shell screenshots

The shell has a BMP screenshot command:

```text
screenshot [name.bmp]
```

It writes an uncompressed 24-bit BMP from the RAM shadow framebuffer where possible. This avoids slow reads from the physical framebuffer aperture.

## Wallpaper

`fuko.conf` can name a BMP wallpaper. The image is decoded once, scaled to the active display size, and stored in console wallpaper memory. Text cell backgrounds use a pre-blended terminal background buffer controlled by `TERMINAL_TRANSPARENCY`.

## Doom integration

The `doom/` directory contains a Doom Generic-style port linked into the kernel. DOOM is not a process; it runs in the kernel address space.

The platform layer maps:

- frame presentation to the framebuffer API;
- keyboard polling to direct PS/2 scancode reading;
- timing to PIT/TSC-style helpers;
- memory allocation to a private arena from the kernel heap;
- exit handling through a small setjmp/longjmp bridge back to shell.

## DOOM and background music

The shell/background audio path normally advances from keyboard decoding. DOOM reads keyboard input directly, so its platform loop now explicitly calls:

```c
xhci_idle_drain();
hda_bg_poll();
```

This keeps `bgplay` music running while DOOM is active. No port `0x80` delay is used.

## DOOM repeat launches

Before every launch, `doom_reset_state()` restores linker-isolated Doom initialized data and clears Doom BSS. This prevents stale state from breaking the second and later DOOM runs.

DOOM then reserves a 32 MiB arena from the kernel heap and releases it when returning to shell.

## DOOM save files on FAT

DOOM save/load now uses real FAT files through the freestanding libc bridge in `doom/dg_libc.c`.

Files such as:

```text
doomsav0.dsg
doomsav1.dsg
temp.dsg
```

are written to the mounted FAT volume. After reboot, DOOM can open the saved `.dsg` files from the Load Game menu.

The original DOOM flow of writing `temp.dsg` and renaming it to the selected slot is preserved where possible, reducing the risk of destroying an older save if a write fails.

## Current DOOM file-path limitation

The current bridge stores save files by basename in the mounted FAT current/root directory. It does not yet create a nested `.savegame/doom1.wad/` directory tree. This keeps the implementation simple and avoids interfering with shell current-directory state.

## Licensing

See the main `README.md` notice for DOOM shareware data licensing.
