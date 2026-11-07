bits 32

section .multiboot_header
align 8
multiboot_header_start:
    dd 0xE85250D6                ; Magic number (Multiboot 2)
    dd 0                         ; Architecture (0 = 32-bit protected mode)
    dd multiboot_header_end - multiboot_header_start
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header_start))
    
    ; Запрос линейного фреймбуфера 1024x768x32 (флаг optional=1: если режим недоступен,
    ; GRUB всё равно загрузит ядро, и оно продолжит работать через UART)
    align 8
    dw 5                         ; type = framebuffer
    dw 1                         ; flags: optional
    dd 20                        ; size
    dd 1024                      ; width
    dd 768                       ; height
    dd 32                        ; depth
    dd 0                         ; выравнивание тега до 8 байт

    ; Required end tag
    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss
align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table:
    resb 4096                    ; Один PD = 512 страниц по 2 МБ = 1 ГБ
stack_bottom:
    resb 16384
stack_top:

; Multiboot2 magic и физический адрес info-структуры (qword, старшие 32 бита = 0)
mb_magic:
    resq 1
mb_info:
    resq 1

section .text
global start
global stack_bottom
global stack_top
extern kernel_main

start:
    cli
    mov esp, stack_top

    ; Сохраняем Multiboot magic (eax) и pointer (ebx) в память.
    ; (bss обнулён загрузчиком, старшие 32 бита qword остаются нулевыми)
    mov [mb_magic], eax
    mov [mb_info], ebx

    ; Настройка пейджинга для Long Mode (identity map первого 1 ГБ)
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax

    ; Higher-half НЕ реализован: ядро слинковано по адресу 1 МБ и работает в
    ; identity-карте. Настоящий higher-half - отдельный этап (смена VMA в linker.ld).

    mov eax, pd_table
    or eax, 3
    mov [pdpt_table], eax

    ; Заполняем 512 огромных страниц по 2 МБ (итого 1 ГБ identity map)
    mov ecx, 0
.map_pages:
    mov eax, ecx
    shl eax, 21                 ; Умножаем на 2 МБ (1 << 21)
    or eax, 0x83                ; Present + Writable + Huge (2MB)
    mov [pd_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jl .map_pages

    ; Включаем PAE
    mov eax, cr4
    or eax, 32                  ; 1 << 5
    mov cr4, eax

    ; Загружаем PML4 в CR3
    mov eax, pml4_table
    mov cr3, eax

    ; Включаем Long Mode в MSR EFER
    mov ecx, 0xC0000080
    rdmsr
    or eax, 256                 ; 1 << 8
    wrmsr

    ; Включаем пейджинг и защищенный режим в CR0
    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax

    ; Загружаем 64-битную GDT
    lgdt [gdt64.pointer]

    ; Дальний прыжок в 64-битный сегмент кода
    jmp 0x08:long_mode_start

bits 64
; Native UEFI enters the already-long-mode kernel using the Microsoft x64 ABI:
; RCX = KIRA_BOOTINFO_MAGIC, RDX = physical pointer to the validated KBI blob.
global uefi_entry
uefi_entry:
    cli
    mov r8, rcx                 ; preserve Multiboot2 magic across the table loop
    mov r9, rdx                 ; preserve the MBI physical pointer
    ; Do not rely on firmware selectors/GDT after ExitBootServices. Load
    ; KIRA's own descriptors and reload CS before entering kernel C code.
    lgdt [rel gdt64.pointer]
    push qword 0x08
    lea rax, [rel .uefi_cs_ready]
    push rax
    retfq
.uefi_cs_ready:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; UEFI page tables are firmware-owned. Install KIRA's 1 GiB identity map
    ; before switching to the kernel stack or touching physical boot data.
    mov rax, pdpt_table
    or rax, 3
    mov [pml4_table], rax
    mov rax, pd_table
    or rax, 3
    mov [pdpt_table], rax
    xor ecx, ecx
.uefi_map_pages:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [pd_table + rcx * 8], rax
    inc ecx
    cmp ecx, 512
    jl .uefi_map_pages
    mov rax, cr4
    or rax, 32
    mov cr4, rax
    mov rax, pml4_table
    mov cr3, rax
    mov rsp, stack_top
    and rsp, -16
    mov rdi, r8
    mov rsi, r9
    call kernel_main
.uefi_hang:
    cli
    hlt
    jmp .uefi_hang

long_mode_start:
    ; Инициализация сегментов данных
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Восстанавливаем аргументы Multiboot в регистры ABI (rdi, rsi)
    mov edi, [mb_magic]         ; запись в 32-битный регистр обнуляет старшую половину rdi
    mov esi, [mb_info]

    ; Выравнивание стека по 16 байт
    and rsp, -16

    ; Вызов точки входа ядра на C
    call kernel_main

hang:
    cli
    hlt
    jmp hang

section .rodata
gdt64:
    dq 0                        ; Null descriptor
.code:
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; 64-bit code segment
.data:
    dq (1<<41) | (1<<44) | (1<<47)            ; 64-bit data segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .note.GNU-stack noalloc noexec nowrite progbits
