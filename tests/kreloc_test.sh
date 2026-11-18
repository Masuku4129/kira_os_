#!/bin/bash
# Проверка перемещения образа ядра "в лоб": образец (абсолютные адреса всех видов, включая абсолютный символ) линкуется
# дважды - по адресу A и по A+сдвиг. Таблицу строит tools/mkkreloc.py из образа A, kreloc_apply применяет её к содержимому A;
# результат должен совпасть с образом, который скомпоновщик собрал по A+сдвиг, до последнего байта. Это независимый
# эталон: он не опирается ни на разбор перемещений в tools/mkkreloc.py, ни на приёмы kaslr.c.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf $T' EXIT
LD=${LD:-ld.lld}; command -v $LD >/dev/null || LD=ld
CF="-O2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=small -c"
gcc $CF tests/kreloc/prog.c -o $T/prog.o
gcc -c tests/kreloc/s.s -o $T/s.o
A=0x100000; SLIDE=0x2400000; B=$((A + SLIDE))
python3 -c "import struct,sys;open(sys.argv[1],'wb').write(struct.pack('<5I',0x434C524B,1,0,0,0))" $T/kreloc.bin
mkobj() { printf 'section .kreloc progbits alloc noexec nowrite align=8\nincbin "%s"\n' $T/kreloc.bin > $T/kreloc.s; nasm -f elf64 -o $T/kreloc.o $T/kreloc.s; }
link() { sed "s/BASE/$1/" tests/kreloc/link.ld > $T/l.ld; $LD -T $T/l.ld -nostdlib -z max-page-size=0x1000 --defsym=ABSVAL=0x4321 "${@:3}" $T/prog.o $T/s.o -o $2; }
mkobj; link $A $T/a1.elf --emit-relocs $T/kreloc.o
python3 tools/mkkreloc.py build $T/a1.elf $T/kreloc.bin
mkobj; link $A $T/a.elf --emit-relocs $T/kreloc.o
python3 tools/mkkreloc.py check $T/a.elf $T/kreloc.bin
link $B $T/b.elf $T/kreloc.o
python3 tests/kreloc/image.py $T/a.elf $T/a.img
python3 tests/kreloc/image.py $T/b.elf $T/b.img
gcc -O1 -Wall -Werror -I. -o $T/apply tests/kreloc/apply.c kaslr.c bootinfo.c
$T/apply $T/a.img $T/kreloc.bin $SLIDE $T/moved.img
if cmp $T/moved.img $T/b.img; then echo "kreloc: образ, перемещённый по таблице, совпал с образом, собранным по новому адресу ($(stat -c %s $T/a.img) байт)"; else echo "kreloc: РАСХОЖДЕНИЕ"; exit 1; fi
# контроль теста: без таблицы образы различаются, а абсолютный символ не сдвинут
if cmp -s $T/a.img $T/b.img; then echo "kreloc: образы A и B одинаковы - тест ничего не проверяет"; exit 1; fi
NW=$(python3 -c "import struct;h=struct.unpack('<5I',open('$T/kreloc.bin','rb').read(20));print(h[2],h[3])")
echo "kreloc: перемещений (64-бит, 32-бит): $NW"
