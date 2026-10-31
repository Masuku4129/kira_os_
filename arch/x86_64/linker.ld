ENTRY(start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    ro_start = .;
    .rodata : {
        KEEP(*(.multiboot_header))
        *(.rodata*)
    }
    ro_end = .;

    text_start = .;
    .text : {
        *(.text*)
    }
    text_end = .;

    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4096);
    kernel_end = .;
}
