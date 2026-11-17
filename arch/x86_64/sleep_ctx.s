; Сон S3: сохранение и возврат точки исполнения процессора (как setjmp/longjmp, но возврат идёт ПОСЛЕ полного восстановления
; состояния процессора: таблицы, регистры управления, MSR - это делает sleep.c на временном стеке).
; Раскладка блока (qword): 0 rbx, 8 rbp, 16 r12, 24 r13, 32 r14, 40 r15, 48 rsp, 56 rip. Остальное состояние хранит sleep.c.
bits 64
section .text
global cpu_ctx_save
global cpu_ctx_jump

; uint64_t cpu_ctx_save(cpu_ctx_asm_t* c): запоминает callee-saved регистры, указатель стека и адрес возврата; возвращает 0.
; Второй раз «возвращается» из cpu_ctx_jump с значением 1.
cpu_ctx_save:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    mov rax, [rsp]
    mov [rdi + 56], rax
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    xor eax, eax
    ret

; void cpu_ctx_jump(const cpu_ctx_asm_t* c): продолжает исполнение так, будто cpu_ctx_save вернула 1.
cpu_ctx_jump:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov rcx, [rdi + 56]
    mov eax, 1
    jmp rcx

section .note.GNU-stack noalloc noexec nowrite progbits
