; nasm -f elf64: функция, указатель на неё в .data (R_X86_64_64) и вызов через указатель
default rel
global asm_twice
global asm_fn
section .text
asm_twice:
    lea eax, [rdi + rdi]
    ret
section .data
align 8
asm_fn: dq asm_twice
section .note.GNU-stack noalloc noexec nowrite progbits
