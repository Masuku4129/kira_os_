/* ARM64: раскладка программы та же, что на x86-64, со сдвигом VMM_USER_SHIFT (128 ТБ). */
USER_BASE = 0x808000000000;
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);      /* R-X */
    ro   PT_LOAD FLAGS(4);      /* R-- */
    data PT_LOAD FLAGS(6);      /* RW-: .data и .bss (хвост без файла - нули) */
}
SECTIONS
{
    . = USER_BASE;
    .text   : { *(.text.start) *(.text*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata*) } :ro
    . = ALIGN(4096);
    .data   : { *(.data*) } :data
    .bss    : { *(.bss*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
