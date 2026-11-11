ENTRY(start)

/* Границы секций для W^X: каждая секция выровнена по странице.
   Ядро начинается с 32 МиБ: UEFI firmware может фрагментировать первые 16 МиБ
   физической памяти; BIOS stage2 и Multiboot2 загружают ELF PT_LOAD по p_paddr.
   Ранняя identity-карта покрывает первые 1 ГиБ. */
SECTIONS
{
    . = 32M;
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
