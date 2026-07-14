ENTRY(_start)

SECTIONS
{
    . = 1M;

    .boot ALIGN(8) :
    {
        KEEP(*(.multiboot_header))
    }

    .text ALIGN(4K) :
    {
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        /* Keep Doom state contiguous so kernel.c can reset it. */
        __doom_data_start = .;
        */doom/*.o(.data .data.*)
        __doom_data_end = .;
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        __doom_bss_start = .;
        */doom/*.o(.bss .bss.* COMMON)
        __doom_bss_end = .;
        *(COMMON)
        *(.bss*)
    }

    _kernel_end = .;
}
