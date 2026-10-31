/* memcpy/memmove/memset/memcmp для ARM64: компилятор вызывает их сам (копирование структур), поэтому они на
   ассемблере - цикл на C он превратил бы в вызов самого себя. Без невыровненных обращений (до включения MMU
   память - устройство, там они запрещены): по 8 байт, только когда оба адреса выровнены. */
    .text
    .global memcpy
memcpy:
    mov     x3, x0
    orr     x4, x0, x1
    tst     x4, #7
    b.ne    2f
1:  cmp     x2, #8
    b.lo    2f
    ldr     x4, [x1], #8
    str     x4, [x3], #8
    sub     x2, x2, #8
    b       1b
2:  cbz     x2, 3f
    ldrb    w4, [x1], #1
    strb    w4, [x3], #1
    sub     x2, x2, #1
    b       2b
3:  ret

    .global memmove
memmove:
    cmp     x0, x1
    b.ls    memcpy
    add     x3, x0, x2
    add     x1, x1, x2
4:  cbz     x2, 5f
    ldrb    w4, [x1, #-1]!
    strb    w4, [x3, #-1]!
    sub     x2, x2, #1
    b       4b
5:  ret

    .global memset
memset:
    mov     x3, x0
    and     w1, w1, #0xff
6:  cbz     x2, 7f
    strb    w1, [x3], #1
    sub     x2, x2, #1
    b       6b
7:  ret

    .global memcmp
memcmp:
8:  cbz     x2, 9f
    ldrb    w3, [x0], #1
    ldrb    w4, [x1], #1
    subs    w5, w3, w4
    b.ne    10f
    sub     x2, x2, #1
    b       8b
9:  mov     w0, #0
    ret
10: mov     w0, w5
    ret
