bits 16
org 0x7C00
%include "bios_layout.inc"

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    ; Require INT 13h extensions (EDD), used for LBA reads.
    mov bx, 0x55AA
    mov ah, 0x41
    mov dl, [boot_drive]
    int 0x13
    jc boot_error
    cmp bx, 0xAA55
    jne boot_error
    test cx, 1
    jz boot_error

    xor ax, ax
    mov ds, ax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc boot_error
    mov dl, [boot_drive]
    jmp 0x0000:0x8000

boot_error:
    xor ax, ax
    mov ds, ax
    mov si, error_text
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
error_text db "KIRA BIOS: EDD stage2 read failed",0
align 4
dap:
    db 0x10,0
    dw STAGE2_SECTORS
    dw 0
    dw 0x0800
    dq 1

times 510-($-$$) db 0
dw 0xAA55
