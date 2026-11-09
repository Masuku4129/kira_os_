#!/bin/bash
# L1: 11 нагрузок tests/kcc/bench/*.c x кандидаты (kcc, clang -O0/-O2, gcc -O0/-O2) в ОДНОЙ рамке: объект с bench_main + общий драйвер driver.c (clang -O2), нативный запуск.
# Дополнительно kcc-kloader: ELF Киры под хост-загрузчиком (как bench.sh). Эталон корректности: полный результат clang -O0.
. "$(dirname "$0")/lang_common.sh"; build_kcc
WL=${WL:-"sieve matmul hash sort sha256 list strscan bits calls mlp fmatmul"}
D=$WORK/l1; mkdir -p $D; clang -O2 -c $RD/scripts/lang_src/driver.c -o $D/drv.o
cc_of() { case $1 in clang-O0) echo "clang -O0";; clang-O2) echo "clang -O2";; gcc-O0) echo "gcc -O0";; gcc-O2) echo "gcc -O2";; kcc) echo "$KCC";; esac; }
for p in $WL; do
  src=$BENCH/$p.c; ref=""
  for c in clang-O0 clang-O2 gcc-O0 gcc-O2 kcc; do
    cmd="$(cc_of $c) -Dmain=bench_main -w -c"; [ $c = kcc ] && cmd="$KCC -Dmain=bench_main -c"
    o=$D/$p.$c.o
    # компиляция: BREPS повторов, ядро 0, пик памяти
    for ((i=1;i<=BREPS;i++)); do read us kb rc < <(build_time $cmd -o $o $src); R L1 x86_64 $ENV $c $p compile_us $i $us us $rc ""; R L1 x86_64 $ENV $c $p compile_rss_kb $i $kb KB $rc ""; done
    [ -s $o ] || { R L1 x86_64 $ENV $c $p run_us 0 "" us 1 "NOT RUN: объект не собран"; continue; }
    R L1 x86_64 $ENV $c $p text_bytes 1 $(text_size $o) B 0 ".text* объекта"
    gcc -no-pie $D/drv.o $o -o $D/$p.$c.exe 2>$D/err || { R L1 x86_64 $ENV $c $p run_us 0 "" us 1 "FAIL: не скомпоновано"; continue; }
    out=$(taskset -c 1 $D/$p.$c.exe); [ $c = clang-O0 ] && ref=$out
    ok=0; [ "$out" = "$ref" ] && ok=1
    R L1 x86_64 $ENV $c $p checksum 1 $out int $((1-ok)) "эталон clang-O0=$ref; match=$ok"
    [ $ok = 1 ] || { R L1 x86_64 $ENV $c $p run_us 0 "" us 1 "FAIL: контрольная сумма не совпала"; continue; }
    run_cpu $D/$p.$c.exe >/dev/null; run_cpu $D/$p.$c.exe >/dev/null
    for ((i=1;i<=REPS;i++)); do read us rc < <(run_cpu $D/$p.$c.exe); R L1 x86_64 $ENV $c $p run_us $i $us us $rc "нативный запуск, ядро 1"; done
  done
  # kcc-kloader: ELF Киры под kloader
  $KCC -o $D/$p.k.elf $src 2>/dev/null && {
    out=$(taskset -c 1 $KLOADER $D/$p.k.elf 2>&1 | tail -1); ok=0; [ "$out" = "$ref" ] && ok=1
    R L1 x86_64 $ENV kcc-kloader $p checksum 1 $out int $((1-ok)) "эталон=$ref"
    if [ $ok = 1 ]; then run_cpu $KLOADER $D/$p.k.elf >/dev/null; run_cpu $KLOADER $D/$p.k.elf >/dev/null
      for ((i=1;i<=REPS;i++)); do read us rc < <(run_cpu $KLOADER $D/$p.k.elf); R L1 x86_64 $ENV kcc-kloader $p run_us $i $us us $rc "ELF Киры под kloader, ядро 1"; done
    fi; }
  echo "done $p"
done
