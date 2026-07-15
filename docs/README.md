# fukOS technical documentation

This directory documents the current fukOS version: a freestanding 32-bit i686 hobby OS with Limine boot, framebuffer graphics, PS/2 keyboard input, FAT storage, xHCI USB Mass Storage, Intel HDA audio, a physical-memory kernel heap, and an in-kernel Doom Generic port.

The primary validation platform is Acer Aspire ES1-533 with Intel Pentium N4200. QEMU is useful for basic boot checks, but physical hardware remains the source of truth.

## Contents

| Document | Topic |
|---|---|
| [`01-architecture.md`](01-architecture.md) | Boot flow, memory model, heap, kernel layout, Doom state reset |
| [`02-framebuffer-console.md`](02-framebuffer-console.md) | Framebuffer rendering, shadow buffers, wallpaper, scrollback |
| [`03-keyboard.md`](03-keyboard.md) | PS/2 input, scancode decoding, TSC timing, key repeat |
| [`04-storage-drivers.md`](04-storage-drivers.md) | PCI, ATA PIO, xHCI, USB Mass Storage, FAT |
| [`05-audio-hda.md`](05-audio-hda.md) | Intel HDA discovery, routing, DMA, WAV playback, bgplay |
| [`06-shell-editor-config.md`](06-shell-editor-config.md) | Shell, command editing, text editor, config, heaptest |
| [`07-images-and-doom.md`](07-images-and-doom.md) | BMP images, photo viewer, Doom integration, Doom saves |
| [`08-hacks-and-gotchas.md`](08-hacks-and-gotchas.md) | Hardware constraints and non-obvious implementation rules |
| [`09-build-and-run.md`](09-build-and-run.md) | Toolchain, build targets, deployment, testing |
| [`10-interrupts-panic-serial.md`](10-interrupts-panic-serial.md) | IDT/PIC/PIT interrupts, panic register dump, COM1 logging |
| [`11-usb-layout.md`](11-usb-layout.md) | USB image layout, installation, and black-screen diagnosis |
| [`12-external-fuk-apps.md`](12-external-fuk-apps.md) | Runtime app loading from USB and the complete FUK1 language guide |
| [`13-fuk-programming-tutorial.md`](13-fuk-programming-tutorial.md) | Step-by-step FUK1 programming tutorial with game and Tetris examples |

## Design principles

- Prefer predictable memory ownership; use the heap deliberately, not invisibly.
- Keep hot input/rendering paths free of port `0x80` delay loops.
- Avoid reads from slow framebuffer memory; render in normal RAM and write outward.
- Keep hardware waits bounded unless the call is intentionally blocking.
- Preserve correctness on real Acer hardware even when QEMU behaves differently.
- Compile with `make kernel.bin CROSS=i686-elf-` and freestanding flags.
