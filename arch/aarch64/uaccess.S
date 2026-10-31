/* Доступ ядра к памяти программ - только здесь, командами LDTR/STTR: они проверяют права так, как если бы обращалась
   сама программа (EL0), поэтому ядро не прочтёт и не испортит через указатель программы ни свою память, ни чужую
   страницу только для чтения, а включённый PAN закрывает память программ для любых других команд ядра.
   Ошибка доступа внутри [uaccess_begin, uaccess_end) не авария: обработчик переводит ELR на uaccess_fault -> -1. */
    .text
    .global uaccess_begin, uaccess_end, uaccess_fault, uaccess_copy_from, uaccess_copy_to, uaccess_read32
uaccess_begin:
/* int uaccess_copy_from(void* dst, const void* usrc, uint64_t n) */
uaccess_copy_from:
1:  cbz     x2, 2f
    ldtrb   w3, [x1]
    strb    w3, [x0], #1
    add     x1, x1, #1
    sub     x2, x2, #1
    b       1b
2:  mov     w0, #0
    ret

/* int uaccess_copy_to(uint64_t udst, const void* src, uint64_t n) */
uaccess_copy_to:
3:  cbz     x2, 4f
    ldrb    w3, [x1], #1
    sttrb   w3, [x0]
    add     x0, x0, #1
    sub     x2, x2, #1
    b       3b
4:  mov     w0, #0
    ret

/* int uaccess_read32(const volatile uint32_t* user, uint32_t* out) */
uaccess_read32:
    ldtr    w2, [x0]
    str     w2, [x1]
    mov     w0, #0
    ret

uaccess_fault:
    mov     w0, #-1
    ret
uaccess_end:

/* Вход в программу: void arm_enter_user(uint64_t pc, uint64_t sp, uint64_t arg, uint64_t kstack_top).
   Стек ядра потока сбрасывается к вершине (сюда не вернёмся; исключения из EL0 придут на SP_EL1 = вершина),
   регистры программы - нулевые, кроме x0 = arg. EL0t, прерывания разрешены. */
    .global arm_enter_user
arm_enter_user:
    msr     daifset, #2
    msr     sp_el0, x1
    msr     elr_el1, x0
    msr     spsr_el1, xzr
    mov     sp, x3
    mov     x0, x2
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, xzr
    mov     x5, xzr
    mov     x6, xzr
    mov     x7, xzr
    mov     x8, xzr
    mov     x9, xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret
