#!/bin/bash
# L8: вклад проходов kcc. Один переключатель за раз (-fno-*), 11 нагрузок, BREPS=9 повторов на ядре 1, корректность по эталону clang -O0 (как L1).
# Чтобы убрать дрейф скорости машины (виртуальная среда, соседние нагрузки), замеры ЧЕРЕДУЮТСЯ: повтор базы, повтор варианта, повтор базы, ...
# База для каждого варианта пишется отдельно как кандидат "kcc-base@<флаг>" (парная база). Нативная рамка (как L1): kcc -c -Dmain=bench_main + driver.c.
. "$(dirname "$0")/lang_common.sh"; build_kcc
WL=${WL:-"sieve matmul hash sort sha256 list strscan bits calls mlp fmatmul"}
FLAGS=${FLAGS:-"-fno-sr -fno-lvn -fno-regs -fno-dce -fno-align -fno-nnshift -fno-mrot -fno-disp -fno-extld -fno-rec-inline -fno-once-inline -fno-popcount -fno-reassoc -fno-cfgfirst -fno-pooltrim -O0"}
D=$WORK/l8; mkdir -p $D; clang -O2 -c $RD/scripts/lang_src/driver.c -o $D/drv.o
for p in $WL; do
  clang -O0 -w -Dmain=bench_main -c $BENCH/$p.c -o $D/ref.o && gcc -no-pie $D/drv.o $D/ref.o -o $D/ref.exe && ref=$(taskset -c 1 $D/ref.exe)
  $KCC -c -Dmain=bench_main -o $D/base.o $BENCH/$p.c && gcc -no-pie $D/drv.o $D/base.o -o $D/base.exe || { echo "база не собрана $p"; continue; }
  out=$(taskset -c 1 $D/base.exe); [ "$out" = "$ref" ] || { echo "база: неверная сумма $p"; continue; }
  for fl in $FLAGS; do
    c="kcc$fl"
    if ! $KCC -c $fl -Dmain=bench_main -o $D/x.o $BENCH/$p.c 2>$D/err; then R L8 x86_64 $ENV "$c" $p run_us 0 "" us 1 "FAIL: kcc не собрал: $(head -c 100 $D/err | tr '\n"' '  ')"; continue; fi
    R L8 x86_64 $ENV "$c" $p text_bytes 1 $(text_size $D/x.o) B 0 ".text объекта"
    R L8 x86_64 $ENV "kcc-base@$fl" $p text_bytes 1 $(text_size $D/base.o) B 0 ".text базы"
    gcc -no-pie $D/drv.o $D/x.o -o $D/x.exe || { R L8 x86_64 $ENV "$c" $p run_us 0 "" us 1 "FAIL: линковка"; continue; }
    out=$(taskset -c 1 timeout 60 $D/x.exe); m=0; [ "$out" = "$ref" ] && m=1
    R L8 x86_64 $ENV "$c" $p checksum 1 "$out" int $((1-m)) "эталон clang-O0=$ref; match=$m"
    [ $m = 1 ] || { R L8 x86_64 $ENV "$c" $p run_us 0 "" us 1 "FAIL: контрольная сумма"; continue; }
    run_cpu $D/base.exe >/dev/null; run_cpu $D/x.exe >/dev/null
    for ((i=1;i<=BREPS;i++)); do
      read us rc < <(run_cpu $D/base.exe); R L8 x86_64 $ENV "kcc-base@$fl" $p run_us $i $us us $rc "парная база, ядро 1"
      read us rc < <(run_cpu $D/x.exe);    R L8 x86_64 $ENV "$c" $p run_us $i $us us $rc "ядро 1"
    done
  done
  echo "done $p"
done
