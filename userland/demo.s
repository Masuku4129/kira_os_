bits 64
; Программа Ring 3 (demo). Собирается в ELF и лежит в /bin на диске KiraFS; ядро её не содержит.
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
    mov eax, 6
    syscall
    mov r12, rax                    ; pid
    mov rbx, 0x8000200000           ; собственная страница данных
    mov [rbx], r12
    SYS_WRITE .hello, .hello_end
    mov r13d, 3
.again:
    mov eax, 4                      ; sleep(30)
    mov edi, 30
    syscall
    cmp [rbx], r12                  ; чужой процесс по тому же адресу меня не затёр?
    jne .bad
    dec r13d
    jnz .again
    SYS_WRITE .ok, .ok_end
    mov eax, 2
    xor edi, edi
    syscall
.bad:
    SYS_WRITE .bad_msg, .bad_end
    mov eax, 2
    mov edi, 1
    syscall
.hang:
    jmp .hang
.hello:   db "Hello from Ring 3: own address space, writing pid to data page 0x8000200000", 10
.hello_end:
.ok:      db "data page intact after 3 sleeps: address spaces are isolated (PASS)", 10
.ok_end:
.bad_msg: db "data page CORRUPTED: isolation broken (FAIL)", 10
.bad_end:

section .note.GNU-stack noalloc noexec nowrite progbits
