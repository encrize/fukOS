CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
ASM     := nasm
PYTHON  ?= python3
QEMU    ?= qemu-system-x86_64
OVMF    ?=
SERIAL  ?= -serial stdio

CFLAGS  := -m32 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-tree-loop-distribute-patterns -fno-builtin \
           -Wall -Wextra -O2 -Ikernel

OBJS := build/boot.o build/kernel.o build/multiboot2.o \
        build/render.o build/framebuffer.o build/ata.o build/image.o \
        build/fat.o build/util.o build/console.o build/keyboard.o \
        build/shell.o build/demo_image.o build/font.o build/pci.o \
        build/xhci.o build/rtc.o build/hda.o build/heap.o \
        build/serial.o build/interrupts.o build/interrupt_stubs.o build/panic.o

GCC_INC := $(shell $(CC) $(CFLAGS) -print-file-name=include)
LIBGCC  := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)

# Doom is built against its private freestanding headers.
DOOMFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector \
             -fno-tree-loop-distribute-patterns -fno-builtin -fno-common -O2 -w \
             -nostdinc -Idoom -Idoom/inc -Ikernel -isystem $(GCC_INC) \
             -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
             -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200

DOOM_CSRC := $(wildcard doom/*.c)
DOOM_OBJS := $(patsubst doom/%.c,build/doom/%.o,$(DOOM_CSRC)) build/doom/dg_wad.o
OBJS += $(DOOM_OBJS)

LDFLAGS := -m elf_i386 -T kernel/linker.ld -nostdlib

LIMINE_DIR ?= $(if $(wildcard /usr/share/limine/limine-bios.sys),/usr/share/limine,limine)
LIMINE     ?= $(if $(wildcard $(LIMINE_DIR)/limine),$(LIMINE_DIR)/limine,limine)

.PHONY: all toolcheck image-toolcheck run-toolcheck limine run-limine clean

all: kernel.bin

toolcheck:
	@command -v $(CC) >/dev/null || { echo "ERROR: $(CC) not found."; exit 1; }
	@command -v $(LD) >/dev/null || { echo "ERROR: $(LD) not found."; exit 1; }
	@command -v $(ASM) >/dev/null || { echo "ERROR: $(ASM) not found; install nasm."; exit 1; }
	@$(CC) -m32 -print-libgcc-file-name 2>/dev/null | grep -qv '^libgcc.a$$' || { \
	  echo "ERROR: 32-bit libgcc not found; install gcc-multilib."; exit 1; }
	@$(LD) -V 2>/dev/null | grep -q elf_i386 || { \
	  echo "ERROR: $(LD) cannot produce elf_i386 output."; \
	  echo "Install Arch binutils or use CROSS=i686-elf-."; exit 1; }

image-toolcheck:
	@command -v $(PYTHON) >/dev/null || { echo "ERROR: $(PYTHON) not found; install python."; exit 1; }
	@test -f "$(LIMINE_DIR)/limine-bios.sys" || { echo "ERROR: $(LIMINE_DIR)/limine-bios.sys not found."; exit 1; }
	@test -f "$(LIMINE_DIR)/BOOTX64.EFI" || { echo "ERROR: $(LIMINE_DIR)/BOOTX64.EFI not found."; exit 1; }
	@test -f "$(LIMINE_DIR)/BOOTIA32.EFI" || { echo "ERROR: $(LIMINE_DIR)/BOOTIA32.EFI not found."; exit 1; }
	@command -v $(LIMINE) >/dev/null || { echo "ERROR: Limine installer not found: $(LIMINE)"; exit 1; }

run-toolcheck:
	@command -v $(QEMU) >/dev/null || { echo "ERROR: $(QEMU) not found; install qemu-system-x86."; exit 1; }
	@if [ -n "$(OVMF)" ] && [ ! -f "$(OVMF)" ]; then echo "ERROR: OVMF file not found: $(OVMF)"; exit 1; fi

build:
	mkdir -p build

build/boot.o: boot/boot.asm | build
	$(ASM) -f elf32 $< -o $@

build/interrupt_stubs.o: kernel/interrupt_stubs.asm | build
	$(ASM) -f elf32 $< -o $@

build/%.o: kernel/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/doom:
	mkdir -p build/doom

build/doom/%.o: doom/%.c | build/doom
	$(CC) $(DOOMFLAGS) -c $< -o $@

build/doom/dg_wad.o: doom/dg_wad.S | build/doom
	$(CC) -m32 -c $< -o $@

# Large generated data tables do not benefit from optimization.
build/demo_image.o: kernel/demo_image.c | build
	$(CC) $(subst -O2,-O0,$(CFLAGS)) -c $< -o $@
build/font.o: kernel/font.c | build
	$(CC) $(subst -O2,-O0,$(CFLAGS)) -c $< -o $@

kernel.bin: $(OBJS) kernel/linker.ld | toolcheck
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

limine: kernel.bin limine.conf tools/make_usb.py tools/fatfs.py | image-toolcheck
	$(PYTHON) tools/make_usb.py --conf limine.conf --kernel kernel.bin \
	        --limine-bios-sys $(LIMINE_DIR)/limine-bios.sys \
	        --efi64 $(LIMINE_DIR)/BOOTX64.EFI \
	        --efi32 $(LIMINE_DIR)/BOOTIA32.EFI \
	        --out os-limine.img
	$(LIMINE) bios-install os-limine.img
	sync

ACCEL    ?= -accel kvm -accel tcg
QEMU_CPU ?= max

run-limine: limine run-toolcheck
	$(QEMU) -drive file=os-limine.img,format=raw,if=ide -m 512 -vga std \
	        -cpu $(QEMU_CPU) $(ACCEL) $(SERIAL) $(if $(OVMF),-bios $(OVMF),)

clean:
	rm -rf build kernel.bin os-limine.img
