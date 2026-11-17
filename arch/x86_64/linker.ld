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

    /* Таблица перемещений для KASLR (tools/mkkreloc.py кладёт её сюда второй линковкой): последняя часть, лежащая в файле.
       kernel_loaded_end - сколько байт образа копируется при перемещении (таблица сама не копируется). Без таблицы
       (ядро собрано другим способом) секция пуста, и ядро честно остаётся на месте. */
    . = ALIGN(8);
    kernel_loaded_end = .;
    kreloc_start = .;
    .kreloc : {
        KEEP(*(.kreloc))
    }
    kreloc_end = .;

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4096);
    wx_data_end = .;
    kernel_end = .;
}
