# Storage drivers

## Overview

fukOS exposes FAT files through a common sector interface backed by either ATA PIO, xHCI USB Mass Storage, or a memory image supplied by a legacy loader. The normal hardware path on the Acer laptop is xHCI.

## PCI discovery

`kernel/pci.c` scans PCI configuration space through ports `0xCF8` and `0xCFC`. Drivers use class, subclass, and programming-interface values rather than fixed bus addresses.

The xHCI controller is identified by class `0x0C`, subclass `0x03`, and programming interface `0x30`. Before MMIO access, the kernel enables memory-space and bus-master operation in the PCI command register.

## ATA PIO

`kernel/ata.c` implements legacy primary-channel PIO transfers. It is useful in emulators and on systems exposing compatibility-mode IDE, but Apollo Lake laptops normally do not provide the boot USB device through ATA ports.

ATA functions use bounded status polling and validate error bits before transferring sector words.

## xHCI initialization

`kernel/xhci.c` performs the minimum controller sequence required for one USB Mass Storage device:

1. Map the controller MMIO BAR.
2. Stop and reset the host controller.
3. Allocate and initialize DCBAA, command ring, event ring, and ERST storage.
4. Start the controller.
5. Reset a connected port and determine its negotiated speed.
6. Enable a slot and address the device.
7. Read USB descriptors through endpoint zero.
8. Configure the bulk IN and bulk OUT endpoints.

All structures are statically allocated and aligned for DMA. Because the kernel has no paging, virtual and physical addresses are identical.

## USB Mass Storage

The driver implements Bulk-Only Transport:

- Command Block Wrapper (CBW)
- optional data stage
- Command Status Wrapper (CSW)

SCSI commands include Inquiry, Test Unit Ready, Read Capacity, Read(10), and Write(10). Transfer completion is consumed from the xHCI event ring.

The Event Handler Busy bit and dequeue pointer must be acknowledged correctly. Failing to update them can leave the ring apparently empty or cause old events to be processed again.

## Port behavior on real hardware

Apollo Lake controllers may expose USB 2 and USB 3 protocol ranges separately. Port reset and link-state handling must follow the protocol capability associated with the selected port. Emulator behavior is not sufficient to validate SuperSpeed link training.

## FAT layer

`kernel/fat.c` supports FAT16 and FAT32 without dynamic allocation. It provides:

- mounting and BPB validation
- root and subdirectory enumeration
- short names and long file names
- file reads and writes
- file creation, replacement, copying, and deletion
- file rename and cross-directory moves without copying cluster data
- directory creation, removal, and navigation
- cluster allocation and chain release
- existence checks without opening or reading a file (`fat_exists()`)

All on-disk integers are decoded explicitly as little-endian values. Date and time fields come from the RTC.

## Removing directories

`fat_rmdir()` refuses to remove `.`, `..`, anything that is not a directory
entry, and any directory that still has entries other than `.`/`..`. It
temporarily `fat_chdir()`s into the target to count its contents with the
same directory-iteration path `fat_dir_count()` uses, then restores the
original working directory before unlinking the now-confirmed-empty
directory's entry and releasing its cluster chain. The shell exposes this as
`rmdir` (alias `rd`).

## Mount order

At shell startup:

1. Try the ATA backend.
2. Discover and initialize xHCI.
3. Locate a FAT partition on USB Mass Storage.
4. Leave storage unavailable if neither backend succeeds.

The shell reports the mounted directory count and keeps all file commands behind `fat_mounted()` checks.

## Safety rules

- Never assume a fixed xHCI MMIO address.
- Keep command and transfer-ring ownership rules explicit.
- Bound every hardware wait.
- Validate CSW signature, tag, residue, and status.
- Flush sector updates before reporting a successful file operation.
- Test write paths on expendable media before using important data.
