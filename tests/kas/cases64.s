; Дифференциальные случаи для bits 64: сверяются с nasm побайтно (tests/kas_test.sh)
bits 64
%define K 5
%define ADDK(x) ((x) + K)
%macro PUSH2 2
    push %1
    push %2
%endmacro
section .text
global fn, fn2
extern ext_f, ext_d
fn:
    mov rax, rbx
    mov eax, ebx
    mov ax, bx
    mov al, bl
    mov r8, r15
    mov r8d, r9d
    mov r10w, r11w
    mov r12b, r13b
    mov sil, dil
    mov ah, bl
    mov rax, 1
    mov rax, 0xFFFFFFFF
    mov rax, -1
    mov rax, 0x123456789A
    mov eax, 5
    mov r9d, 7
    mov ax, 9
    mov cl, 3
    mov dil, 4
    mov rax, ext_d
    mov eax, ext_d
    mov rax, fn
    mov [rax], rbx
    mov [rax + 8], ebx
    mov [rax + rcx*4], dx
    mov [rax + rcx*4 + 0x1234], dl
    mov [rbp], rax
    mov [rbp + 0], rax
    mov [r13], rax
    mov [r12], rax
    mov [rsp], rax
    mov [rsp + 8], rax
    mov [r12 + r13*8 - 16], rax
    mov rax, [rbx + rsi*2]
    mov eax, [ecx]
    mov eax, [ecx + edx*2 + 4]
    mov rax, [rel fn]
    mov rax, [rel ext_d]
    mov [rel ext_d + 4], eax
    mov eax, [ext_d]
    mov eax, [ext_d + rcx*8]
    mov eax, [gs:0x10]
    mov rax, [fs:rbx]
    mov [gs:8], rsp
    mov dword [rax], 5
    mov qword [rax], -5
    mov byte [rax], 0x7f
    mov word [rax], 0x1234
    mov dword [rel ext_d], 1
    mov qword [rax + 8], 0x7fffffff
    add rax, rbx
    add eax, 5
    add eax, 0x1000
    add rax, 0x1000
    add al, 3
    add bl, 3
    add byte [rax], 3
    add dword [rax], 300
    add word [rax], 300
    add ax, 300
    add rsp, -8
    sub rsp, 0x100
    and rsp, -16
    or eax, 0x80000000
    or rax, rdx
    xor r8d, r8d
    xor eax, eax
    cmp byte [rel ext_d], 0
    cmp dword [rax + 4], 0x12345
    cmp rax, [rbx]
    cmp [rbx], rax
    adc eax, ecx
    sbb rax, 1
    test eax, eax
    test al, 1
    test rax, 0x1000
    test bl, 4
    test dword [rax], 0xff
    test [rax], ecx
    test ecx, [rax]
    lea rax, [rax + rax*2]
    lea r9, [rsp + 16]
    lea eax, [rax + 4]
    lea rax, [rel fn2]
    push rax
    push r9
    pop rbx
    pop r15
    push 0
    push 127
    push 128
    push -128
    push -129
    push 0x7fffffff
    push qword 5
    push qword [rax]
    push qword [rel ext_d]
    pop qword [rax]
    pop qword [gs:16]
    push ext_d
    PUSH2 1, rax
    inc eax
    inc rax
    inc byte [rax]
    inc qword [rax]
    dec ecx
    dec r9
    dec word [rax]
    not rax
    neg ecx
    mul rbx
    div ecx
    idiv qword [rax]
    shl eax, 1
    shl eax, 21
    shr r11, 47
    sar rax, cl
    rol bl, 3
    shl qword [rax], 2
    movzx eax, bl
    movzx eax, byte [rax]
    movzx rax, word [rax]
    movsx eax, bx
    movsx rax, byte [rbx]
    movsxd rax, ecx
    movsxd rax, dword [rax]
    cmovz eax, ecx
    cmovne rax, [rbx]
    setz al
    setne byte [rax]
    setg r9b
    call rax
    call [rax]
    call qword [rel ext_d]
    jmp rax
    jmp [rax + 8]
    call ext_f
    call fn2
    call ext_f wrt ..plt
    jmp ext_f
    jmp near fn2
    int 0x80
    in al, dx
    in eax, 0x60
    out dx, al
    out 0x20, ax
    lock inc qword [rel ext_d]
    lock xor [rax], ecx
    rep movsb
    rep movsq
    rep stosd
    repne scasb
    mov cr0, rax
    mov rax, cr2
    mov cr8, r9
    mov r10, cr3
    mov ds, ax
    mov es, cx
    mov ax, ds
    mov eax, cs
    mov [rax], ds
    mov fs, [rax]
    lgdt [rax]
    lidt [rel ext_d]
    sgdt [rbx + 4]
    o64 sysret
    sysret
    syscall
    swapgs
    iretq
    retfq
    pushfq
    popfq
    cli
    sti
    hlt
    nop
    ret
    cpuid
    rdtsc
    pause
    ud2
    lfence
    cdq
    cqo
    cdqe
    clac
    stac
    a32 mov [ecx], eax
fn2:
    jmp short .a
.a:
    jz .b
    jnz .b
    jc .b
    jae .b
    jl .b
.b:
    times 130 nop
    jmp .c
    jz .c
    jmp .far_back
    jne .far_back
.c:
    jmp .a
    jz .a
    times 140 db 0x90
.far_back:
    jmp .d
    times 127 nop
.d:
    jmp .e
    times 128 nop
.e:
    jmp .f
    times 126 nop
.f:
    jmp short .g
.g:
    mov eax, ADDK(3)
    mov eax, K + 2 * 3 - (7 % 4) + (1 << 4) + (0xff >> 2) + (6 & 3) + (6 | 1) + (6 ^ 3)
    mov eax, ~5 & 0xff
    mov eax, -(2 + 3)
    mov eax, 'A'
    mov eax, 'AB'
    mov eax, 0x10 + 010 + 10h + 1010b + 0b11 + 0o17
    align 16
    db 1, 2, "abc", 'd', 0
    dw 0x1234, -1
    dd 0x12345678, fn2 - fn, -5
    dq 0x1122334455667788, fn, ext_d + 8
    dd fn
    dd ext_d
    align 8
    resb 0
    times 3 dw 5
    times 2 db "xy"
loc2:
    jmp loc2
    nop
.l1: nop
    jmp .l1
exported equ 12345
SZ equ $ - fn
    mov eax, exported
    mov eax, SZ
%if K == 5
    mov eax, 111
%else
    mov eax, 222
%endif
%if K != 5
    mov eax, 333
%else
    mov eax, 444
%endif
section .data
dat: dd 1, 2
dat2: dq dat
section .rodata
ro: db "hello", 10, 0
section .bss
b1: resb 10
b2: resd 3
b3: resq 2
alignb 16
b4: resw 7
section .text
    mov rax, [rel dat]
    mov rax, [rel b2]
    mov eax, [b2]
    mov eax, dat
    mov eax, [dat + 4]
    mov rax, ro
    dq dat, ro, b2
    jmp b1
    call ro
