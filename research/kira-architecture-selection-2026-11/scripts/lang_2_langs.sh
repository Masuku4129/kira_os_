#!/bin/bash
# L2: языки. 4 нагрузки (sha256 sort matmul strscan) на C (clang/gcc -O2), Rust (-C opt-level=3 no_std panic=abort; checked и unchecked),
# C++ (clang++/g++ -O2 -fno-exceptions -fno-rtti -ffreestanding), ручной asm (strscan), Go (справка, PARTIAL). Одна рамка frame4.c (clang -O2).
. "$(dirname "$0")/lang_common.sh"
S=$RD/scripts/lang_src; D=$WORK/l2; mkdir -p $D; cd $D
WL="sha256 sort matmul strscan"
declare -A REF
# L2_ONLY="КАНДИДАТ:нагрузка ..." - перезапуск отдельных ячеек (остальные не трогаются); L2_ONLY пусто - всё
want() { [ -z "$L2_ONLY" ] || [[ " $L2_ONLY " == *" $1:$2 "* ]]; }
printf 'void rust_eh_personality(void){}\n' > $D/rs_eh.c; clang -O2 -c $D/rs_eh.c -o $D/rs_eh.o   # заглушка личности исключений: prebuilt core собран с unwind, наш crate - panic=abort
frame() { clang -O2 -w -DFN=$1 -c $S/frame4.c -o $D/frame_$1.o; }
meas() { # meas CAND WL OBJ FN [LINKARG] : линковка (LINKARG вместо OBJ, если задан - для Rust staticlib с core), корректность, замер
  local c=$1 p=$2 o=$3 fn=$4 la=${5:-$3}; frame $fn
  if [ -z "${REF[$p]}" ] && [ $c != C-clang-O2 ]; then clang -O2 -w -Dmain=c_$p -c $BENCH/$p.c -o $D/ref_$p.o && frame c_$p && gcc -no-pie $D/frame_c_$p.o $D/ref_$p.o -o $D/ref_$p.exe && REF[$p]=$(taskset -c 1 $D/ref_$p.exe); fi
  gcc -no-pie $D/frame_$fn.o $la -o $D/$p.$c.exe 2>$D/err || { R L2 x86_64 $ENV $c $p run_us 0 "" us 1 "FAIL: линковка: $(head -c 120 $D/err | tr '\n"' '  ')"; return; }
  local out; out=$(taskset -c 1 $D/$p.$c.exe); [ $c = C-clang-O2 ] && REF[$p]=$out
  local ok=0; [ "$out" = "${REF[$p]}" ] && ok=1
  R L2 x86_64 $ENV $c $p checksum 1 $out int $((1-ok)) "эталон C-clang-O2=${REF[$p]}"
  R L2 x86_64 $ENV $c $p text_bytes 1 $(text_size $o) B 0 ".text* объекта"
  R L2 x86_64 $ENV $c $p exe_text_bytes 1 $(text_size $D/$p.$c.exe) B 0 ".text* готового исполняемого (с crt и нужными частями core/libc)"
  [ $ok = 1 ] || { R L2 x86_64 $ENV $c $p run_us 0 "" us 1 "FAIL: контрольная сумма"; return; }
  run_cpu $D/$p.$c.exe >/dev/null; run_cpu $D/$p.$c.exe >/dev/null
  for ((i=1;i<=REPS;i++)); do read us rc < <(run_cpu $D/$p.$c.exe); R L2 x86_64 $ENV $c $p run_us $i $us us $rc "ядро 1"; done
}
ctime() { # ctime CAND WL OUT CMD...
  local c=$1 p=$2 o=$3; shift 3
  for ((i=1;i<=BREPS;i++)); do read us kb rc < <(build_time "$@" -o $o); R L2 x86_64 $ENV $c $p compile_us $i $us us $rc "ядро 0"; R L2 x86_64 $ENV $c $p compile_rss_kb $i $kb KB $rc ""; done; }
for p in $WL; do
  # C-эталоны: main -> c_<p>
  want C-clang-O2 $p && { ctime C-clang-O2 $p $D/$p.cc.o clang -O2 -w -Dmain=c_$p -c $BENCH/$p.c;  meas C-clang-O2 $p $D/$p.cc.o c_$p ; }
  want C-gcc-O2 $p && { ctime C-gcc-O2   $p $D/$p.cg.o gcc   -O2 -w -Dmain=c_$p -c $BENCH/$p.c;  meas C-gcc-O2 $p $D/$p.cg.o c_$p ; }
  # Rust: объект (--emit=obj) для времени компиляции и размера собственного кода; исполняемый файл - из staticlib (включает нужные части core, напр. panic_bounds_check)
  want Rust-O3 $p && { ctime Rust-O3 $p $D/$p.rs.o rustc --crate-type=lib --emit=obj -C opt-level=3 -C panic=abort --cfg w_$p $S/bench4.rs; rustc --crate-type=staticlib -C opt-level=3 -C panic=abort --cfg w_$p $S/bench4.rs -o $D/$p.rs.a 2>/dev/null; meas Rust-O3 $p $D/$p.rs.o rs_$p "$D/$p.rs.a $D/rs_eh.o" ; }
  want Rust-O3-unchecked $p && { ctime Rust-O3-unchecked $p $D/$p.rsu.o rustc --crate-type=lib --emit=obj -C opt-level=3 -C panic=abort --cfg w_$p --cfg unchecked $S/bench4.rs; rustc --crate-type=staticlib -C opt-level=3 -C panic=abort --cfg w_$p --cfg unchecked $S/bench4.rs -o $D/$p.rsu.a 2>/dev/null; meas Rust-O3-unchecked $p $D/$p.rsu.o rs_$p "$D/$p.rsu.a $D/rs_eh.o" ; }
  # C++
  CXXF="-O2 -fno-exceptions -fno-rtti -ffreestanding -w -std=c++17 -DW_$p -c"
  want Cpp-clang-O2 $p && { ctime Cpp-clang-O2 $p $D/$p.xc.o clang++ $CXXF $S/bench4.cpp; meas Cpp-clang-O2 $p $D/$p.xc.o cpp_$p ; }
  ctime Cpp-gcc-O2   $p $D/$p.xg.o g++ $CXXF $S/bench4.cpp;     meas Cpp-gcc-O2 $p $D/$p.xg.o cpp_$p
done
[ -n "$L2_ONLY" ] && { echo "L2 (частичный перезапуск) done"; exit 0; }
# ручной asm: strscan (ядра cnt/fnv на asm, остальное C -O2)
clang -O2 -w -c $S/strscan_asm_main.c -o $D/sa_main.o
ctime Asm-GNUas strscan $D/sa_k.o as $S/strscan_kernels.s
ld -r $D/sa_main.o $D/sa_k.o -o $D/strscan.asm.o
meas Asm-GNUas strscan $D/strscan.asm.o asm_strscan
R L2 x86_64 $ENV Asm-GNUas strscan asm_lines 1 $(grep -vc '^\s*$' $S/strscan_kernels.s) lines 0 "строк в strscan_kernels.s (без пустых) против C: $(grep -vc '^\s*$' $BENCH/strscan.c) строк"
# Go (справка): отдельная программа, рантайм Go, нативная рамка другая
export GOCACHE=$WORK/gocache GOFLAGS=-mod=mod GO111MODULE=off CGO_ENABLED=0
mkdir -p $D/go; cp $S/bench4.go $D/go/main.go
taskset -c 0 go build -o $D/go/warm $D/go/main.go 2>$D/err || echo "go build FAIL: $(head -c 300 $D/err)"
if [ -x $D/go/warm ]; then
  for p in $WL; do
    out=$(taskset -c 1 $D/go/warm $p); ok=0; [ "$out" = "${REF[$p]}" ] && ok=1
    R L2 x86_64 $ENV Go-ref $p checksum 1 $out int $((1-ok)) "PARTIAL: рантайм не freestanding; эталон ${REF[$p]}"
    [ $ok = 1 ] || continue
    run_cpu $D/go/warm $p >/dev/null; run_cpu $D/go/warm $p >/dev/null
    for ((i=1;i<=REPS;i++)); do read us rc < <(run_cpu $D/go/warm $p); R L2 x86_64 $ENV Go-ref $p run_us $i $us us $rc "PARTIAL: целая программа с рантаймом Go, старт включён; ядро 1"; done
  done
  R L2 x86_64 $ENV Go-ref all text_bytes 1 $(text_size $D/go/warm) B 0 "PARTIAL: .text всего исполняемого файла с рантаймом (не сравнимо с объектами)"
  for ((i=1;i<=BREPS;i++)); do echo "// rep $i" >> $D/go/main.go; read us kb rc < <(build_time go build -o $D/go/bin $D/go/main.go); R L2 x86_64 $ENV Go-ref all compile_us $i $us us $rc "PARTIAL: go build, кэш std тёплый, пересобирается пакет main + линковка"; R L2 x86_64 $ENV Go-ref all compile_rss_kb $i $kb KB $rc "maxrss go build (wait4 учитывает потомков)"; done
fi
for c in Asm-GNUas; do :; done
for c in Zig Ada-SPARK; do R L2 x86_64 $ENV $c all run_us 0 "" us NOTRUN "NOT RUN: компилятора нет на машине"; done
R L2 x86_64 $ENV KIRA-lang all run_us 0 "" us NOTRUN "NOT RUN: реализации языка KIRA нет"
echo L2 done
