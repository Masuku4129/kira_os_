ENTRY(_start)
SECTIONS
{
    . = BASE;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data : { *(.data*) }
    . = ALIGN(8);
    kimg_end = .;
    .kreloc : { KEEP(*(.kreloc)) }
    .bss : { *(.bss*) *(COMMON) }
}
