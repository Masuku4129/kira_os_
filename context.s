bits 64

section .text

; void context_switch(uint64_t* old_rsp_slot, uint64_t new_rsp)
; Сохраняет callee-saved регистры на стеке текущего потока, запоминает rsp,
; переключается на стек нового потока и возвращается в то место, где он
; был приостановлен (или в thread_trampoline для нового потока).
global context_switch
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
