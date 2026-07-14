

bits 16
org 0x7C00

STAGE2_LBA      equ 1
STAGE2_SECTORS  equ 8
STAGE2_SEG      equ 0x0000
STAGE2_OFF      equ 0x7E00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    ; Load the fixed-size second stage through BIOS extended reads.
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc  disk_error

    mov dl, [boot_drive]
    jmp STAGE2_SEG:STAGE2_OFF

disk_error:
    mov si, msg
.print:
    lodsb
    or  al, al
    jz  .halt
    mov ah, 0x0E
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
msg        db "Stage1 disk error", 0

align 4
dap:
    db 0x10
    db 0
    dw STAGE2_SECTORS
    dw STAGE2_OFF
    dw STAGE2_SEG
    dq STAGE2_LBA

; One bootable FAT16 partition beginning at LBA 2048.
times 446-($-$$) db 0

    db 0x80
    db 0xFE, 0xFF, 0xFF
    db 0x0E
    db 0xFE, 0xFF, 0xFF
    dd 2048
    dd 262144

    times 16*3 db 0

dw 0xAA55
