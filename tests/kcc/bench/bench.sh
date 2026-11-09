#!/bin/bash
# Замеры kcc против clang -O2 (docs/BUILDER_PLAN.md, этап 0). Запуск из корня: bash tests/kcc/bench/bench.sh [--gate] [имя...]
# Время - минимум из N запусков (KIRA_BENCH_RUNS, по умолчанию 5). Коды возврата обязаны совпасть, иначе строка помечается НЕВЕРНО.
# --gate: ненулевой код выхода, если среднее геометрическое > +10% или худший случай > +25% или размер кода > +10% (пороги плана).
cd "$(dirname "$0")/../../.." || exit 2
gate=0; [ "$1" = "--gate" ] && { gate=1; shift; }
RUNS=${KIRA_BENCH_RUNS:-5}
T=$(mktemp -d); trap 'rm -rf $T' EXIT
CC=${CC:-clang}
$CC -std=c99 -O1 -w -o $T/kcc kcc/*.c || exit 2
$CC -std=gnu99 -O1 -w -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c || exit 2
names="$*"; [ -z "$names" ] && names=$(cd tests/kcc/bench && ls *.c | sed 's/\.c$//')
best() { local m=999999 i s e d; for ((i=0;i<RUNS;i++)); do s=$(date +%s%N); "$@" >/dev/null 2>&1; e=$(date +%s%N); d=$(( (e-s)/1000 )); [ $d -lt $m ] && m=$d; done; echo $m; }
printf "%-9s %9s %9s %7s %8s %8s %7s %s\n" prog kcc_us clang_us ratio kcc_txt cl_txt sizeR check
lg=0; n=0; worst=0; szbad=0; wrong=0
for p in $names; do
    f=tests/kcc/bench/$p.c
    $CC -O2 -w -o $T/c.exe $f || { echo "$p: clang не собрал"; exit 2; }
    $T/kcc -o $T/k.elf $f || { echo "$p: kcc не собрал"; wrong=1; continue; }
    $T/c.exe; rc=$?
    got=$($T/kloader $T/k.elf 2>&1 | tail -1)
    ck=ok; [ $(( got & 255 )) = $rc ] 2>/dev/null || ck=НЕВЕРНО
    [ $ck = ok ] || wrong=1
    tk=$(best $T/kloader $T/k.elf); tc=$(best $T/c.exe)
    $T/kcc -c -o $T/k.o $f; $CC -O2 -w -c -o $T/c.o $f
    sk=$(size $T/k.o | awk 'NR==2{print $1}'); sc=$(size $T/c.o | awk 'NR==2{print $1}')
    r=$(awk -v a=$tk -v b=$tc 'BEGIN{printf "%.2f", a/(b>0?b:1)}')
    sr=$(awk -v a=$sk -v b=$sc 'BEGIN{printf "%.2f", a/(b>0?b:1)}')
    printf "%-9s %9d %9d %7s %8d %8d %7s %s\n" $p $tk $tc $r $sk $sc $sr $ck
    lg=$(awk -v l=$lg -v r=$r 'BEGIN{print l+log(r)}'); n=$((n+1))
    worst=$(awk -v w=$worst -v r=$r 'BEGIN{print (r>w)?r:w}')
    awk -v s=$sr 'BEGIN{exit !(s>1.10)}' && szbad=$((szbad+1))
done
gm=$(awk -v l=$lg -v n=$n 'BEGIN{printf "%.3f", exp(l/(n>0?n:1))}')
echo "среднее геометрическое kcc/clang: $gm, худший: $worst, программ с кодом >+10%: $szbad, неверных: $wrong"
if [ $gate = 1 ]; then
    awk -v g=$gm -v w=$worst 'BEGIN{exit !(g<=1.10 && w<=1.25)}' && [ $szbad = 0 ] && [ $wrong = 0 ] && { echo "GATE: пройден"; exit 0; }
    echo "GATE: НЕ пройден (пороги: геом. <=1.10, худший <=1.25, размер <=+10%, без неверных)"; exit 1
fi
[ $wrong = 0 ]
