/* Embed the shareware IWAD directly into the kernel image so DOOM can load
   it via fopen("doom1.wad") without any real filesystem. */
    .section .rodata
    .align 16
    .global doom1_wad_start
    .global doom1_wad_end
doom1_wad_start:
    .incbin "doom/DOOM1.WAD"
doom1_wad_end:
