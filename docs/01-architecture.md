# Architecture

## Execution model

fukOS is a 32-bit monolithic i686 kernel. Drivers, shell, editor, HDA audio, FAT storage, xHCI, and DOOM all run in one address space. There is no process model, user mode, or paging yet.

This version has a physical-memory kernel heap initialized from the Multiboot2 memory map. Static buffers are still used for many large predictable objects, but dynamic allocation is now available for controlled use.

## Limine boot path

1. Firmware starts Limine from the FAT boot partition.
2. Limine loads `KERNEL.BIN` and passes Multiboot2 information.
3. `boot/boot.asm` sets up the 32-bit entry stack and calls C code.
4. `kernel/kernel.c` parses framebuffer, memory-map, and ACPI RSDP data.
5. The kernel enables framebuffer write combining when possible.
6. The kernel initializes the heap from the largest suitable usable memory range.
7. `shell_run()` initializes console, keyboard, storage, config, and command loop.

## Memory layout

`kernel/linker.ld` links the kernel at 1 MiB and defines linker symbols including `_kernel_end` plus Doom data/BSS ranges.

The heap starts above the static kernel image with a guard gap and is selected from the Multiboot2 memory map by `mb2_find_heap_range()`. The chosen range is identity-mapped physical memory below the 32-bit address cap.

## Kernel heap

Files:

```text
kernel/heap.c
kernel/heap.h
```

The heap is intentionally simple:

- first-fit search;
- 16-byte alignment;
- per-block header with size, links, free flag, and magic;
- split large free blocks on allocation;
- coalesce adjacent free blocks on free;
- support for `kheap_alloc`, `kheap_calloc`, `kheap_realloc`, `kheap_free`;
- `kheap_get_info()` for total/used/free/largest-free/allocation statistics.

This is not a virtual-memory allocator. It manages one physical contiguous range and assumes identity mapping.

## Heap testing

The shell command:

```text
heaptest
```

checks allocation, free, calloc zeroing, realloc growth, realloc shrink, data preservation, block reuse, split behavior, and final allocation-count recovery.

A successful run prints `heaptest: OK` with before/after free and largest-free numbers.

## Static and shared buffers

Large static buffers remain important for:

- framebuffer shadows;
- wallpaper/terminal background storage;
- file and image buffers;
- editor undo buffer;
- xHCI rings and DMA buffers;
- HDA BDL and stream ring.

Callers must not keep pointers into shared buffers after another subsystem might reuse them.

## Doom state reset

DOOM was designed for one process lifetime. fukOS can launch it repeatedly by isolating Doom initialized data and BSS in linker ranges. Before each launch, `doom_reset_state()` restores initialized data from a pristine copy and clears Doom BSS.

DOOM then reserves a private 32 MiB arena from the kernel heap and gives that arena to its freestanding libc allocator. On DOOM exit, the arena is returned to the kernel heap.

## ACPI

The Multiboot2 ACPI RSDP is passed to shell code. Shutdown locates FADT/DSDT, extracts `_S5_`, and writes sleep control registers. VM-specific power ports are fallback paths only.

## Error-handling rules

- Validate firmware-provided sizes and addresses before dereferencing.
- Bound hardware waits.
- Check DMA alignment and physical-address limits.
- Treat QEMU-only success as insufficient evidence.
- Keep heap allocations paired with frees and test with `heaptest` after allocator changes.
