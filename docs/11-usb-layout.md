# USB layout and boot installation

## Recommended method: write the generated image

This is the least error-prone installation path because it creates the MBR,
partition, FAT filesystem, Limine BIOS stages, and UEFI fallback loader in one
flow.

1. Put optional runtime files in the repository root:

```text
fuko.conf
boot.bmp
```

2. Build from a clean tree:

```sh
make clean
make kernel.bin CROSS=i686-elf-
make limine CROSS=i686-elf-
```

`make limine` automatically includes `fuko.conf` and `boot.bmp` when those files
exist in the repository root.

3. Identify the USB device:

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
```

4. Unmount its partitions and write the image to the whole device:

```sh
sudo umount /dev/sdX?* 2>/dev/null || true
sudo dd if=os-limine.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Replace `/dev/sdX` with the USB device, for example `/dev/sdb`. Do not use a
partition such as `/dev/sdb1` as the `dd` destination.

> **Warning:** `dd` destroys all data on the selected device. Confirm the model
> and size in `lsblk` before running it. Never assume that the USB is `/dev/sda`.

## Expected filesystem tree

After writing the image and mounting its first partition, the tree is:

```text
USB FAT root/
├── EFI/
│   └── BOOT/
│       ├── BOOTX64.EFI
│       └── BOOTIA32.EFI
├── KERNEL.BIN
├── limine.conf
├── limine-bios.sys
├── fuko.conf          # optional
└── boot.bmp           # optional
```

FAT is case-insensitive, but using these names exactly avoids confusion.

`fuko.conf` example:

```ini
BOOT_IMAGE = boot.bmp
BOOT_IMAGE_SECONDS = 3
BOOT_IMAGE_MODE = fit
TERMINAL_TRANSPARENCY = 55
AUDIO_OUTPUT = auto
```

## What is not visible as a normal file

A BIOS-capable Limine USB also contains boot code written into the disk MBR and
post-MBR area by:

```sh
limine bios-install os-limine.img
```

Copying `limine-bios.sys` alone does not install the BIOS boot code. This is why
writing the complete `os-limine.img` is preferred over manually copying files.

UEFI boot uses `EFI/BOOT/BOOTX64.EFI`; the Acer's 64-bit firmware should find
this fallback path without a custom NVRAM entry.

## Manual installation to an existing FAT32 USB

Use this only when preserving an existing partition is necessary.

Assuming the whole USB is `/dev/sdX` and its FAT partition is `/dev/sdX1`:

```sh
sudo umount /dev/sdX1
sudo parted -s /dev/sdX mklabel msdos
sudo parted -s /dev/sdX mkpart primary fat32 1MiB 100%
sudo parted -s /dev/sdX set 1 boot on
sudo mkfs.fat -F 32 -n FUKOS /dev/sdX1
sudo mkdir -p /mnt/fukos
sudo mount /dev/sdX1 /mnt/fukos
sudo mkdir -p /mnt/fukos/EFI/BOOT
sudo cp kernel.bin /mnt/fukos/KERNEL.BIN
sudo cp limine.conf /mnt/fukos/limine.conf
sudo cp /usr/share/limine/limine-bios.sys /mnt/fukos/limine-bios.sys
sudo cp /usr/share/limine/BOOTX64.EFI /mnt/fukos/EFI/BOOT/BOOTX64.EFI
sudo cp /usr/share/limine/BOOTIA32.EFI /mnt/fukos/EFI/BOOT/BOOTIA32.EFI
sudo cp fuko.conf /mnt/fukos/fuko.conf        # optional
sudo cp boot.bmp /mnt/fukos/boot.bmp          # optional
sync
sudo umount /mnt/fukos
sudo limine bios-install /dev/sdX
sync
```

The final `bios-install` target is the whole disk `/dev/sdX`, not `/dev/sdX1`.
For UEFI-only boot it is not required, but keeping both paths makes the USB
portable.

## Limine configuration

The supplied `limine.conf` is:

```text
timeout: 3

/fukOS
    protocol: multiboot2
    path: boot():/KERNEL.BIN
```

The kernel must be built as a 32-bit Multiboot2 ELF file despite its historical
`.bin` name. Check it before copying:

```sh
file kernel.bin
readelf -h kernel.bin | grep -E 'Class|Machine|Entry'
```

Expected essentials:

```text
Class:   ELF32
Machine: Intel 80386
```

## Black-screen isolation

The stable build prints boot checkpoints directly to the framebuffer:

```text
fukOS boot: framebuffer console ready
fukOS boot: keyboard ready
fukOS boot: probing ATA storage...
fukOS boot: ATA unavailable, probing xHCI USB...
fukOS boot: FAT storage mounted
```

Interpretation:

- No Limine menu: firmware or USB boot installation problem.
- Limine menu appears, then no checkpoint text: kernel was not loaded correctly,
  framebuffer was not supplied, or the copied `kernel.bin` is stale.
- Stops after `probing ATA`: ATA timeout path.
- Stops after `probing xHCI USB`: xHCI enumeration or USB Mass Storage path.
- Reaches `FAT storage mounted` but no splash: `fuko.conf` or BMP decoding issue.

For the first test, remove optional files completely:

```sh
sudo rm /run/media/$USER/FUKOS/fuko.conf
sudo rm /run/media/$USER/FUKOS/boot.bmp
sync
```

Then boot only with Limine, the kernel, and EFI files. Add `fuko.conf` and
`boot.bmp` only after the shell is confirmed working.

## Firmware settings

On the Acer:

- select the USB entry beginning with `UEFI:` when testing the UEFI path;
- disable Secure Boot because the custom kernel and Limine binary are unsigned;
- if both UEFI and legacy entries exist, test UEFI first;
- the PIT timer IRQ is enabled, while keyboard, xHCI, and HDA remain on their polling/cooperative paths.
