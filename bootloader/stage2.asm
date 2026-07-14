

bits 16
org 0x7E00

KERNEL_LBA      equ 9
KERNEL_SECTORS  equ 896
KERNEL_DST_SEG  equ 0x1000
BOOTINFO        equ 0x0500
TARGET_W        equ 1024
TARGET_H        equ 768

STORAGE_LBA     equ 2048
PART_SECTORS    equ 262144
STORAGE_PHYS    equ 0x04000000
BOUNCE_SEG      equ 0x8000
BOUNCE_LIN      equ 0x00080000

stage2_start:
xor ax, ax
mov ds, ax
mov es, ax
mov ss, ax
mov sp, 0x7C00
mov [boot_drive], dl

; Select the target 32-bit linear framebuffer mode.
mov di, vbe_info
mov dword [di], 0x32454256
mov ax, 0x4F00
int 0x10
cmp ax, 0x004F
jne fatal

mov si, [vbe_info + 0x0E]
mov ax, [vbe_info + 0x10]
mov fs, ax
.next:
mov cx, [fs:si]
add si, 2
cmp cx, 0xFFFF
je  fatal
push si
push fs
xor ax, ax
mov es, ax
mov di, mode_info
mov ax, 0x4F01
int 0x10
pop fs
pop si
cmp ax, 0x004F
jne .next
test byte [mode_info + 0x00], 0x80
jz  .next
mov ax, [mode_info + 0x12]
cmp ax, TARGET_W
jne .next
mov ax, [mode_info + 0x14]
cmp ax, TARGET_H
jne .next
cmp byte [mode_info + 0x19], 32
jne .next

mov bx, cx
or  bx, 0x4000
mov ax, 0x4F02
int 0x10
cmp ax, 0x004F
jne fatal

mov di, BOOTINFO
mov eax, [mode_info + 0x28]
mov [di + 0], eax
movzx eax, word [mode_info + 0x10]
mov [di + 4], eax
movzx eax, word [mode_info + 0x12]
mov [di + 8], eax
movzx eax, word [mode_info + 0x14]
mov [di + 12], eax
movzx eax, byte [mode_info + 0x19]
mov [di + 16], eax

; Load the flat kernel into low memory in BIOS-sized chunks.
mov word  [dap.count], 64
mov word  [dap.off], 0
mov word  [dap.seg], KERNEL_DST_SEG
mov dword [dap.lba_lo], KERNEL_LBA
mov dword [dap.lba_hi], 0
mov cx, KERNEL_SECTORS / 64
.load:
push cx
mov ah, 0x42
mov dl, [boot_drive]
mov si, dap
int 0x13
jc  fatal
add word  [dap.seg], 0x800
add dword [dap.lba_lo], 64
pop cx
loop .load

in  al, 0x92
or  al, 2
out 0x92, al

; Preload the FAT partition for the legacy non-xHCI path.
mov eax, PART_SECTORS
mov [storage_total], eax

mov [sec_left], eax
mov dword [dest_ptr], STORAGE_PHYS
mov dword [dap.lba_lo], STORAGE_LBA
.chunk:
mov eax, [sec_left]
test eax, eax
jz  .store_done
cmp eax, 64
jbe .cnt
mov eax, 64
.cnt:
mov [this_count], ax

mov bx, ax
xor ax, ax
mov ds, ax
mov es, ax
mov ax, bx

mov [dap.count], ax
mov word [dap.off], 0
mov word [dap.seg], BOUNCE_SEG
mov ah, 0x42
mov dl, [boot_drive]
mov si, dap
int 0x13
jc  .no_storage

call go_unreal
mov esi, BOUNCE_LIN
mov edi, [dest_ptr]
movzx ecx, word [this_count]
shl ecx, 7
a32 rep movsd
mov [dest_ptr], edi

movzx eax, word [this_count]
add [dap.lba_lo], eax
sub [sec_left], eax
jmp .chunk
.store_done:
mov di, BOOTINFO
mov dword [di + 20], STORAGE_PHYS
mov eax, [storage_total]
shl eax, 9
mov [di + 24], eax
jmp .storage_end
.no_storage:
mov di, BOOTINFO
mov dword [di + 20], 0
mov dword [di + 24], 0
.storage_end:

cli
xor ax, ax
mov ds, ax
mov es, ax

; Enter protected mode and relocate the kernel to 1 MiB.
lgdt [gdtr]
mov eax, cr0
or  eax, 1
mov cr0, eax
jmp 0x08:pmode

go_unreal:
; Unreal mode provides 32-bit addressing while BIOS calls remain available.
cli
xor ax, ax
mov ds, ax
lgdt [gdtr]
mov eax, cr0
or  al, 1
mov cr0, eax
jmp short .flush
.flush:
mov bx, 0x10
mov ds, bx
mov es, bx
mov eax, cr0
and al, 0xFE
mov cr0, eax
sti
ret

bits 32
pmode:
mov ax, 0x10
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
mov ss, ax
mov esp, 0x90000

mov esi, 0x10000
mov edi, 0x100000
mov ecx, (KERNEL_SECTORS * 512) / 4
rep movsd

jmp 0x08:0x100000

bits 16
fatal:
mov si, msg
.p:
lodsb
or  al, al
jz  .h
mov ah, 0x0E
int 0x10
jmp .p
.h:
cli
hlt
jmp .h

boot_drive db 0
msg        db "Stage2 error (no LFB 32bpp mode at target res, or disk error)", 0

align 4
storage_total dd 0
dest_ptr      dd 0
sec_left      dd 0
this_count    dw 0

align 8
gdt_start:
dq 0x0000000000000000
dq 0x00CF9A000000FFFF
dq 0x00CF92000000FFFF
gdt_end:
gdtr:
dw gdt_end - gdt_start - 1
dd gdt_start

align 4
dap:
db 0x10
db 0
.count:  dw 0
.off:    dw 0
.seg:    dw 0
.lba_lo: dd 0
.lba_hi: dd 0

vbe_info:  times 512 db 0
mode_info: times 256 db 0

times (8*512)-($-$$) db 0
