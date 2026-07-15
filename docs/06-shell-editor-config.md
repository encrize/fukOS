# Shell, editor, and configuration

## Shell loop

`kernel/shell.c` initializes storage, configuration, console, keyboard, and command dispatch. The shell uses blocking keyboard input and does not insert artificial delay loops.

## Command line

The shell line editor supports:

- insertion at cursor;
- Left/Right, Home/End;
- Backspace/Delete;
- Up/Down history;
- draft restoration after history browsing;
- wrapped rendering;
- Tab completion for commands and current-directory entries.

## File commands

Supported commands include:

```text
ls / dir
cd
pwd
tree
cat / type
head
wc
hexdump
touch
mkdir / md
rmdir / rd
rm / del
cp / copy
mv / move / rename
echo
```

`mv` moves a directory entry and preserves the existing cluster chain; it can move files across directories when the destination includes a file name.

## System and diagnostic commands

```text
ff / fastfetch
heaptest
open <file>
matrix
res
lspci
usb / xhci
diskinfo
time / date
start <app>
about
reboot
poweroff / shutdown / off
```

`ff` shows CPU, display, storage, memory, and heap information. `heaptest` exercises the kernel heap and reports success/failure.

`start <app>` loads `/apps/<app>.fuk` from the mounted FAT volume at runtime. The application is not linked into the kernel. See [`12-external-fuk-apps.md`](12-external-fuk-apps.md) for the loader architecture and FUK1 programming guide.

## Audio commands

Foreground WAV playback:

```text
play <file.wav>
play *.wav
playlist
```

Background WAV playback:

```text
bgplay <file.wav>
bgplay *.wav
bgpause
bgresume
bgstop
bgstatus
bgvolume <0-100>
bgrepeat [on|off]
bgshuffle [on|off]
bgnext
bgprev
audioout [auto|speaker|headphones]
```

Background playback advances from `hda_bg_poll()`. The keyboard path, shell/editor waits, and DOOM platform loop service it cooperatively.

## Text editor

```text
edit <filename>
```

Supported operations:

- character insertion/deletion;
- arrows, Home, End, Page Up, Page Down;
- Tab insertion;
- `Ctrl+F` search;
- `Ctrl+K` delete line;
- `Ctrl+Z` undo;
- Escape or `Ctrl+X` save and exit;
- `Ctrl+Q` quit without saving.

The editor uses a RAM shadow framebuffer, incremental line tracking, and partial redraw. Ordinary typing redraws only the changed row area; cursor-only movement updates old and new cells.

## Critical input-latency rule

The editor must use `kbd_getchar()` and must not return to a non-blocking loop with repeated `io_wait()` calls. On the Acer Aspire ES1-533, port `0x80` writes caused severe latency even when the actual edit work was cheap.

## Configuration

At startup, `load_fuko_config()` reads `fuko.conf` from FAT root.

Supported keys:

```ini
BOOT_IMAGE = boot.bmp
BOOT_IMAGE_SECONDS = 3
BOOT_IMAGE_MODE = fit
WALLPAPER = photos/wallpaper.bmp
TERMINAL_TRANSPARENCY = 55
AUDIO_OUTPUT = auto
```

The boot image is shown after FAT storage is mounted and `fuko.conf` has been
read, immediately before the shell banner. `BOOT_IMAGE_SECONDS` accepts 0–30;
zero disables the splash. `BOOT_IMAGE_MODE` accepts `fit` or `fill`. Pressing
any key skips the splash. The image must be an uncompressed 24-bit or 32-bit
BMP that fits the 16 MiB decode buffer.

`AUDIO_OUTPUT` accepts `auto`, `speaker`, or `headphones`. Malformed and unknown lines are ignored. See `fuko.conf.example` in the repository root.

## Full-screen clock

`clock` opens a keyboard-controlled, full-screen flip-clock-style display. It
shows hours and minutes on two large cards with a steady colon. It does not
show or repaint seconds. The screen is redrawn only when the minute changes,
so there are no framebuffer writes once per second. Clock and boot-splash input polling do not depend on PIT IRQ delivery,
so firmware that does not route the legacy timer cannot freeze either screen.

Controls:

- `Q`, Escape, or Enter returns to the terminal;
- `F12` saves a screenshot;
- background HDA playback and xHCI event servicing continue while it runs.

## Open command

`open <file>` dispatches by extension:

- text/source/config files (`.txt`, `.md`, `.c`, `.h`, `.asm`, `.conf`, `.cfg`, `.ini`, `.log`) open in `edit`;
- `.bmp` and `.img` open in the image/photo viewer;
- `.wav` opens in foreground `play`;
- `.dsg` prints a short DOOM save description;
- unknown extensions fall back to `cat`.

## Matrix screensaver

`matrix` switches to a full-screen green digital-rain effect. It exits on `Q`, Escape, or Enter. The loop remains cooperative and services `hda_bg_poll()` and `xhci_idle_drain()` so background music and USB event handling continue.
