# ручной x86-64 (GNU as, синтаксис Intel/noprefix), SysV AMD64. n кратно 16 в нашей нагрузке (1<<20); для общности есть хвост.
        .intel_syntax noprefix
        .text
        .globl asm_cnt
        .type asm_cnt,@function
# int asm_cnt(const char *s /*rdi*/, int n /*esi*/, char c /*dl*/)
asm_cnt:
        movsxd  rsi, esi
        movzx   eax, dl
        movd    xmm1, eax
        punpcklbw xmm1, xmm1
        punpcklwd xmm1, xmm1
        pshufd  xmm1, xmm1, 0         # c во всех 16 байтах
        pxor    xmm0, xmm0            # накопитель байтовых счётчиков
        pxor    xmm3, xmm3            # нули для psadbw
        xor     eax, eax              # итог
        mov     rcx, rsi
        and     rcx, -16              # число байт в блоках по 16
        jz      .Ltail
.Lblk:                                # внешний цикл: до 255 блоков по 16 байт, затем сброс байтовых счётчиков
        mov     r8, rcx
        cmp     r8, 255*16
        jbe     1f
        mov     r8d, 255*16
1:      sub     rcx, r8
        pxor    xmm0, xmm0
.Lin:   movdqu  xmm2, [rdi]
        pcmpeqb xmm2, xmm1            # 0xFF там, где совпало
        psubb   xmm0, xmm2            # -(-1) = +1
        add     rdi, 16
        sub     r8, 16
        jnz     .Lin
        psadbw  xmm0, xmm3            # суммы байтов в двух 64-битных половинах
        movd    r9d, xmm0
        add     eax, r9d
        pshufd  xmm0, xmm0, 0xEE
        movd    r9d, xmm0
        add     eax, r9d
        test    rcx, rcx
        jnz     .Lblk
.Ltail: and     rsi, 15
        jz      .Ldone
.Lt1:   cmp     byte ptr [rdi], dl
        jne     2f
        inc     eax
2:      inc     rdi
        dec     rsi
        jnz     .Lt1
.Ldone: ret
        .size asm_cnt, .-asm_cnt

        .globl asm_fnv
        .type asm_fnv,@function
# unsigned asm_fnv(const char *s /*rdi*/, int n /*esi*/)
asm_fnv:
        mov     eax, 2166136261
        test    esi, esi
        jle     .Lfd
        mov     esi, esi
        lea     rsi, [rdi+rsi]
.Lf:    movzx   ecx, byte ptr [rdi]
        xor     eax, ecx
        imul    eax, eax, 16777619
        inc     rdi
        cmp     rdi, rsi
        jne     .Lf
.Lfd:   ret
        .size asm_fnv, .-asm_fnv
        .section .note.GNU-stack,"",@progbits
