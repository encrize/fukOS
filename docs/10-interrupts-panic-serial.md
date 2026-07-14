# Interrupts, kernel panic, and serial logging

## Interrupt architecture

The kernel installs a 256-entry IDT during early boot. Assembly stubs exist for CPU exceptions 0–31 and legacy PIC IRQs 0–15, remapped to vectors 32–47.

The common assembly entry saves:

- segment registers;
- `EAX`, `EBX`, `ECX`, and `EDX`;
- `ESI`, `EDI`, `EBP`, and enough stack state to reconstruct the interrupted `ESP`;
- vector and exception error code;
- `EIP`, `CS`, and `EFLAGS` pushed by the CPU.

The saved frame is passed to `interrupt_dispatch()` in `kernel/interrupts.c`. Exception vectors enter the panic path. Hardware vectors receive an end-of-interrupt command before returning through `iretd`.

## PIC and PIT policy

The two 8259 PICs are remapped to vectors 32–47. Only IRQ0 is currently unmasked. PIT channel 0 runs at approximately 100 Hz and increments the system tick counter exposed by `interrupts_ticks()`.

Keyboard, xHCI, and HDA continue using their existing polling/cooperative paths. Do not unmask their IRQ lines until each driver has an IRQ-safe handler that acknowledges the device before the PIC EOI.

The shell command below confirms that timer interrupts are advancing:

```text
irqinfo
```

It also reports whether COM1 was detected.

## Panic behavior

`kernel/panic.c` provides two fatal paths:

- `kernel_panic(message)` for explicit invariant failures;
- `panic_from_interrupt(frame)` for CPU exceptions.

A panic disables interrupts, switches the framebuffer to a dedicated diagnostic screen, prints the reason, and halts every subsequent CPU cycle with `cli; hlt`.

Exception panics display and log:

```text
vector  error code  CR2
EAX EBX ECX EDX
ESI EDI EBP ESP
EIP CS EFLAGS
DS ES FS GS
```

`CR2` is especially useful for page faults. Error codes are preserved exactly as supplied by the processor; exceptions without a hardware error code receive zero from their assembly stub.

To test the complete exception path, including the assembly stub and register dump:

```text
panic-test confirm
```

This executes `int 3`, so the machine intentionally halts. Save any required files before running it.

## COM1 serial log

`kernel/serial.c` initializes COM1 at I/O base `0x3F8` with 115200 baud, 8 data bits, no parity, and one stop bit. Transmit waits are bounded so a missing or stuck UART cannot deadlock the panic path.

Early boot, interrupt initialization, framebuffer setup, heap setup, and shell startup emit log records. A CPU exception writes its complete register dump to COM1 even when framebuffer output is unavailable.

QEMU attaches COM1 to the terminal by default through the Makefile:

```sh
make run-limine CROSS=i686-elf-
```

To save the log:

```sh
make run-limine CROSS=i686-elf- SERIAL='-serial file:serial.log'
```

For real hardware without a legacy UART, `irqinfo` reports COM1 as unavailable and panic output remains visible on the framebuffer.

## Source layout

```text
kernel/interrupt_stubs.asm  exception and IRQ entry stubs
kernel/interrupts.c         IDT, PIC, PIT, dispatch, tick counter
kernel/interrupts.h         saved frame and public interrupt API
kernel/panic.c              panic screen and register dump
kernel/panic.h              panic API
kernel/serial.c             COM1 driver and log helpers
kernel/serial.h             serial API
```

## Rules for future IRQ handlers

- Keep handlers bounded and non-blocking.
- Do not allocate from the heap inside an IRQ until allocator locking exists.
- Acknowledge the device before sending the PIC EOI.
- Do not call FAT, xHCI transfer, HDA streaming, or framebuffer presentation code from an IRQ without an explicitly IRQ-safe design.
- Protect shared state with short interrupt-disabled sections or atomics.
- Preserve the common frame layout when changing the assembly stubs.
