; Трамплин запуска остальных процессоров (AP). Копируется ядром на физический адрес TRAMP_BASE (страница ниже 1 МБ),
; туда указывает вектор SIPI: AP стартует в реальном режиме с CS = TRAMP_BASE >> 4, IP = 0.
; Путь: реальный режим -> защищённый 32-бит -> long mode с CR3 загрузочного процессора -> C-функция ядра на своём стеке.
; Все адреса внутри блока считаются от ap_tramp_start (блок собран в ELF, но исполняется по TRAMP_BASE).
%define TRAMP_BASE 0x8000
%define OFF(x) ((x) - ap_tramp_start)
%define ABS(x) (TRAMP_BASE + OFF(x))

section .text
global ap_tramp_start
global ap_tramp_end
global ap_tramp_params_off

ap_tramp_start:
bits 16
    cli
    cld
    mov ax, cs
    mov ds, ax
    lgdt [OFF(ap_gdtr)]
    mov eax, cr0
    or eax, 1                       ; PE
    mov cr0, eax
    jmp dword 0x08:ABS(ap_pm)

bits 32
ap_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr4
    or eax, 0x20                    ; PAE
    mov cr4, eax
    mov eax, [ABS(ap_params)]       ; CR3 загрузочного процессора (ниже 4 ГБ: ядро лежит около 1 МБ)
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, 0x900                   ; LME | NXE (ядро отображает память с бит NX)
    wrmsr
    mov eax, cr0
    or eax, 0x80000000              ; PG
    mov cr0, eax
    jmp dword 0x18:ABS(ap_lm)

bits 64
ap_lm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, [abs ABS(ap_params) + 8]    ; вершина стека этого процессора
    mov rdi, [abs ABS(ap_params) + 24]   ; аргумент: номер в таблице percpu
    mov rax, [abs ABS(ap_params) + 16]   ; C-функция входа
    xor ebp, ebp
    call rax
.hang:
    cli
    hlt
    jmp .hang

align 8
ap_gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: код 32-бит, плоский
    dq 0x00CF92000000FFFF           ; 0x10: данные, плоские
    dq 0x00AF9A000000FFFF           ; 0x18: код 64-бит
ap_gdt_end:
ap_gdtr:
    dw ap_gdt_end - ap_gdt - 1
    dd ABS(ap_gdt)

align 8
ap_params:                          ; cr3, stack_top, entry, arg (qword); заполняет ядро после копирования
    dq 0, 0, 0, 0
ap_tramp_end:

ap_tramp_params_off equ OFF(ap_params)

section .note.GNU-stack noalloc noexec nowrite progbits
