; nasm -f elf64: обращения через GOT, которые нельзя релаксировать (nasm выдаёт R_X86_64_GOTPCREL): klink заводит ячейку GOT на символ
default rel
extern data_val
extern asm_twice
extern unused_b_data
global got_addr
global got_addr2
global got_twice
global got_is
global got_dead
section .text
got_addr:                          ; int *got_addr(void) { return &data_val; }
    mov rax, [rel data_val wrt ..got]
    ret
got_addr2:                         ; вторая ссылка на тот же символ: общая ячейка
    mov rax, [rel data_val wrt ..got]
    ret
got_twice:                         ; int got_twice(int x) { return asm_twice(x); } - переход по адресу из GOT
    mov rax, [rel asm_twice wrt ..got]
    jmp rax
got_is:                            ; int got_is(int *p) { return p == &data_val; } - cmp с операндом из GOT
    cmp rdi, [rel data_val wrt ..got]
    sete al
    movzx eax, al
    ret
section .text.got_dead
got_dead:                          ; недостижима: при --gc-sections ячейка для unused_b_data не создаётся
    mov rax, [rel unused_b_data wrt ..got]
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
