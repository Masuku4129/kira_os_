/* ARM64: ядро загружается по физическому адресу из этого файла (QEMU virt: ОЗУ с 0x40000000, первые 512 КБ -
   дереву устройств). Адреса виртуальные = физические (тождественная карта MMU). */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    __kernel_start = .;
    .text : { KEEP(*(.text.boot)) *(.text*) }
    .rodata : ALIGN(16) { *(.rodata*) }
    .data : ALIGN(16) { *(.data*) }
    .bss (NOLOAD) : ALIGN(16) { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; }
    . = ALIGN(4096);
    __kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
