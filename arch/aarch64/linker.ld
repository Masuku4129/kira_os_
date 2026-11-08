/* ARM64: ядро собрано с адреса 0 как позиционно-независимое (PIE): загрузчик кладёт образ куда угодно
   (2 МБ-выровненный адрес + 0x80000), boot.S сам применяет перемещения. Адреса виртуальные = физические
   (тождественная карта MMU).
   Границы wx_* (W^X, main.c): текст, затем данные только для чтения (после перемещений в boot.S их никто не пишет),
   затем записываемые данные и bss; каждая группа начинается и заканчивается на границе 4-КБ страницы, поэтому права
   (текст - RX, чтение - RO+PXN, данные - RW+PXN) ставятся постранично. */
ENTRY(_start)
SECTIONS
{
    . = 0;
    __kernel_start = .;
    kernel_start = .;            /* то же имя, что у x86-64 (проверка памяти консоли) */
    wx_text_start = .;
    .text : { text_start = .; KEEP(*(.text.boot)) *(.text*) text_end = .; }
    . = ALIGN(4096);
    wx_text_end = .;
    wx_ro_start = .;
    .rodata : ALIGN(16) { ro_start = .; *(.rodata*) ro_end = .; }
    .data.rel.ro : ALIGN(16) { *(.data.rel.ro*) }
    .rela.dyn : ALIGN(8) { __rela_start = .; *(.rela*) __rela_end = .; }
    .dynamic : { *(.dynamic) }
    .got : { *(.got*) }
    . = ALIGN(4096);
    wx_ro_end = .;
    wx_data_start = .;
    .data : ALIGN(16) { *(.data*) }
    .bss (NOLOAD) : ALIGN(16) { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; }
    . = ALIGN(4096);
    wx_data_end = .;
    __kernel_end = .;
    __image_size = __kernel_end - __kernel_start;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp) }
}
