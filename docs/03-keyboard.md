# Keyboard input

## Hardware path

The keyboard driver reads the legacy PS/2 controller ports `0x64` and `0x60`. On the target laptop, firmware USB Legacy Emulation exposes the built-in USB keyboard as a PS/2 scancode set 1 device.

The driver uses polling and does not require an IDT. `kbd_getchar()` blocks until a decoded key is available. `kbd_poll()` performs one non-blocking decode step and is used only where the caller must continue other work.

## Scancode decoding

`kernel/keyboard.c` handles:

- printable US-layout keys
- Shift, Caps Lock, and Ctrl
- Enter, Backspace, Tab, and Escape
- arrows, Home, End, Page Up, Page Down, and Delete
- `0xE0` extended make and break sequences
- Ctrl+letter control codes used by the editor
- `F12` (scancode `0x58`, non-extended) as a global screenshot hotkey

Break codes immediately clear held-key state. They must not be ignored or replaced by timeout-only release detection, because doing so loses fast repeated taps and duplicate letters.

## TSC frequency

Software repeat timing is based on TSC ticks per millisecond. Detection uses this order:

1. CPUID leaf `0x15`
2. PIT channel 2 calibration
3. a conservative 1.1 GHz fallback

Apollo Lake normally reports a TSC frequency near 1094 MHz through CPUID. Correct frequency detection is required to prevent stuck or over-repeating keys.

## Software repeat

Firmware typematic settings are not reliable through every USB legacy implementation. The driver therefore tracks the current make code and emits software repeats while no new event is waiting.

Current timing:

- initial delay: approximately 300 ms
- repeat interval: approximately 35 ms
- lost-break fallback: approximately 200 ms

The break code remains the primary release signal. The timeout is only a safety fallback.

## Blocking input in the editor

The full-screen editor uses `kbd_getchar()`, matching the shell input path. An earlier non-blocking loop performed repeated `io_wait()` calls through port `0x80`; those writes caused severe latency on the physical Acer system. Port `0x80` delays must not be reintroduced into keyboard, editor, or rendering loops.

## Background service polling

Each decode pass also services xHCI event draining and background HDA playback. This keeps storage controller event rings and audio queues progressing while the shell or editor waits for keyboard input.

## Global F12 screenshot hotkey

`F12` decodes to `KEY_F12` the same way arrows and Page Up/Down decode to their
synthetic key codes, and follows the same software-repeat state machine so
holding it does not spam captures. The shell prompt, the text pager, the
image/photo gallery, and the full-screen editor all check for `KEY_F12` in
their own input loop and call `capture_screenshot_auto()` before continuing,
so the hotkey works everywhere without interrupting whatever is on screen.
Captures are silent and auto-numbered (`shot1.bmp`, `shot2.bmp`, ...) via
`fat_exists()` so repeated presses never overwrite a previous screenshot.

## Validation checklist

- A short key press produces exactly one character.
- Two quick presses of the same key produce two characters.
- Releasing a key stops repeat immediately.
- Holding a key starts after the configured delay and repeats smoothly.
- Extended key releases clear held state.
- Shell and editor input behave consistently on physical hardware.
