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
uefi_info:
    resd 1
uefi_size:
    resd 1
uefi_mode:
    resd 1

section .text
global start
global stack_bottom
global stack_top
extern kernel_main
extern kernel_uefi_entry
extern kernel_end                ; конец образа ядра (linker.ld): выше него страницы раннего отображения получают NX

start:
    cli
    ; ELF entry точка одна, а входят в неё в разных режимах: GRUB и BIOS stage2 - в 32-bit
    ; protected mode, UEFI-загрузчик - уже в 64-bit long mode (Microsoft x64 ABI). Код ниже
    ; собран как 32-битный, поэтому в 64-битном режиме те же байты декодируются иначе: например
    ; "mov [mem], eax" превращается в movabs по 8-байтному адресу или в запись относительно RIP.
    ; Поэтому первые 16 байт безопасны в обоих режимах и разводят пути сами:
    ;   32-bit: dec ecx; mov esi, imm32; jmp short .protected_entry
    ;   64-bit: movabs r14, imm64 (REX.W+REX.B 0x49); jmp .uefi_entry
    ; ecx и esi не несут данных ни при входе из Multiboot2, ни из BIOS stage2; RCX/RDX
    ; (аргументы UEFI) не затрагиваются.
.dual_mode_prologue:
    db 0x49, 0xBE                ; 32-bit: dec ecx; mov esi, imm32 / 64-bit: movabs r14, imm64
    db 0x90, 0x90, 0x90, 0x90    ; imm32 / младшая половина imm64
    db 0xEB, 0x07                ; 32-bit: jmp short .protected_entry (старшая половина imm64)
    db 0x90, 0x90
    jmp near .uefi_entry         ; исполняется только в 64-bit режиме
%if ($ - .dual_mode_prologue) != 15
%error "dual-mode entry prologue changed size; fix the short jump above"
%endif
.protected_entry:
    mov [mb_magic], eax
    mov [mb_info], ebx

    mov esp, stack_top

    ; Настройка пейджинга для Long Mode (identity map первого 1 ГБ)
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax

    ; Higher-half НЕ реализован: ядро слинковано по адресу 32 МиБ и работает в
    ; identity-карте. Настоящий higher-half - отдельный этап (смена VMA в linker.ld).

    mov eax, pd_table
    or eax, 3
    mov [pdpt_table], eax

    ; Заполняем 512 огромных страниц по 2 МБ (итого 1 ГБ identity map). Страницы целиком выше образа ядра
    ; (kernel_end из linker.ld, округлено вверх до 2 МБ) получают NX уже здесь, до первой строки C-кода: исполняемыми
    ; остаются только страницы, где лежит образ (2-МБ страницу под ним на этом этапе не раздробить). Бит NX требует
    ; EFER.NXE - он включается ниже вместе с LME. Образ дробит на 4-КБ страницы и ставит права по секциям
    ; vmm_enforce_wx (vmm.c: текст RX, rodata R+NX, остальное RW+NX).
    mov edx, kernel_end
    add edx, 0x1FFFFF
    shr edx, 21                 ; первая 2-МБ страница, целиком лежащая выше образа
    mov ecx, 0
.map_pages:
    mov eax, ecx
    shl eax, 21                 ; Умножаем на 2 МБ (1 << 21)
    or eax, 0x83                ; Present + Writable + Huge (2MB)
    mov [pd_table + ecx * 8], eax
    mov dword [pd_table + ecx * 8 + 4], 0
    cmp ecx, edx
    jb .map_keep_x
    mov dword [pd_table + ecx * 8 + 4], 0x80000000   ; бит 63 - NX
.map_keep_x:
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
    or eax, 0x900               ; LME (1 << 8) | NXE (1 << 11): страницы выше образа уже помечены NX
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
.uefi_entry:
    ; UEFI уже включил long mode. Аргументы по Microsoft x64 ABI: RCX=KBI, RDX=size
    ; (сохраняем до RDMSR, который их затирает). Пересобираем минимальную identity-карту
    ; ядра и переходим в общий long_mode_start, как BIOS/Multiboot2.
    mov dword [rel uefi_info], ecx
    mov dword [rel uefi_size], edx
    mov dword [rel uefi_mode], 1
    mov esp, stack_top
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax
    mov eax, pd_table
    or eax, 3
    mov [pdpt_table], eax
    mov edx, kernel_end         ; как в BIOS-пути: выше образа ядра - NX
    add edx, 0x1FFFFF
    shr edx, 21
    mov ecx, 0
.uefi_map_pages:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [pd_table + ecx * 8], eax
    mov dword [pd_table + ecx * 8 + 4], 0
    cmp ecx, edx
    jb .uefi_keep_x
    mov dword [pd_table + ecx * 8 + 4], 0x80000000
.uefi_keep_x:
    inc ecx
    cmp ecx, 512
    jl .uefi_map_pages
    ; NASM's long-mode parser rejects MOV CRx in this build configuration.
    ; Emit the architecturally defined 0F 20/22 encodings explicitly.
    ; MOV RAX, CR4; OR RAX, PAE; MOV CR4, RAX.
    db 0x0f, 0x20, 0xe0
    or rax, 32
    db 0x0f, 0x22, 0xe0
    mov eax, pml4_table
    ; MOV CR3, RAX (zero-extended low physical address).
    db 0x0f, 0x22, 0xd8
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x900               ; LME | NXE
    wrmsr
    ; MOV RAX, CR0; enable paging and protected mode; MOV CR0, RAX.
    db 0x0f, 0x20, 0xc0
    mov edx, 0x80000001
    or rax, rdx
    db 0x0f, 0x22, 0xc0
    lgdt [gdt64.pointer]
    push qword 0x08
    lea rax, [rel long_mode_start]
    push rax
    retfq

long_mode_start:
    ; Инициализация сегментов данных
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Выравнивание стека по 16 байт
    mov esp, stack_top
    and rsp, -16
    cmp dword [rel uefi_mode], 0
    jne .call_uefi
    ; Multiboot2: System V ABI (RDI=magic, RSI=info pointer).
    mov edi, [rel mb_magic]
    mov rsi, [rel mb_info]
    call kernel_main
    jmp hang

.call_uefi:
    ; UEFI KBI pointer and size were saved from RCX/RDX before RDMSR.
    mov edi, [rel uefi_info]
    mov esi, [rel uefi_size]
    call kernel_uefi_entry

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
