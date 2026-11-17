; Секция таблицы перемещений для KASLR: содержимое kreloc.bin строит tools/mkkreloc.py (build_clang.sh: два прохода линковки).
; Лежит в конце файловой части образа (linker.ld: .kreloc), в самом образе после перемещения не нужна.
section .kreloc progbits alloc noexec nowrite align=8
incbin "kreloc.bin"
section .note.GNU-stack noalloc noexec nowrite progbits
