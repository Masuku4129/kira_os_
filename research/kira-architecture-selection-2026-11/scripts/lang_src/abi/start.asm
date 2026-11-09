; точка входа Киры для объектов вне KIRA_ENTRY: main() -> выход процесса Киры (системный вызов 2)
global _start
extern main
section .text.start
_start:
    and rsp, -16
    call main
    mov edi, eax
    mov eax, 2
    syscall
.hang:
    jmp .hang
section .note.GNU-stack noalloc noexec nowrite progbits
