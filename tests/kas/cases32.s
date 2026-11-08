bits 32
section .text
global f32
extern e32
f32:
    mov eax, ebx
    mov ax, bx
    mov al, bl
    mov eax, 5
    mov ax, 5
    mov esp, f32
    mov eax, [f32]
    mov [f32], eax
    mov [f32], ebx
    mov al, [f32]
    mov [f32], al
    mov ax, [f32]
    mov eax, [ebx]
    mov eax, [ebx + ecx*4 + 8]
    mov eax, [e32 + ecx*8]
    mov [e32], ecx
    mov eax, [gs:4]
    mov eax, [gs:ebx]
    mov dword [f32], 7
    inc eax
    inc cx
    inc byte [eax]
    dec edx
    dec dword [eax]
    push eax
    push 5
    push 300
    push f32
    push dword [eax]
    pop ecx
    pop dword [f32]
    mov eax, cr0
    mov cr3, eax
    mov cr4, ecx
    or eax, 0x80000001
    or ax, 1
    add esp, 4
    cmp ecx, 512
    lea eax, [eax + eax*4]
    lgdt [f32]
    lidt [eax]
    jmp 0x08:f32
    jmp dword 0x10:e32
    call e32
    call f32
    jmp e32
    jmp f32
    rdmsr
    wrmsr
    shl eax, 3
    shr cl, 1
    movzx eax, byte [ebx]
    xor eax, eax
    test eax, 1
    int 0x10
    in al, 0x64
    out dx, eax
    rep movsd
    cld
    ret
    mov ax, ds
    mov ds, ax
    mov eax, cs
    mov ax, cs
bits 16
    cli
    mov ax, cs
    mov ds, ax
    lgdt [0x600 + 2]
    mov eax, cr0
    or eax, 1
    or al, 1
    mov cr0, eax
    jmp dword 0x08:f32
    mov ax, 5
    mov eax, 5
    mov [0x500], ax
    mov ax, [0x500]
    mov eax, [0x500]
    mov bx, [0x500]
    push ax
    push eax
    push 5
    inc ax
    inc eax
    jmp short x16
x16:
    jmp x16
    jz x16
    call x16
    times 200 nop
    jmp x16
    jz x16
gd16: dw 3
    dd f32
v16: dw 0
