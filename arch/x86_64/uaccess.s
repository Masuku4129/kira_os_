bits 64
; Доступ ядра к памяти программ. Только эти функции читают и пишут пользовательские адреса:
; - при SMAP (процессор запрещает ядру трогать страницы USER) доступ открывается stac/clac лишь на время копирования;
; - если страница исчезла между проверкой и копированием (другой поток программы снял отображение),
;   #PF внутри [uaccess_begin, uaccess_end) не роняет ядро: обработчик переводит RIP на uaccess_fault -> -1.
section .data
global uaccess_smap
uaccess_smap: db 0

section .text
global uaccess_begin, uaccess_end, uaccess_fault
global uaccess_copy, uaccess_read32

; int uaccess_copy(void* dst, const void* src, uint64_t n)  (rdi, rsi, rdx) -> 0 / -1
uaccess_begin:
uaccess_copy:
    mov rcx, rdx
    cmp byte [rel uaccess_smap], 0
    je .go
    stac
.go:
    rep movsb
    cmp byte [rel uaccess_smap], 0
    je .done
    clac
.done:
    xor eax, eax
    ret

; int uaccess_read32(const volatile uint32_t* user, uint32_t* out)  (rdi, rsi) -> 0 / -1
uaccess_read32:
    cmp byte [rel uaccess_smap], 0
    je .go
    stac
.go:
    mov eax, [rdi]
    cmp byte [rel uaccess_smap], 0
    je .done
    clac
.done:
    mov [rsi], eax
    xor eax, eax
    ret

uaccess_fault:
    cmp byte [rel uaccess_smap], 0
    je .ret
    clac
.ret:
    mov eax, -1
    ret
uaccess_end:
