ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);      /* R-X */
    ro   PT_LOAD FLAGS(4);      /* R-- */
    data PT_LOAD FLAGS(6);      /* RW-: .data и .bss (хвост без файла - нули) */
}
SECTIONS
{
    . = 0x8000000000;
    .text   : { *(.text.start) *(.text*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata*) } :ro
    . = ALIGN(4096);
    .data   : { *(.data*) } :data
    .bss    : { *(.bss*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
