#!/bin/bash
# L4: ABI/интероп. A) цепочка main(clang)->k(kcc)->g(gcc|clang)->rs(rustc)->cpp(clang++)->k_cb(kcc) в одном исполняемом: нативно (ld.lld, ld.bfd) и в формате Киры (ld.lld, ld.bfd, klink; запуск под kloader).
# B) матрица SysV AMD64: реализация f_* одним компилятором, вызов другим (kcc, clang, gcc): 9 пар, 17 тестов.
. "$(dirname "$0")/lang_common.sh"; build_kcc
S=$RD/scripts/lang_src/abi; D=$WORK/l4; mkdir -p $D; cd $D
CMPELF=$REPO/tests/klink/cmp_elf.py; ULD=$REPO/userland/user.ld
ULINK="-T $ULD -z max-page-size=4096 -z noseparate-code -z norelro"
[ -x $WORK/klink ] || taskset -c 0 gcc -std=c99 -O2 -w -o $WORK/klink $REPO/klink/klink.c $REPO/klink/main.c
KL=$WORK/klink
INC="-I$S"
# ---------- A. цепочка (вариант PV: {long;double}, вариант PI: {long;long}) ----------
for VAR in PV PI; do
CD=''; RSD=''; [ $VAR = PI ] && { CD='-DPINT'; RSD='--cfg pint'; }
FS="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -msse2 -mfpmath=sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -w"
$KCC -c $INC $CD -o k.o $S/abi_k.c || echo "kcc FAIL"
clang $FS -c $INC $CD $S/abi_main.c -o main_f.o;  gcc $FS -c $INC $CD $S/abi_g.c -o g_gcc_f.o;  clang $FS -c $INC $CD $S/abi_g.c -o g_clang_f.o
clang++ $FS -fno-exceptions -fno-rtti $CD -c $S/abi_cpp.cpp -o cpp_f.o
rustc --crate-type=lib --emit=obj -C opt-level=2 -C panic=abort -C relocation-model=pic -C force-unwind-tables=no $RSD $S/abi_rs.rs -o rs_f.o 2>rs.err || { echo "rustc FAIL"; cat rs.err; }
nasm -f elf64 $S/start.asm -o start.o
# эталон: всё на C (clang -O2) в нативной рамке
clang -O2 -w -Dmain=bench_main $INC $CD $S/abi_k.c $S/abi_g.c $S/abi_refimpl.c $S/abi_main.c -o ref_parts 2>/dev/null
clang -O2 -w -c $INC $CD $S/abi_k.c -o r_k.o; clang -O2 -w -c $INC $CD $S/abi_g.c -o r_g.o; clang -O2 -w -c $INC $CD $S/abi_refimpl.c -o r_ref.o; clang -O2 -w -Dmain=bench_main -c $INC $CD $S/abi_main.c -o r_main.o
clang -O2 -w -c $RD/scripts/lang_src/driver.c -o drv.o
gcc -no-pie drv.o r_main.o r_k.o r_g.o r_ref.o -o ref.exe && REF=$(./ref.exe); echo "эталон (всё на C, clang -O2): $REF"
R L4 x86_64 $ENV ref-all-C-$VAR chain result 1 $REF int 0 "эталон: kcc-часть тоже собрана clang"
# A1. нативно: смешанные объекты. main и драйвер - clang; g - gcc или clang
clang -O2 -w -Dmain=bench_main -c $INC $CD $S/abi_main.c -o main_n.o
gcc -O2 -w -c $INC $CD $S/abi_g.c -o g_gcc_n.o; clang -O2 -w -c $INC $CD $S/abi_g.c -o g_clang_n.o
clang++ -O2 -fno-exceptions -fno-rtti $CD -c $S/abi_cpp.cpp -o cpp_n.o
rustc --crate-type=lib --emit=obj -C opt-level=2 -C panic=abort -C relocation-model=pic $RSD $S/abi_rs.rs -o rs_n.o
for gk in gcc clang; do
  for ld in lld bfd; do
    out=$D/chain_n_${gk}_$ld
    if clang -fuse-ld=$ld -no-pie drv.o main_n.o k.o g_${gk}_n.o rs_n.o cpp_n.o -o $out 2>err; then
      got=$(taskset -c 0 $out); m=0; [ "$got" = "$REF" ] && m=1
      R L4 x86_64 $ENV "$VAR:native:ld.$ld:g=$gk" chain result 1 "$got" int $((1-m)) "объекты kcc+$gk+rustc+clang++; эталон $REF; совпало=$m"
    else R L4 x86_64 $ENV "$VAR:native:ld.$ld:g=$gk" chain result 1 "" int 1 "FAIL линковки: $(head -c 200 err | tr '\n"' '  ')"; fi
  done
done
# A2. формат Киры: ld.lld / ld.bfd / klink. Rust-объект в двух моделях перемещений: pic (по умолчанию rustc) и static (без GOT-вызовов)
rustc --crate-type=lib --emit=obj -C opt-level=2 -C panic=abort -C relocation-model=static -C force-unwind-tables=no $RSD $S/abi_rs.rs -o rs_f_static.o 2>rs.err || { echo "rustc static FAIL"; cat rs.err; }
cp rs_f.o rs_f_pic.o
for gk in gcc clang; do for RSM in pic static; do
  O="start.o main_f.o k.o g_${gk}_f.o rs_f_$RSM.o cpp_f.o"; T="$VAR:kira:g=$gk:rust=$RSM"
  rm -f ck.lld ck.bfd ck.kl
  ld.lld $ULINK -O0 -o ck.lld $O 2>err.lld; ld.bfd $ULINK -o ck.bfd $O 2>err.bfd; $KL $ULINK -o ck.kl $O 2>err.kl
  for ld in lld bfd kl; do
    f=ck.$ld
    if [ -s $f ]; then got=$(taskset -c 0 timeout 20 $KLOADER $f 2>&1 | tail -1); m=0; [ "$got" = "$REF" ] && m=1
      R L4 x86_64 $ENV "$T:$ld" chain result 1 "$got" int $((1-m)) "kloader; эталон $REF; совпало=$m; размер файла $(stat -c%s $f)"
    else R L4 x86_64 $ENV "$T:$ld" chain result 1 "" int 1 "FAIL: образ не собран: $(head -c 220 err.$ld | tr '\n"' '  ')"; fi
  done
  if [ -s ck.kl ] && [ -s ck.lld ]; then
    if python3 $CMPELF ck.lld ck.kl --syms >cmp.out 2>&1; then eq=1; else eq=0; fi
    R L4 x86_64 $ENV "$T:klink-vs-lld" chain image_equal 1 $eq bool $((1-eq)) "cmp_elf.py --syms (вход, PT_LOAD, секции, символы): $(head -c 200 cmp.out | tr '\n"' '  ')"
    if cmp -s ck.lld ck.kl; then R L4 x86_64 $ENV "$T:klink-vs-lld" chain bytes_equal 1 1 bool 0 "cmp побайтно"; else R L4 x86_64 $ENV "$T:klink-vs-lld" chain bytes_equal 1 0 bool 1 "cmp: файлы различаются"; fi
  fi
done; done
# время линковки A2 (g=gcc, rust=static), ядро 0
O="start.o main_f.o k.o g_gcc_f.o rs_f_static.o cpp_f.o"
for ld in lld bfd kl; do for ((i=1;i<=BREPS;i++)); do
  case $ld in lld) c="ld.lld $ULINK -O0 -o t.$ld $O";; bfd) c="ld.bfd $ULINK -o t.$ld $O";; kl) c="$KL $ULINK -o t.$ld $O";; esac
  read us kb rc < <(build_time $c); R L4 x86_64 $ENV "$VAR:kira-link:$ld" chain link_us $i $us us $rc "6 объектов"; R L4 x86_64 $ENV "$VAR:kira-link:$ld" chain link_rss_kb $i $kb KB $rc ""; done; done
done

# ---------- B. матрица SysV ----------
declare -A CC=( [kcc]="$KCC -c" [clang]="clang -O2 -w -c" [gcc]="gcc -O2 -w -c" )
for X in kcc clang gcc; do ${CC[$X]} $INC -o impl_$X.o $S/abi_impl.c 2>err || echo "impl $X FAIL $(head -c 200 err)"; ${CC[$X]} $INC -o call_$X.o $S/abi_call.c 2>err || echo "call $X FAIL $(head -c 200 err)"; done
gcc -no-pie call_clang.o impl_clang.o -o m_ref && ./m_ref > ref.txt; echo "тестов в эталоне: $(wc -l < ref.txt)"
for X in kcc clang gcc; do for Y in kcc clang gcc; do   # X - реализация, Y - вызывающий
  c="impl-${X}_call-${Y}"
  if [ -s impl_$X.o ] && [ -s call_$Y.o ] && gcc -no-pie call_$Y.o impl_$X.o -o m_${X}_$Y 2>err; then
    taskset -c 0 ./m_${X}_$Y > out_${X}_$Y.txt 2>&1; rc=$?
    while IFS='=' read name want; do got=$(grep "^$name=" out_${X}_$Y.txt | cut -d= -f2); m=0; [ "$got" = "$want" ] && m=1
      R L4 x86_64 $ENV "$c" $name abi_match 1 $m bool $rc "получено=$got эталон(clang/clang)=$want"; done < ref.txt
    cp out_${X}_$Y.txt $LOGD/lang_4_abi_out_${X}_$Y.txt
  else R L4 x86_64 $ENV "$c" all abi_match 1 "" bool 1 "FAIL сборки/линковки: $(head -c 200 err | tr '\n"' '  ')"; fi
done; done
echo L4 done
