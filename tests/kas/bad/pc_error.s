;; ожидается: 5: dual-mode
section .text
a: nop
%if ($ - a) != 2
%error "dual-mode"
%endif
