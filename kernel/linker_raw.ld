

OUTPUT_FORMAT(elf32-i386)
ENTRY(_kstart)

SECTIONS
{
    . = 0x00100000;

    .entry  : { *(.entry) }
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }
}
