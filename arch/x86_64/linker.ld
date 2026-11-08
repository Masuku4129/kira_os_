ENTRY(start)

/* Границы секций для W^X (vmm.c / wx.c): каждая секция начинается и заканчивается на границе 4-КБ страницы,
   поэтому права страниц (текст - RX, rodata - только чтение, data/bss - RW, всё без исполнения) ставятся точно.
   ro_start/ro_end и text_start/text_end оставлены для immune.c: они хэшируют ровно содержимое секций без добивки. */
SECTIONS
{
    . = 1M;
    kernel_start = .;

    wx_ro_start = .;
    ro_start = .;
    .rodata : {
        KEEP(*(.multiboot_header))
        *(.rodata*)
    }
    ro_end = .;
    . = ALIGN(4096);
    wx_ro_end = .;

    wx_text_start = .;
    text_start = .;
    .text : {
        *(.text*)
    }
    text_end = .;
    . = ALIGN(4096);
    wx_text_end = .;

    wx_data_start = .;
    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4096);
    wx_data_end = .;
    kernel_end = .;
}
