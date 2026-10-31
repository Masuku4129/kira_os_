/* Kira OS, ARM64: вход в ядро. Загрузчик (QEMU -kernel, U-Boot, прошивка) передаёт адрес дерева устройств в x0
   (так велит протокол загрузки образа ARM64). Ядро может прийти на EL3, EL2 или EL1: работает на
   EL1, поэтому с более высокого уровня спускается, разрешив EL1 64-битный режим и прямой доступ к таймеру.
   Остальные ядра стартуют позже через PSCI CPU_ON (secondary_entry). */

    .section .text.boot, "ax"
    .global _start
/* Заголовок образа ARM64 (как у Linux): загрузчики (QEMU -kernel, U-Boot booti, прошивки плат) кладут образ по
   2 МБ-выровненному адресу + text_offset и передают адрес дерева устройств в x0. Флаги: little-endian, страницы
   4 КБ, размещение где угодно - ядро позиционно-независимо и само правит свои указатели (R_AARCH64_RELATIVE). */
_start:
    b       real_start                  // code0
    .long   0                           // code1
    .quad   0x80000                     // text_offset
    .quad   __image_size                // image_size (с .bss)
    .quad   0xA                         // flags
    .quad   0, 0, 0
    .ascii  "ARM\x64"                   // magic
    .long   0

real_start:
    mov     x19, x0                     // адрес дерева устройств
    bl      drop_to_el1
    // Перемещение: ядро собрано с адреса 0, исполняется там, куда положили; поправить абсолютные указатели
    adr     x20, _start                 // база = смещение загрузки
    adrp    x0, __rela_start
    add     x0, x0, :lo12:__rela_start
    adrp    x1, __rela_end
    add     x1, x1, :lo12:__rela_end
5:  cmp     x0, x1
    b.hs    6f
    ldp     x2, x3, [x0], #16           // r_offset, r_info
    ldr     x4, [x0], #8                // r_addend
    cmp     w3, #1027                   // R_AARCH64_RELATIVE
    b.ne    5b
    add     x4, x4, x20
    str     x4, [x20, x2]
    b       5b
6:  msr     spsel, #1                   // свой стек EL1 (SP_EL1): на нём же работают обработчики исключений
    adrp    x0, boot_stack_top
    add     x0, x0, :lo12:boot_stack_top
    mov     sp, x0
    // .bss - нулями (компоновщик выравнивает границы на 16)
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:  cmp     x0, x1
    b.hs    2f
    stp     xzr, xzr, [x0], #16
    b       1b
2:  bl      cpu_common
    mov     x0, x19
    bl      arm_main
3:  wfe
    b       3b

/* Вторичное ядро: PSCI CPU_ON передаёт context_id в x0 - вершину стека, выделенного основным ядром. */
    .global secondary_entry
secondary_entry:
    mov     x19, x0
    bl      drop_to_el1
    msr     spsel, #1
    mov     sp, x19
    bl      cpu_common
    bl      arm_secondary_main
4:  wfe
    b       4b

/* Общее для каждого ядра: вектора исключений, доступ к FP/SIMD (ядро его не использует - запрет до программ). */
cpu_common:
    adrp    x0, vectors
    add     x0, x0, :lo12:vectors
    msr     vbar_el1, x0
    msr     cpacr_el1, xzr
    isb
    ret

/* С EL3/EL2 на EL1h с замаскированными прерываниями. */
drop_to_el1:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #1
    b.eq    9f
    cmp     x0, #2
    b.eq    8f
    // EL3: EL2 и ниже - небезопасный мир, 64 бита
    mov     x0, #(1 << 10) | (1 << 0)   // SCR_EL3.RW | NS
    orr     x0, x0, #(1 << 8)           // HCE: на EL2 доступен hvc
    msr     scr_el3, x0
    mov     x0, #0x3c9                  // EL2h, DAIF замаскированы
    msr     spsr_el3, x0
    adr     x0, 8f
    msr     elr_el3, x0
    eret
8:  // EL2: EL1 - 64 бита, таймер и счётчик EL1 доступны напрямую, без смещения виртуального счётчика
    mov     x0, #(1 << 31)              // HCR_EL2.RW
    msr     hcr_el2, x0
    mov     x0, #3                      // CNTHCTL_EL2.EL1PCTEN | EL1PCEN
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0x33ff                 // CPTR_EL2: RES1 и TFP=0 - FP/SIMD не ловить на EL2
    msr     cptr_el2, x0
    mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0
    mov     x0, #0x3c5                  // EL1h, DAIF замаскированы
    msr     spsr_el2, x0
    msr     elr_el2, x30
    eret
9:  ret

/* ---- Вектора исключений: 16 входов по 0x80 байт. Кадр: x0..x30, elr, spsr, esr, far (36 слов). ---- */
    .macro VEC kind
    .balign 0x80                        // вход - ровно 0x80 байт: только сохранить x0/x1 и уйти в общий обработчик
    sub     sp, sp, #288
    stp     x0, x1, [sp, #0]
    mov     x1, #\kind
    b       exc_common
    .endm

    .balign 0x800
    .global vectors
vectors:
    VEC 0       // текущий EL, SP_EL0: синхронное
    VEC 1       //   IRQ
    VEC 2       //   FIQ
    VEC 3       //   SError
    VEC 4       // текущий EL, SP_ELx
    VEC 5
    VEC 6
    VEC 7
    VEC 8       // нижний EL, AArch64
    VEC 9
    VEC 10
    VEC 11
    VEC 12      // нижний EL, AArch32
    VEC 13
    VEC 14
    VEC 15

/* Кадр: x0..x30, elr, spsr, esr, far (36 слов, 288 байт). x0/x1 уже сохранены входом, в x1 - номер входа. */
exc_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x0, elr_el1
    stp     x30, x0, [sp, #240]
    mrs     x0, spsr_el1
    mrs     x2, esr_el1
    stp     x0, x2, [sp, #256]
    mrs     x0, far_el1
    str     x0, [sp, #272]
    mov     x0, sp
    bl      arm_exception
    ldp     x0, x1, [sp, #256]
    msr     spsr_el1, x0
    ldp     x30, x0, [sp, #240]
    msr     elr_el1, x0
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #288
    eret

    .section .bss
    .balign 16
boot_stack:
    .skip 16384
boot_stack_top:
