; KIRA BIOS stage 1 для компакт-диска (El Torito без эмуляции): BIOS загружает 17 КиБ образа (этот сектор + 512 байт запаса + stage 2) с адреса 0000:7C00.
; Stage 2 собран с org 0x8000, т.е. лежит в образе со смещения 0x400. Номер привода остаётся в DL.
bits 16
org 0x7C00
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [0x0500], dl
    jmp 0x0000:0x8000
times 1024 - ($ - $$) db 0
