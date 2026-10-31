bits 64

; ============================================================================
; Раскладка стека внутри общих заглушек (адреса растут вверх):
;
;   [rsp +   0 .. 112]  сохранённые r15 ... rax   (15 регистров по 8 байт)
;   [rsp + 120]         номер вектора             (push %1)
;   [rsp + 128]         код ошибки                (CPU или фиктивный 0)
;   [rsp + 136]         RIP                       (кадр прерывания CPU)
;   [rsp + 144]         CS
;   [rsp + 152]         RFLAGS
;   [rsp + 160]         RSP
;   [rsp + 168]         SS
; ============================================================================

; ----------------------------------------------------------------------------
; GS ядра. В режиме ядра GS_BASE = данные процессора (percpu), в KERNEL_GS_BASE лежит значение процесса (0).
; Программа в Ring 3 может перезаписать GS_BASE (mov gs, ax), поэтому при входе из Ring 3 делаем swapgs, при выходе - обратно.
; Флаг "swapgs сделан" лежит в двух словах под сохранёнными регистрами (второе слово - выравнивание стека).
; Для всего, что прервало ядро (CS = 0), решает сам GS_BASE (0 - GS процесса, у ядра он всегда не ноль):
; NMI, #DF и #MC приходят в окне между SYSCALL и swapgs, а ошибка ядра с GS процесса иначе превращалась в
; бесконечную рекурсию исключений (обработчик читает gs:0) и переполнение стека вместо понятного дампа.
; ----------------------------------------------------------------------------
%macro GS_ENTER 0
    pushfq                          ; RFLAGS.AC программы не должен доживать до ядра: при AC=1 SMAP не защищает
    and qword [rsp], ~(1 << 18)     ; (вход через шлюз прерывания AC не сбрасывает; clac есть не на всех ЦП)
    popfq
    xor eax, eax
    mov ecx, [rsp + 120]            ; номер вектора
    cmp ecx, 2
    je %%paranoid
    cmp ecx, 8
    je %%paranoid
    cmp ecx, 18
    je %%paranoid
    test qword [rsp + 144], 3       ; CS прерванного кода: RPL=3 - пришли из процесса
    jz %%paranoid                   ; из ядра - тоже по самому GS_BASE: если ядро оказалось с GS процесса (окно
                                    ; выхода, ошибка), обработчик не должен сам падать на gs:0 и уходить в рекурсию
    swapgs
    mov eax, 1
    jmp %%done
%%paranoid:
    mov ecx, 0xC0000101             ; IA32_GS_BASE
    rdmsr
    mov ecx, eax
    xor eax, eax
    or ecx, edx
    jnz %%done                      ; не ноль: GS уже ядерный
    swapgs
    mov eax, 1
    test qword [rsp + 144], 3
    jnz %%done
    lock inc qword [rel gs_fixups]  ; ядро прервано с GS процесса: запомнить где (диагностика, см. kernel.c)
    mov rcx, [rsp + 136]
    mov [rel gs_fixup_rip], rcx
    mov ecx, [rsp + 120]
    mov [rel gs_fixup_vec], ecx
%%done:
    push rax                        ; флаг
    push rax                        ; выравнивание (стек до вызова C должен быть кратен 16)
%endmacro

%macro GS_LEAVE 0
    add rsp, 8
    pop rax
    test eax, eax
    jz %%skip
    swapgs
%%skip:
%endmacro

section .data
global gs_fixups, gs_fixup_rip, gs_fixup_vec
gs_fixups:    dq 0
gs_fixup_rip: dq 0
gs_fixup_vec: dd 0, 0

section .text

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0
    push %1
    jmp interrupt_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1
    jmp interrupt_common_stub
%endmacro

; Аппаратные IRQ0..IRQ15 (векторы 32..47 после ремапа PIC)
%macro IRQ 1
global irq%1
irq%1:
    push 0
    push 32 + %1
    jmp irq_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

; Все остальные векторы (48..255): "неожиданное прерывание", вектор 255.
global isr_unhandled
isr_unhandled:
    push 0
    push 255
    jmp interrupt_common_stub

; Межъядерный пинг (вектор 0xF0): ipi_dispatch(vector); GS и регистры - как в irq_common_stub
global isr_ipi_ping
isr_ipi_ping:
    push 0
    push 0xF0
    jmp ipi_common_stub

global isr_ipi_resched
isr_ipi_resched:
    push 0
    push 0xF1
    jmp ipi_common_stub

; Ложное прерывание Local APIC: EOI слать нельзя, регистры не трогаем.
global isr_ipi_tlb
isr_ipi_tlb:
    push 0
    push 0xF2
    jmp ipi_common_stub

global isr_spurious
isr_spurious:
    iretq

extern exception_handler_main
extern irq_dispatch
extern ipi_dispatch

; Исключения CPU: exception_handler_main(vector, error_code, rip, cr2, cs, frame)
interrupt_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    GS_ENTER
    cld
    mov rdi, [rsp + 136]        ; вектор (под ним два служебных слова GS)
    mov rsi, [rsp + 144]        ; код ошибки
    mov rdx, [rsp + 152]        ; RIP
    mov rcx, cr2                ; адрес page fault
    mov r8, [rsp + 160]         ; CS прерванного кода: RPL=3 - исключение в процессе
    lea r9, [rsp + 16]          ; кадр целиком (регистры, вектор, код ошибки, RIP, CS, RFLAGS, RSP, SS) для дампа
    call exception_handler_main
    GS_LEAVE

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                 ; снять вектор и код ошибки
    iretq                       ; RFLAGS (в т.ч. IF) восстанавливает iretq

; Аппаратные IRQ: irq_dispatch(vector) - маршрутизация и EOI делаются в C (pic.c)
irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    GS_ENTER
    cld
    mov rdi, [rsp + 136]        ; вектор
    mov rsi, [rsp + 160]        ; CS прерванного кода: из Ring 3 - проверить, не завершается ли процесс
    mov rdx, [rsp + 152]        ; RIP прерванного кода (диагностика зависаний)
    mov rcx, [rsp + 80]         ; его RBP (сохранён среди регистров: 64 + 16 байт флага GS)
    call irq_dispatch
    GS_LEAVE

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; IPI: ipi_dispatch(vector) - счёт и EOI в C (smp.c)
ipi_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    GS_ENTER
    cld
    mov rdi, [rsp + 136]        ; вектор
    mov rsi, [rsp + 160]        ; CS прерванного кода
    call ipi_dispatch
    GS_LEAVE

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; ----------------------------------------------------------------------------
; Вход SYSCALL (включается syscall_init(); kernel-стек потока задаёт proc_arch_switch).
;
; ABI Kira: rax = номер, rdi/rsi/rdx = аргументы 1..3, результат в rax.
; Портятся: rcx, r11 (аппаратно), а также rdi, rsi, rdx, r8-r10.
; На входе rsp - ПОЛЬЗОВАТЕЛЬСКИЙ, ему нельзя доверять, поэтому первым делом
; переключаемся на kernel-стек из percpu.syscall_kernel_rsp (свои значения у каждого ядра). Оба значения сразу
; кладутся на kernel-стек, поэтому вытеснение внутри вызова глобальные слоты не портит.
; ----------------------------------------------------------------------------
extern syscall_handler
extern syscall_bad_return
global syscall_entry_asm
syscall_entry_asm:
    swapgs                      ; GS процесса -> GS ядра (данные этого процессора)
    mov [gs:8], rsp             ; percpu.syscall_user_rsp (GS - данные этого процессора)
    mov rsp, [gs:16]            ; percpu.syscall_kernel_rsp
    push qword [gs:8]
    push rcx                    ; пользовательский RIP
    push r11                    ; пользовательский RFLAGS
    push 0                      ; выравнивание стека по 16 байт перед call
    mov r11, rcx                ; адрес возврата должен быть каноническим и пользовательским: sysret с
    shr r11, 47                 ; неканоническим RCX бросает #GP уже в Ring 0, но на пользовательском стеке
    jnz .bad_return             ; (известная уязвимость SYSRET на Intel). Такой процесс завершается.
    mov rcx, rdx                ; arg3
    mov rdx, rsi                ; arg2
    mov rsi, rdi                ; arg1
    mov rdi, rax                ; номер вызова
    cld
    sti                         ; со входа (FMASK) IF=0 только пока rsp переключается; обработчик вытесняем,
                                ; иначе долгий вызов глушил бы таймер и ввод
    call syscall_handler
    cli                         ; обратно на пользовательский стек только с выключенными прерываниями
    add rsp, 8
    pop r11
    pop rcx
    xor edi, edi                ; не отдавать процессу ядерные значения в портящихся регистрах
    xor esi, esi
    xor edx, edx
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    swapgs                      ; GS ядра -> GS процесса (до этого места GS ядерный)
    pop rsp                     ; вернуть пользовательский стек
    o64 sysret
.bad_return:
    sti
    call syscall_bad_return     ; не возвращается: процесс завершается

; void enter_user(uint64_t rip, uint64_t rsp): переход в Ring 3 через iretq.
; Все регистры обнуляются: процесс не должен видеть значения ядра.
; void enter_user_arg(rip, rsp, arg): то же, но первый аргумент функции потока (rdi) = arg.
global enter_user_arg
enter_user_arg:
    mov r8, rdx
    jmp enter_user_common
global enter_user
enter_user:
    xor r8d, r8d
enter_user_common:
    cli
    mov ax, 0x1B                ; пользовательский сегмент данных (0x18 | RPL 3)
    mov ds, ax
    mov es, ax
    push qword 0x1B             ; SS
    push rsi                    ; RSP
    push qword 0x202            ; RFLAGS: IF=1, IOPL=0
    push qword 0x23             ; CS (0x20 | RPL 3)
    push rdi                    ; RIP
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    mov rdi, r8                 ; аргумент потока (0 у главного)
    xor r8d, r8d
    swapgs                      ; в Ring 3 уходим с GS процесса; ядерный лежит в KERNEL_GS_BASE
    iretq

global isr_stub_table
isr_stub_table:
    dq isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    dq isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
    dq irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15

; MSI / MSI-X: векторы 0x50..0x5F, тот же путь, что у аппаратных IRQ (irq_dispatch различает по вектору)
%macro MSI 1
global msi%1
msi%1:
    push 0
    push 0x50 + %1
    jmp irq_common_stub
%endmacro
MSI 0
MSI 1
MSI 2
MSI 3
MSI 4
MSI 5
MSI 6
MSI 7
MSI 8
MSI 9
MSI 10
MSI 11
MSI 12
MSI 13
MSI 14
MSI 15
global msi_stub_table
msi_stub_table:
    dq msi0, msi1, msi2, msi3, msi4, msi5, msi6, msi7
    dq msi8, msi9, msi10, msi11, msi12, msi13, msi14, msi15

global load_idt
load_idt:
    lidt [rdi]
    ret

section .bss
align 16

section .note.GNU-stack noalloc noexec nowrite progbits
