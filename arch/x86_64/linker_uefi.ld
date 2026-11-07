ENTRY(uefi_entry)

SECTIONS
{
    . = 1M;
    kernel_start = .;
    ro_start = .;
    . = ALIGN(4096);
    .rodata : {
        KEEP(*(.multiboot_header))
        *(.rodata*)
    }
    ro_end = .;
    . = ALIGN(4096);
    text_start = .;
    .text : { *(.text*) }
    text_end = .;
    . = ALIGN(4096);
    .data : { *(.data*) }
    . = ALIGN(4096);
    .bss : {
        *(COMMON)
        *(.bss*)
    }
    . = ALIGN(4096);
    kernel_end = .;
}
