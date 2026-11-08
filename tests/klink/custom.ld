/* Скрипт, который не совпадает ни с kernel.ld, ни с user.ld: проверка PROVIDE, ALIGN у секции, символов-границ, KEEP, 1M/4K, нескольких PHDRS. */
ENTRY(_start)
PHDRS
{
    t PT_LOAD FLAGS(5);
    r PT_LOAD FLAGS(4);
    d PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x8000000000 + 4K;
    __image_start = .;
    .text : ALIGN(64) { *(.text.start) *(.text*) } :t
    . = ALIGN(4K);
    __rodata_start = .;
    .rodata : { KEEP(*(.rodata.keepme)) *(.rodata*) } :r
    PROVIDE(__rodata_end = .);
    PROVIDE(never_referenced_sym = 0x1234);
    . = ALIGN(4096) + 1M;
    .data : { *(.data*) __data_mid = .; } :d
    .bss : { *(.bss*) *(COMMON) } :d
    __bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
