bits 32
section .entry
global _kstart
extern kmain_raw
extern __bss_start
extern __bss_end
_kstart:
    mov esp, 0x90000
    xor ebp, ebp

    ; Flat binaries do not contain the BSS payload.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain_raw
.hang:
    cli
    hlt
    jmp .hang
