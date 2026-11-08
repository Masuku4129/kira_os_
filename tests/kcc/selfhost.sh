#!/bin/bash
# Самосборка kcc (x86-64). Цепочка:
#   stage0 - kcc, собранный хостовым компилятором (gcc/clang) из kcc/*.c;
#   stage1 - kcc/*.c + userland/kcc.c + userland/kccrt.c, собранные stage0 одним файлом (kcc/selfhost/unity.c) в ELF Киры;
#   stage2 - то же, собранное stage1 (stage1 работает как программа Киры под тестовым загрузчиком tests/kcc/kloader.c);
#   stage3 - то же, собранное stage2.
# Требования: stage1 == stage2 == stage3 побайтно (воспроизводимость и неподвижная точка). Затем stage2 компилирует корпус
# программ (tests/kcc/native, exec, run), и результат сверяется побайтно с результатом stage0.
# Это самосборка ТОЛЬКО компилятора для x86-64 под хост-загрузчиком Киры; ядро и ОС по-прежнему собирает внешний Clang/LLD/NASM.
set -u
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
note() { echo "FAIL: $*"; fail=$((fail+1)); }

$CC -std=c99 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -o $T/kcc0 kcc/*.c || exit 1
$CC -std=gnu99 -O1 -Wall -Wextra -Werror -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c || exit 1

# Песочница Киры: имена плоские (без каталогов), поэтому все исходники лежат рядом.
SB=$T/sb; INC=$T/inc
mkdir -p $SB $INC
for f in kcc/*.c kcc/*.h; do [ "$f" = kcc/main.c ] || cp $f $SB/; done
cp kcc/selfhost/* $SB/
cp userland/kccrt.c userland/kira.h $SB/
# Только пути включений: в песочнице нет каталогов (userland/kcc.c включает ../kcc/kcc.h, kccrt.c - kccrt/*.h).
sed 's|"../kcc/kcc.h"|"kcc.h"|' userland/kcc.c > $SB/kcc_main.c
sed -i 's|"kccrt/\([a-z]*\.h\)"|<\1>|' $SB/kccrt.c
cp kcc/include/* $INC/
cp userland/kccrt/* $INC/      # заголовки самого kcc (stdio.h, string.h, stdlib.h, setjmp.h) заменяют libc-lite программ

echo "== самосборка kcc: stage1 (kcc хоста собирает kcc) =="
if ! (cd $SB && $T/kcc0 -I. -I$INC -o $T/stage1.elf unity.c) 2>$T/err; then
    echo "самосборка: stage1 не собран: $(head -c 300 $T/err)"; exit 1
fi
echo "stage1: $(stat -c%s $T/stage1.elf) байт"

cp $T/stage1.elf $T/prog.elf
for st in 2 3; do
    rm -f $SB/stage$st.elf
    $T/kloader -m 512 -s $SB -i $INC -a "unity.c stage$st.elf" $T/prog.elf >$T/got$st 2>&1
    if ! grep -q "^kcc: ok: unity.c -> stage$st.elf" $T/got$st || [ ! -s $SB/stage$st.elf ]; then
        echo "самосборка: stage$st не собран: $(head -c 300 $T/got$st)"; exit 1
    fi
    echo "stage$st: $(stat -c%s $SB/stage$st.elf) байт, $(grep -o 'память компилятора [0-9]* КБ' $T/got$st)"
    cp $SB/stage$st.elf $T/prog.elf
done
cmp -s $T/stage1.elf $SB/stage2.elf || note "stage1 != stage2 (компилятор зависит от того, чем он собран)"
cmp -s $SB/stage2.elf $SB/stage3.elf || note "stage2 != stage3 (нет неподвижной точки)"
sha=$(sha256sum < $SB/stage3.elf | cut -d' ' -f1)
echo "stage3 sha256: $sha"

echo "== setjmp/longjmp (встроенные функции kcc) =="
$T/kcc0 -o $T/sj.elf tests/kcc/selfhost/setjmp_test.c 2>$T/err || note "setjmp_test не собран: $(head -c 200 $T/err)"
if [ -s $T/sj.elf ]; then
    got=$($T/kloader $T/sj.elf 2>&1 | tail -1)
    [ "$got" = 0 ] || note "setjmp_test: код выхода '$got', ожидался 0"
fi

echo "== корпус: stage2 против stage0 (побайтно) =="
UINC=$T/uinc; mkdir -p $UINC
cp kcc/include/* $UINC/; cp tests/kcc/inc/*.h $UINC/       # заголовки программ (libc-lite), не заголовки самого kcc
cp $SB/stage2.elf $T/prog2.elf
total=0
for f in tests/kcc/native/*.c tests/kcc/exec/*.c tests/kcc/run/*.c; do
    n=$(basename $f .c)
    case $f in */native/*) IP="-I kcc/include" ;; *) IP="-I tests/kcc/inc -I kcc/include" ;; esac
    $T/kcc0 $IP -o $T/ref.elf $f 2>/dev/null || continue            # программы, которые stage0 отвергает, не сравниваем
    total=$((total+1))
    cp $f $SB/prog_$n.c
    rm -f $SB/out_$n.elf
    $T/kloader -m 512 -s $SB -i $UINC -a "prog_$n.c out_$n.elf" $T/prog2.elf >$T/gotc 2>&1
    if [ ! -s $SB/out_$n.elf ]; then note "$f: stage2 не собрал: $(head -c 200 $T/gotc)"; continue; fi
    cmp -s $T/ref.elf $SB/out_$n.elf || note "$f: результат stage2 расходится с stage0"
done
[ $total -ge 20 ] || note "в сравнении слишком мало программ: $total"
echo "корпус: сравнено программ $total"

[ $fail = 0 ] && echo "kcc selfhost: OK" || { echo "kcc selfhost: ПРОВАЛОВ $fail"; exit 1; }
