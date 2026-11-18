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

%define EARLY_PT_MAX 10          ; страниц таблиц в пуле: образ до 16 МБ + низкая область + мост KASLR (earlymap.h: EARLYMAP_PT_MAX)
%define EARLY_TRAMP  0x8000      ; страница трамплина AP (smp.h: SMP_TRAMP_BASE) - единственная исполняемая вне образа

section .bss
align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table:
    resb 4096                    ; Один PD = 512 областей по 2 МБ = 1 ГБ
early_pt_pool:
    resb 4096 * EARLY_PT_MAX     ; 4-КБ таблицы: страница N пула - область N (образ ядра по секциям, раннее W^X)
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
global pml4_table, pdpt_table, pd_table, early_pt_pool
global mb_magic, mb_info, uefi_info, uefi_size, uefi_mode
extern kernel_main
extern kernel_uefi_entry
extern kernel_end                ; конец образа ядра (linker.ld)
extern wx_ro_start, wx_ro_end, wx_text_start, wx_text_end    ; границы секций (linker.ld): права страниц с первой же инструкции
extern kaslr_setup, kaslr_finish ; arch/x86_64/kaslr_arch.c

; Ранняя карта страниц с W^X. Строится ДО включения страничной адресации и первой строки C-кода, поэтому ни одна страница
; образа не бывает записываемой и исполняемой одновременно с момента, когда процессор вообще начинает проверять права:
;   PML4[0] -> PDPT[0] -> PD (1 ГБ, identity); области 0..R-1 (R - сколько 2-МБ областей накрывает образ, включая
;   область 0 с трамплином AP) - 4-КБ таблицы из early_pt_pool, остальные области - 2-МБ страницы RW+NX.
;   Права 4-КБ страницы: текст - RX; rodata - R+NX; страница трамплина - RX; всё остальное (data, bss, стеки, таблицы,
;   ниже 1 МБ, зазоры) - RW+NX. То же правило применяют earlymap.c (карта перемещённого образа) и wx_enforce (позже).
; Сам по себе код макроса одинаков в 32- и 64-битном режиме (регистры eax..edi, абсолютные адреса ниже 2 ГБ).
%macro EARLY_TABLES 0
    mov eax, pdpt_table
    or eax, 3
    mov [pml4_table], eax
    mov eax, pd_table
    or eax, 3
    mov [pdpt_table], eax
    mov esi, kernel_end
    add esi, 0x1FFFFF
    shr esi, 21                  ; esi = R
    cmp esi, EARLY_PT_MAX
    ja early_too_big
    xor ecx, ecx
%%pd:
    cmp ecx, esi
    jae %%huge
    mov eax, ecx
    shl eax, 12                  ; страница пула = область ecx
    add eax, early_pt_pool
    or eax, 3                    ; P | W (права решает 4-КБ страница)
    mov [pd_table + ecx * 8], eax
    mov dword [pd_table + ecx * 8 + 4], 0
    jmp %%pd_next
%%huge:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                 ; P | W | PS (2 МБ)
    mov [pd_table + ecx * 8], eax
    mov dword [pd_table + ecx * 8 + 4], 0x80000000       ; NX: бит 63 (EFER.NXE включается до страничной адресации)
%%pd_next:
    inc ecx
    cmp ecx, 512
    jb %%pd
    mov edi, esi
    shl edi, 9                   ; страниц в R областях
    xor ecx, ecx
%%pt:
    mov eax, ecx
    shl eax, 12                  ; адрес страницы
    mov edx, 0x80000000          ; по умолчанию NX
    cmp eax, wx_text_start
    jb %%not_text
    cmp eax, wx_text_end
    jae %%not_text
    xor edx, edx                 ; текст: RX
    or eax, 1
    jmp %%store
%%not_text:
    cmp eax, wx_ro_start
    jb %%not_ro
    cmp eax, wx_ro_end
    jae %%not_ro
    or eax, 1                    ; rodata: R+NX
    jmp %%store
%%not_ro:
    cmp eax, EARLY_TRAMP
    jne %%rw
    xor edx, edx                 ; трамплин AP: RX (пишут в него через отдельное окно, vmm_code_window_open)
    or eax, 1
    jmp %%store
%%rw:
    or eax, 3                    ; RW+NX
%%store:
    mov [early_pt_pool + ecx * 8], eax
    mov [early_pt_pool + ecx * 8 + 4], edx
    inc ecx
    cmp ecx, edi
    jb %%pt
%endmacro

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

    ; Higher-half НЕ реализован: ядро работает в identity-карте (виртуальный адрес = физический, на этом стоит весь DMA).
    ; Случайный адрес базы (KASLR) - физический сдвиг образа при загрузке, см. kaslr_setup ниже и docs/architecture/KASLR_WX.md.
    EARLY_TABLES

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
    or eax, 0x80010001          ; PG | WP | PE: WP - чтобы запрет записи действовал и для самого ядра (RO-страницы образа)
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
    EARLY_TABLES                ; та же ранняя карта с W^X, что в BIOS-пути
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
    mov edx, 0x80010001         ; PG | WP | PE
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

    ; KASLR: kaslr_setup (C, arch/x86_64/kaslr_arch.c; печатать и трогать данные ядра она не должна) решает, куда
    ; поставить образ. 0 - остаёмся на месте (отключено, нет таблицы перемещений, нет места: причина - в журнале ниже).
    ; Иначе образ уже скопирован по новому адресу, исправлен и для него построена своя карта страниц; rax = сдвиг.
    mov rdi, [rel mb_info]       ; Multiboot2: адрес структуры; UEFI: адрес BootInfo (ниже)
    xor esi, esi
    xor edx, edx
    mov ecx, [rel mb_magic]
    cmp dword [rel uefi_mode], 0
    je .ks_call
    mov edi, [rel uefi_info]
    mov esi, [rel uefi_size]
    mov edx, 1
.ks_call:
    call kaslr_setup
    test rax, rax
    jz long_mode_dispatch
    mov rcx, rax                 ; сдвиг
    ; Переменные входа - в копию (её bss обнулена), затем переход через мост: страницу кода, исполняемую в обеих картах
    mov rax, [rel mb_magic]
    lea rdi, [rel mb_magic]
    mov [rdi + rcx], rax
    mov rax, [rel mb_info]
    lea rdi, [rel mb_info]
    mov [rdi + rcx], rax
    mov eax, [rel uefi_info]
    lea rdi, [rel uefi_info]
    mov [rdi + rcx], eax
    mov eax, [rel uefi_size]
    lea rdi, [rel uefi_size]
    mov [rdi + rcx], eax
    mov eax, [rel uefi_mode]
    lea rdi, [rel uefi_mode]
    mov [rdi + rcx], eax
    lea rdi, [rel pml4_table]
    add rdi, rcx                 ; CR3 копии
    lea rsi, [rel stack_top]
    add rsi, rcx                 ; стек копии
    lea rdx, [rel kaslr_resume]
    add rdx, rcx                 ; продолжение в копии
    jmp kaslr_bounce

; Копия образа: сюда приходит мост с картой страниц копии (образ по секциям RX/R/RW+NX) и её стеком.
kaslr_resume:
    lgdt [rel gdt64.pointer]     ; GDT копии (старая лежит в освобождаемом образе)
    and rsp, -16
    call kaslr_finish            ; закрывает мост: страница кода в старом образе снова обычные данные RW+NX
    jmp long_mode_dispatch

long_mode_dispatch:
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

early_too_big:                   ; образ ядра больше пула early_pt_pool (16 МБ): карту не построить, остановка
hang:
    cli
    hlt
    jmp hang

; Мост KASLR: одна страница кода (выровнена по 4 КБ и дополнена до целой страницы), которая в карте копии отображена
; исполняемой по ТОМУ ЖЕ адресу, что и в старом образе. Переключение CR3 происходит здесь: инструкция после mov cr3
; выбирается уже по новой карте, а на старом месте страница исполняема только благодаря мосту. kaslr_finish потом
; закрывает его. Вход: rdi = CR3 копии, rsi = стек копии, rdx = адрес продолжения в копии.
section .text.kaslr_bounce progbits alloc exec nowrite align=4096
global kaslr_bounce
kaslr_bounce:
    db 0x0f, 0x22, 0xdf         ; mov cr3, rdi (NASM в этой сборке не принимает MOV CRx в 64-битном режиме)
    mov rsp, rsi
    xor ebp, ebp
    jmp rdx
    times 4096 - ($ - kaslr_bounce) db 0xCC

section .rodata
; Бит доступа (A, бит 40) у дескрипторов выставлен заранее: при загрузке селектора процессор тогда ничего не пишет в
; GDT, а она лежит в rodata (R+NX) при включённом CR0.WP.
gdt64:
    dq 0                        ; Null descriptor
.code:
    dq (1<<40) | (1<<43) | (1<<44) | (1<<47) | (1<<53) ; 64-bit code segment
.data:
    dq (1<<40) | (1<<41) | (1<<44) | (1<<47)            ; 64-bit data segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .note.GNU-stack noalloc noexec nowrite progbits
