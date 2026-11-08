; KIRA BIOS stage 1: MBR (16 бит). LBA 0 - этот сектор, LBA 1..32 - stage 2 (16 КиБ, загружается в 0000:8000).
; Нужны расширения INT 13h (EDD). Номер диска остаётся в DL и в [0x0500].
bits 16
org 0x7C00
STAGE2_SECTORS equ 32
STAGE2_ADDR    equ 0x8000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive], dl
    mov [0x0500], dl
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error
    test cx, 1
    jz disk_error
    mov si, dap
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov dl, [drive]
    jmp 0x0000:STAGE2_ADDR

disk_error:
    mov si, err_text
.next:
    lodsb
    test al, al
    jz halt
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .next
halt:
    cli
    hlt
    jmp halt

drive: db 0
align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw STAGE2_ADDR, 0
    dq 1
err_text: db "KIRA: stage1 cannot read stage2 (EDD required)", 0

times 446 - ($ - $$) db 0
times 64 db 0
dw 0xAA55
