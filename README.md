# fukOS

![License](https://img.shields.io/github/license/encrize/fukOS)
![Last commit](https://img.shields.io/github/last-commit/encrize/fukOS)
![Architecture](https://img.shields.io/badge/arch-i686-blue)
![Bootloader](https://img.shields.io/badge/boot-Limine%2FMultiboot2-lightgrey)

fukOS is a hobby 32-bit i686 operating system written in freestanding C and assembly. It boots through Limine/Multiboot2, uses its own framebuffer console, storage, keyboard, audio, FAT layer, shell, text editor, and a Doom Generic port.

The main hardware target is **Acer Aspire ES1-533** with Intel Pentium N4200 / Apollo Lake and a 1366×768 display.

## Contents

- [Screenshots](#screenshots)
- [Current feature set](#current-feature-set)
- [Building on Arch Linux](#building-on-arch-linux)
- [Typical hardware test flow](#typical-hardware-test-flow)
- [Shell controls](#shell-controls)
- [Some shell commands](#some-shell-commands)
- [Text editor](#text-editor)
- [Memory and heap](#memory-and-heap)
- [DOOM integration](#doom-integration)
- [Repository layout](#repository-layout)
- [Current limitations](#current-limitations)
- [Development roadmap](#development-roadmap)
- [Third-party material](#third-party-material)

## Screenshots

| Shell | Clock app | Fastfetch |
|---|---|---|
| ![shell](https://github.com/user-attachments/assets/b9896c2b-f4c4-42cf-995b-aa1e6e3b08c6) | ![clock](https://github.com/user-attachments/assets/2dabb7e8-47d3-4e69-a4f6-37bbb8ced766) | ![fastfetch](https://github.com/user-attachments/assets/57ca773f-eabe-4a76-89a8-5b784ad6094a) |
| `ls`/`tree` output on real Acer hardware | Full-screen flip clock | Live system status card (`ff`) |

## Current feature set

- Limine Multiboot2 boot path.
- Linear framebuffer graphics with write-combining setup.
- RAM shadow framebuffer for console/editor rendering.
- Wallpaper and terminal transparency from `fuko.conf`.
- PS/2 scancode set 1 keyboard input via firmware USB Legacy Emulation.
- CPUID/PIT-based TSC frequency detection for software key repeat.
- Shell with editable command line, history, scrollback, and tab completion.
- Full-screen terminal `clock` app with large flip-clock-style hour/minute cards.
- Configurable BMP startup splash loaded from `fuko.conf` after storage mount.
- FAT16/FAT32 storage over ATA PIO or xHCI USB Mass Storage.
- File commands: `ls`, `cd`, `pwd`, `tree`, `cat`, `head`, `wc`, `hexdump`, `touch`, `mkdir`, `rmdir`, `rm`, `cp`, `mv`, `echo`.
- Full-screen `edit` editor with undo, search, tabs, partial redraw, and fast blocking input.
- BMP image/photo viewer with fit/fill modes.
- Intel HDA output with foreground WAV playback and background `bgplay` playlists.
- Doom Generic port linked into the kernel.
- Background music continues while playing DOOM.
- Physical-memory kernel heap initialized from the Multiboot2 memory map.
- `heaptest` command for heap allocation tests.
- IDT exception handling, remapped 8259 PIC, and a 100 Hz PIT timer IRQ.
- Kernel panic screen with register, exception, error-code, and CR2 dump.
- COM1 serial logging for boot diagnostics and panic reports.
- DOOM save files are written as real FAT files and can be loaded after reboot.
- ACPI shutdown and keyboard-controller reboot fallback.

## Building on Arch Linux

Install the build utilities and runtime dependencies:

```sh
sudo pacman -Syu
sudo pacman -S --needed base-devel nasm python qemu-system-x86 limine
```

The supported build uses an `i686-elf` cross-toolchain. Install or build `i686-elf-gcc` and `i686-elf-binutils`, then verify that `i686-elf-gcc` and `i686-elf-ld` are in `PATH`.

Build the kernel:

```sh
make clean
make kernel.bin CROSS=i686-elf-
```

Build a bootable BIOS/UEFI disk image with the packaged Limine files:

```sh
make limine CROSS=i686-elf-
```

Run the image in QEMU using KVM when available, with TCG as a fallback:

```sh
make run-limine CROSS=i686-elf-
# or: ./run.sh
```

For UEFI testing, install `edk2-ovmf` and pass its firmware explicitly:

```sh
sudo pacman -S --needed edk2-ovmf
make run-limine CROSS=i686-elf- OVMF=/usr/share/edk2/x64/OVMF_CODE.fd
```

If Limine is unpacked locally rather than installed from the Arch package, set `LIMINE_DIR`:

```sh
make limine CROSS=i686-elf- LIMINE_DIR="$HOME/src/limine"
```

The generated image is `os-limine.img`. See [`docs/09-build-and-run.md`](docs/09-build-and-run.md) for dependency checks, QEMU options, and USB deployment.

## Typical hardware test flow

```text
ff
heaptest
bgplay *.wav
doom
```

Recommended DOOM save test:

1. Start DOOM.
2. Save to a slot.
3. Exit DOOM.
4. Run `ls` and verify a file such as `doomsav0.dsg` exists.
5. Reboot the Acer.
6. Start DOOM again and load the save from the menu.

## Shell controls

| Key | Action |
|---|---|
| Left / Right | Move through current command |
| Home / End | Move to beginning / end |
| Backspace / Delete | Delete before / under cursor |
| Up / Down | Browse command history |
| Shift+Up / Shift+Down | Scroll shell output one row |
| Page Up / Page Down | Scroll shell output one page |
| Tab | Complete command, file, or directory name |

## Some shell commands

```text
ff                 live system card with RAM/heap/disk/audio bars
heaptest           test kernel heap alloc/free/calloc/realloc behavior
irqinfo            show PIT interrupt ticks and COM1 status
panic-test confirm deliberately trigger a breakpoint panic and register dump
open <file>        open by type: text->edit, BMP->photo, WAV->play
bgplay <file.wav>  start background WAV playback
bgplay *.wav       queue all WAV files in current directory
doom               start DOOM
edit <file>        full-screen text editor
photo [file]       full-screen BMP gallery/viewer
```

## Text editor

```text
edit <filename>
```

The editor supports arrows, Home, End, Page Up, Page Down, Backspace, Delete, Tab, Enter, `Ctrl+F` search, `Ctrl+Z` undo, `Ctrl+K` delete line, `Esc`/`Ctrl+X` save and exit, and `Ctrl+Q` quit without saving.

The editor waits through `kbd_getchar()`. Do not reintroduce non-blocking idle loops with repeated `io_wait()` or port `0x80` writes; that caused severe input latency on the real Acer.

## Memory and heap

The kernel still uses many explicit static buffers for predictable bare-metal behavior, but it now also initializes a simple physical-memory heap from the largest suitable usable Multiboot2 memory-map range above the kernel image.

Heap implementation:

- first-fit free list;
- 16-byte alignment;
- block splitting;
- adjacent free-block coalescing;
- `kheap_alloc`, `kheap_calloc`, `kheap_realloc`, `kheap_free`;
- `kheap_get_info` for statistics;
- `heaptest` for runtime validation.

DOOM reserves a private 32 MiB arena from this kernel heap for each run and frees it on exit.

## DOOM integration

The `doom/` directory contains a Doom Generic-style port linked directly into the kernel. DOOM shares the kernel address space and is not a separate process.

Important implementation points:

- DOOM global state is restored before each launch using linker-isolated data/BSS ranges.
- DOOM uses a private heap arena allocated from the kernel heap.
- DOOM polls keyboard directly, so its platform loop explicitly services `hda_bg_poll()` and `xhci_idle_drain()`.
- This keeps shell-started `bgplay` music running while DOOM is active.
- DOOM save files are bridged through the freestanding libc to FAT and persist across reboot.

## Repository layout

```text
boot/          Multiboot2 entry code
bootloader/    legacy custom loader sources
kernel/        kernel, drivers, shell, editor, heap
doom/          Doom Generic port and embedded shareware IWAD
tools/         Limine FAT image builder
docs/          technical documentation
Makefile       kernel and image build rules
limine.conf    Limine boot configuration
```

## Current limitations

- 32-bit i686 only; no SMP or 64-bit mode.
- Single address space, ring 0 only, and no process isolation.
- No paging, virtual memory, memory protection, or swap.
- No scheduler, processes, threads, signals, or executable loader.
- Keyboard, xHCI, and HDA are still serviced cooperatively; only the PIT uses an IRQ.
- Storage support is limited to FAT16/FAT32 over ATA PIO or one xHCI USB Mass Storage device.
- No journaling, permissions, users, networking, USB HID driver, or package manager.
- Hardware-specific xHCI and Intel HDA support remains incomplete outside the target laptop.
- Audio is limited to uncompressed PCM WAV; images are limited to BMP and IMG1.
- RTC time has no configured timezone, NTP synchronization, or century-register handling.
- The kernel has no automated bare-metal regression suite; real-hardware checks remain manual.

## Development roadmap

### Near-term - architecture debt
1. Move PS/2, xHCI, and HDA toward IRQ-driven operation with synchronization primitives.
2. Create QEMU smoke tests for boot, heap, FAT, exceptions, screenshots, and repeated DOOM launches.

### Mid-term - isolation and safety
3. Add paging with guard pages, a higher-half kernel, and a safer physical-page allocator.
4. Introduce a preemptive scheduler, ring-3 processes, syscalls, and an ELF loader.
5. Add a VFS layer, read-only ISO9660/ext2 support, and transactional FAT write recovery.
6. Implement USB HID keyboards/mice without relying on firmware legacy emulation.

### Long-term - userspace and beyond
7. Build a userspace terminal, file manager, system monitor, and settings application.
8. Add PNG/JPEG decoding, scalable fonts, mouse input, and window compositing.
9. Add an RTL8139 or Intel e1000 driver, IPv4, DHCP, DNS, ICMP, and a small TCP stack.
10. Add ACPI table parsing for power, battery, thermal state, and reliable reboot/shutdown.

## Third-party material

This repository includes the shareware data file `doom1.wad` for demonstration purposes only.

DOOM Shareware data is copyrighted by id Software / ZeniMax Media /
Microsoft. It is not covered by this project's GPL-2.0 license and
remains the property of its respective copyright holders.

This is a non-commercial, non-profit educational project. No copyright
infringement is intended.

If you are the copyright holder and wish this file to be removed from
the repository, please contact: admin@encrize.vip
