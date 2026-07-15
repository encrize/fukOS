# Build and deployment on Arch Linux

## Supported build environment

The documented host is Arch Linux on x86-64. The supported kernel build uses a dedicated `i686-elf` GCC/binutils cross-toolchain through `CROSS=i686-elf-`. Native host GCC is not the documented build path.

## Install dependencies

Update the system and install the required packages:

```sh
sudo pacman -Syu
sudo pacman -S --needed base-devel nasm python qemu-system-x86 limine
```

Package roles:

| Package | Purpose |
|---|---|
| `base-devel` | GNU Make and standard build utilities |
| `nasm` | Multiboot2 and interrupt entry assembly |
| `python` | FAT image builder in `tools/` |
| `qemu-system-x86` | Local BIOS/UEFI boot tests |
| `limine` | Bootloader executable and BIOS/UEFI files |

Build or install an `i686-elf` GCC/binutils toolchain separately; Arch's official repositories do not provide the complete cross-toolchain as a standard package. Both `i686-elf-gcc` and `i686-elf-ld` must be in `PATH`.

For UEFI tests, also install OVMF:

```sh
sudo pacman -S --needed edk2-ovmf
```

## Verify the host

Check the important tools before building:

```sh
i686-elf-gcc --version
i686-elf-ld -V | grep elf_i386
nasm -v
python3 --version
qemu-system-x86_64 --version
limine --version
```

`i686-elf-ld -V` must list `elf_i386`. The Makefile performs equivalent checks and stops with a focused error when a required tool is missing.

## Build targets

| Command | Result |
|---|---|
| `make kernel.bin CROSS=i686-elf-` | Build `kernel.bin` with the supported cross-toolchain |
| `make limine` | Build `os-limine.img` for BIOS and UEFI |
| `make run-limine` | Build and start the image in QEMU |
| `./run.sh` | Short form of `make run-limine` |
| `make clean` | Remove generated objects and images |

A clean build is recommended before a hardware test:

```sh
make clean
make kernel.bin CROSS=i686-elf-
make limine CROSS=i686-elf-
```

The build outputs are intentionally ignored by Git.

## Limine paths

The default `LIMINE_DIR` is `/usr/share/limine` when the Arch package is installed. If those files are elsewhere, override the directory:

```sh
make limine CROSS=i686-elf- LIMINE_DIR="$HOME/src/limine"
```

The directory must contain:

```text
limine-bios.sys
BOOTX64.EFI
BOOTIA32.EFI
```

The `limine` executable may be installed in `PATH` or present as `LIMINE_DIR/limine`. Override it separately when necessary:

```sh
make limine CROSS=i686-elf- LIMINE=/path/to/limine LIMINE_DIR=/path/to/limine-files
```

## QEMU

BIOS/SeaBIOS is the default:

```sh
make run-limine CROSS=i686-elf-
```

QEMU connects COM1 to the launching terminal by default, so boot messages and
panic register dumps appear alongside QEMU output. Disable this only when
another serial backend is supplied:

```sh
make run-limine CROSS=i686-elf- SERIAL='-serial file:serial.log'
```

The Makefile tries KVM first and falls back to TCG. To force software emulation:

```sh
make run-limine CROSS=i686-elf- ACCEL='-accel tcg'
```

For UEFI, pass the OVMF code image explicitly:

```sh
make run-limine CROSS=i686-elf- OVMF=/usr/share/edk2/x64/OVMF_CODE.fd
```

If your package uses another path, locate it with:

```sh
pacman -Ql edk2-ovmf | grep OVMF_CODE
```

QEMU is a build and boot sanity check. The Acer Aspire ES1-533 remains the primary hardware validation target.

## Write the image to USB (1 method)

> **Warning:** the following command destroys data on the selected device. Confirm the device name with `lsblk` and use the whole device, not a partition.

Replace `/dev/sdX` with the actual USB device.

```sh
sudo umount /dev/sdX
```

```sh
sudo mkfs.vfat -F 32 -n "fukOS" /dev/sdX
```

├── EFI/
│   └── BOOT/
│       ├── BOOTIA32.EFI
│       └── BOOTX64.EFI
├── apps/
│    ├── calc.fuk
│    └── tetris.fuk
├── fuko.conf
├── kernel.bin
├── limine-bios.sys
└── limine.conf

To update an existing compatible Limine boot partition without rewriting the full image, replace both files in the FAT root:

```text
kernel.bin
limine.conf
```

## Write the image to USB (2 method)

> **Warning:** the following command destroys data on the selected device. Confirm the device name with `lsblk` and use the whole device, not a partition.

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
sudo umount /dev/sdX?* 2>/dev/null || true
sudo dd if=os-limine.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Replace `/dev/sdX` with the actual USB device. On NVMe or MMC media, use the correct whole-device path such as `/dev/nvme1n1` or `/dev/mmcblk0`.

To update an existing compatible Limine boot partition without rewriting the full image, replace both files in the FAT root:

```text
kernel.bin
limine.conf
```

## Configuration file

`fuko.conf` is an optional runtime configuration file read from the FAT root:

```ini
BOOT_IMAGE = boot.bmp
BOOT_IMAGE_SECONDS = 3
BOOT_IMAGE_MODE = fit
WALLPAPER = photos/wallpaper.bmp
TERMINAL_TRANSPARENCY = 55
# AUDIO_OUTPUT = auto
```

Copy `boot.bmp` and `fuko.conf` to the FAT root of the boot medium. The splash
is loaded by fukOS after the storage driver mounts the volume; it is not a
Limine splash and therefore works through the kernel's BMP decoder.

Supported audio values are `auto`, `speaker`, and `headphones`.

## Validation

Static checks used during this review:

```sh
python3 -m py_compile tools/*.py
bash -n run.sh
make -n kernel.bin CROSS=i686-elf-
```

After booting the Acer, run:

```text
ff
heaptest
irqinfo
bgplay *.wav
doom
```

Verify that:

- shell and editor input remain responsive;
- `heaptest` prints `OK`;
- background audio continues while DOOM runs;
- DOOM can create a save file;
- the save is visible with `ls` and loads after reboot;
- FAT writes and screenshots complete without xHCI timeouts.

## Troubleshooting

### `i686-elf-gcc` is not found

Install or build the cross-toolchain, add its `bin` directory to `PATH`, and rerun `make kernel.bin CROSS=i686-elf-`.

### Linker does not support `elf_i386`

Verify the cross-binutils installation and confirm that `i686-elf-ld -V` lists `elf_i386`.

### Limine files are not found

Check `pacman -Ql limine`, then set `LIMINE_DIR` to the directory containing `limine-bios.sys` and the EFI executables.

### KVM is unavailable

Force TCG with `make run-limine CROSS=i686-elf- ACCEL='-accel tcg'`. This is slower but does not require `/dev/kvm`.
