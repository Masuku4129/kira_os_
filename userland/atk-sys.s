bits 64
; Программа Ring 3 (atk-sys). Собирается в ELF и лежит в /bin на диске KiraFS; ядро её не содержит.
; ABI: rax = номер вызова, rdi/rsi/rdx = аргументы; 1 write(ptr,len) 2 exit(code) 3 ticks 4 sleep(ticks) 5 yield 6 getpid

%macro SYS_WRITE 2
    mov eax, 1
    lea rdi, [rel %1]
    mov esi, %2 - %1
    syscall
%endmacro

section .text.start
global _start
_start:
    mov eax, 1                      ; write(указатель на память ЯДРА, 64)
    mov edi, 0x100000
    mov esi, 64
    syscall
    cmp rax, -1
    jne .vuln
    SYS_WRITE .pass, .pass_end
    mov eax, 2
    xor edi, edi
    syscall
.vuln:
    SYS_WRITE .fail, .fail_end
    mov eax, 2
    mov edi, 1
    syscall
.pass: db "kernel pointer passed to write() was rejected (PASS)", 10
.pass_end:
.fail: db "write() accepted a kernel pointer (FAIL)", 10
.fail_end:

section .note.GNU-stack noalloc noexec nowrite progbits
