bits 64
; Программа Ring 3 (atk-mem). Собирается в ELF и лежит в /bin на диске KiraFS; ядро её не содержит.
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
    SYS_WRITE .m, .m_end
    mov byte [0x100000], 0x90       ; запись в код ядра: страница без бита USER
    SYS_WRITE .survived, .survived_end
    mov eax, 2
    mov edi, 1
    syscall
.m:        db "attack: writing to kernel memory 0x100000 ...", 10
.m_end:
.survived: db "kernel memory was writable from Ring 3 (FAIL)", 10
.survived_end:

section .note.GNU-stack noalloc noexec nowrite progbits
