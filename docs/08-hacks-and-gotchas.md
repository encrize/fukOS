# Hardware notes and non-obvious constraints

## Port `0x80` is not harmless

Classic `io_wait()` often writes to port `0x80`. On the Acer Aspire ES1-533 this is very slow. A previous editor loop performed 40 such writes after each empty keyboard poll and made `edit` feel broken.

Do not add port `0x80` delays to input, rendering, storage, audio polling, or DOOM service loops.

## Editor input must remain blocking

`edit` should wait through `kbd_getchar()`, matching shell input. The working fast path is blocking input plus partial redraw, not non-blocking polling with artificial idle delays.

## Framebuffer reads are expensive

Treat the visible framebuffer as write-only in hot paths. Console/editor/photo rendering should operate from RAM shadows or decoded RAM buffers and then present outward.

## Partial rendering matters

One character, one cursor move, or one line scroll should not force a full-screen copy. Preserve the editor's row/cell-level update strategy.

## Break codes are authoritative

PS/2 break codes arrive correctly through the target firmware's USB Legacy Emulation. Clear held-key state immediately on release; timeout release is only a fallback.

## TSC calibration affects input

Incorrect TSC frequency causes stuck or over-fast software repeat. Prefer CPUID leaf `0x15`, then PIT calibration, then the conservative fallback.

## xHCI event-ring ownership

Acknowledge event-ring dequeue and Event Handler Busy state correctly. Incorrect handling can replay stale events or stop new events.

## Background services are still cooperative

The kernel has an IDT and a 100 Hz PIT interrupt, but keyboard, xHCI, and HDA remain polling/cooperative drivers. Their PIC lines stay masked until dedicated IRQ-safe handlers exist. Keyboard decoding and DOOM's platform layer must therefore continue calling the xHCI/HDA service functions.

## Heap is real but still low-level

The kernel heap is a simple physical first-fit allocator. It has no paging, no per-process isolation, and no memory protection. Use it deliberately and validate allocator changes with `heaptest`.

## HDA gain is codec-specific

Do not assume maximum amplifier gain is safe. Query capabilities and clamp gain to a tested range. Speaker and headphone paths may require different routing.

## Emulator differences

QEMU cannot be the final authority for keyboard latency, framebuffer speed, USB storage, HDA routing, or ACPI shutdown. Final validation belongs to the real Acer.
