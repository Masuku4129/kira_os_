bits 64
; Программа Ring 3 (atk-gs): перезагружает регистр GS (mov gs, ax обнуляет его базу) и после этого вызывает ядро.
; Если бы ядро держало свои данные в GS без swapgs, первый же SYSCALL записал бы в адрес 8 и ядро упало бы.
; ABI: rax = номер вызова, rdi/rsi/rdx = аргументы; 1 write(ptr,len) 2 exit(code) 4 sleep(ticks) 6 getpid

%macro SYS_WRITE 2
    mov eax, 1
    lea rdi, [rel %1]
    mov esi, %2 - %1
    syscall
%endmacro

section .text.start
global _start
_start:
    SYS_WRITE .m, .m_end
    mov ax, 0x1B                    ; пользовательский сегмент данных: загрузка в GS сбрасывает базу GS
    mov gs, ax
    mov eax, 6                      ; getpid: вход в ядро после порчи GS
    syscall
    mov eax, 4                      ; sleep(2): заодно прерывание таймера придёт в ядро из этого процесса
    mov edi, 2
    syscall
    SYS_WRITE .ok, .ok_end
    mov eax, 2
    xor edi, edi
    syscall
.m:  db "attack: reloading GS in Ring 3, then calling the kernel ...", 10
.m_end:
.ok: db "kernel GS intact after the attack (PASS)", 10
.ok_end:

section .note.GNU-stack noalloc noexec nowrite progbits
