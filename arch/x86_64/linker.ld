ENTRY(start)

/* Границы секций для W^X: каждая секция выровнена по странице.
   Ядро начинается с 2 МиБ: первые 2 МиБ зарезервированы firmware/legacy
   областями на части UEFI-платформ. BIOS stage2 и Multiboot2 допускают
   физический адрес выше 1 МиБ; ранняя identity-карта покрывает первые 1 ГиБ. */
SECTIONS
{
    . = 2M;
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
