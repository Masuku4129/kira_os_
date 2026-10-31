/* void context_switch(uint64_t* old_sp_slot, uint64_t new_sp): сохраняет регистры, которые вызываемая функция
   обязана беречь (x19..x29, x30), на стеке текущего потока, переходит на стек нового и возвращается туда, где он
   остановился (или в thread_trampoline для нового). */
    .text
    .global context_switch
context_switch:
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret
