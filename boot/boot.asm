

bits 32

MB2_MAGIC      equ 0xE85250D6
MB2_ARCH       equ 0

section .multiboot_header
align 8
; Multiboot2 header and framebuffer request.
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd header_end - header_start
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (header_end - header_start))

    align 8
    dw 5
    dw 0
    dd 20
    dd 0
    dd 0
    dd 32

    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .bss
align 16
; Early boot stack used before entering C.
stack_bottom:
    resb 32768
stack_top:

section .text
global _start
extern kmain
_start:
    mov esp, stack_top
    xor ebp, ebp
    push ebx
    push eax
    call kmain
.hang:
    cli
    hlt
    jmp .hang
