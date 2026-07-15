import argparse
import os
import struct

import fatfs

SECTOR = 512
PART_LBA = fatfs.PART_LBA
PART_SECTORS = fatfs.PART_SECTORS
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def read_file(path):
    with open(path, "rb") as source:
        return source.read()

def add_optional(fs, source, destination):
    if not source:
        return
    if not os.path.isfile(source):
        raise FileNotFoundError(source)
    fs.add_file(destination, read_file(source))

def build_image(args):
    fs = fatfs.FatFS()
    fs.add_file("limine.conf", read_file(args.conf))
    fs.add_file("KERNEL.BIN", read_file(args.kernel))
    add_optional(fs, args.limine_bios_sys, "limine-bios.sys")
    add_optional(fs, args.efi64, "EFI/BOOT/BOOTX64.EFI")
    add_optional(fs, args.efi32, "EFI/BOOT/BOOTIA32.EFI")

    for name in ("fuko.conf", "boot.bmp"):
        path = os.path.join(ROOT, name)
        if os.path.isfile(path):
            fs.add_file(name, read_file(path))

    apps_dir = os.path.join(ROOT, "apps")
    if os.path.isdir(apps_dir):
        fs.add_dir("apps")
        for name in sorted(os.listdir(apps_dir)):
            source = os.path.join(apps_dir, name)
            if os.path.isfile(source) and name.lower().endswith(".fuk"):
                fs.add_file("apps/" + name, read_file(source))

    partition = fs.serialize()
    expected_size = PART_SECTORS * SECTOR
    if len(partition) != expected_size:
        raise RuntimeError("unexpected FAT image size")

    image = bytearray((PART_LBA + PART_SECTORS) * SECTOR)
    entry = 446
    image[entry:entry + 8] = bytes(
        [0x80, 0xFE, 0xFF, 0xFF, 0x0E, 0xFE, 0xFF, 0xFF]
    )
    image[entry + 8:entry + 16] = struct.pack(
        "<II", PART_LBA, PART_SECTORS
    )
    image[510:512] = b"\x55\xAA"

    offset = PART_LBA * SECTOR
    image[offset:offset + len(partition)] = partition
    with open(args.out, "wb") as destination:
        destination.write(image)

    print("Created %s (%d MiB FAT16 partition)" % (
        args.out, expected_size // 1048576
    ))

def main():
    parser = argparse.ArgumentParser(
        description="Create a Limine BIOS/UEFI disk image"
    )
    parser.add_argument("--conf", default=os.path.join(ROOT, "limine.conf"))
    parser.add_argument("--kernel", default=os.path.join(ROOT, "kernel.bin"))
    parser.add_argument("--limine-bios-sys", dest="limine_bios_sys")
    parser.add_argument("--efi64")
    parser.add_argument("--efi32")
    parser.add_argument("--out", default=os.path.join(ROOT, "os-limine.img"))
    build_image(parser.parse_args())

if __name__ == "__main__":
    main()
