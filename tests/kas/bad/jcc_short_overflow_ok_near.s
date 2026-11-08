;; ожидается: OK
bits 64
section .text
start:
    jz far_away
    times 300 nop
far_away:
