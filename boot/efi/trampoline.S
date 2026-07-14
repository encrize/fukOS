

	.intel_syntax noprefix
	.text
	.code64
	.globl tramp_to_kernel
tramp_to_kernel:
	cli

	mov rsi, rcx
	mov rdi, 0x0500
	mov ecx, 7
1:
	mov eax, dword ptr [rsi]
	mov dword ptr [rdi], eax
	add rsi, 4
	add rdi, 4
	dec ecx
	jnz 1b

	lgdt [rip + gdt32_ptr]

	lea rax, [rip + compat32]
	push 0x08
	push rax
	lretq

	.code32
compat32:

	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	mov eax, cr0
	and eax, 0x7FFFFFFF
	mov cr0, eax

	mov ecx, 0xC0000080
	rdmsr
	and eax, 0xFFFFFEFF
	wrmsr

	mov eax, 0x00100000
	jmp eax

	.section .rodata
	.align 16
gdt32:
	.quad 0x0000000000000000
	.quad 0x00CF9A000000FFFF
	.quad 0x00CF92000000FFFF
gdt32_end:
gdt32_ptr:
	.word gdt32_end - gdt32 - 1
	.quad gdt32
