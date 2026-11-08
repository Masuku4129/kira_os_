;; ожидается: 6: ожидается параметров 2
%macro M 2
    mov %1, %2
%endmacro
section .text
    M eax
