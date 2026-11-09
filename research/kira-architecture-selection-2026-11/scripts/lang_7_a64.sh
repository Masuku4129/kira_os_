#!/bin/bash
# L7: AArch64. Размер кода: clang --target=aarch64-linux-gnu -ffreestanding -c (O0/O2) против kcc -target aarch64 (kcc -c для a64 не реализован -> берётся PT_LOAD R-X готового ELF Киры).
# Выполнение: kcc a64-ELF под хост-эмулятором a64emu (tests/kcc/a64emu.c, подмножество A64 из kcc; это НЕ QEMU и НЕ время железа). Время эмуляции не записывается как скорость.
. "$(dirname "$0")/lang_common.sh"; build_kcc
D=$WORK/l7; mkdir -p $D
# копия эмулятора с поднятым лимитом команд (в оригинале жёстко 4e9; нагрузкам bench нужно больше). Оригинал в репозитории не меняется.
sed -e 's/limit = 4000000000ULL/limit = 40000000000ULL/' -e "s|\"../../elf.h\"|\"$REPO/elf.h\"|" $REPO/tests/kcc/a64emu.c > $D/a64emu_lim.c
[ -x $WORK/a64emu ] || taskset -c 0 clang -std=gnu99 -O2 -w -DKIRA_HOST_TEST -DKIRA_HOST_ARM64 -I$REPO/tests/kcc -I$REPO -o $WORK/a64emu $D/a64emu_lim.c $REPO/elf.c -lm || echo "a64emu FAIL"
WL=${WL:-"sieve matmul hash sort sha256 list strscan bits calls mlp fmatmul"}
seg_text() { # PT_LOAD с флагом X: размер файла сегмента из readelf -lW
  local h; h=$(readelf -lW "$1" 2>/dev/null | awk '$1=="LOAD" && $0 ~ / R E / {print $5; exit}'); printf '%d' "$h"; }
clang -O2 -c $RD/scripts/lang_src/driver.c -o $D/drv.o
$KCC -c -target aarch64 -o $D/t.o $BENCH/sieve.c 2>$D/err; R L7 aarch64 $ENV kcc-a64 sieve obj_c_supported 1 $([ -s $D/t.o ] && echo 1 || echo 0) bool $([ -s $D/t.o ] && echo 0 || echo 1) "kcc -c -target aarch64: $(head -c 120 $D/err | tr '\n"' '  ')"
for p in $WL; do
  src=$BENCH/$p.c
  clang -O0 -w -Dmain=bench_main -c $src -o $D/ref.o && gcc -no-pie $D/drv.o $D/ref.o -o $D/ref.exe && ref=$(taskset -c 1 $D/ref.exe)
  for o in O0 O2; do
    if clang --target=aarch64-linux-gnu -ffreestanding -$o -w -c $src -o $D/c.$o.o 2>$D/err; then R L7 aarch64 $ENV clang-a64-$o $p text_bytes 1 $(text_size $D/c.$o.o) B 0 ".text* объекта (clang --target=aarch64-linux-gnu -ffreestanding -c)"
    else R L7 aarch64 $ENV clang-a64-$o $p text_bytes 1 "" B 1 "FAIL: $(head -c 120 $D/err | tr '\n"' '  ')"; fi
  done
  R L7 x86_64 $ENV kcc-x86 $p text_bytes 1 $($KCC -c -o $D/kx.o $src && text_size $D/kx.o) B 0 ".text kcc -c x86-64 (для сравнения)"
  if $KCC -target aarch64 -o $D/k.a64.elf $src 2>$D/err; then
    R L7 aarch64 $ENV kcc-a64 $p text_bytes 1 $(seg_text $D/k.a64.elf) B 0 "PT_LOAD R-X filesz ELF Киры a64 (включает стартовый код; ET_REL для a64 не поддержан)"
    out=$(cd $D && taskset -c 0 timeout ${A64_TIMEOUT:-100} $WORK/a64emu $D/k.a64.elf 2>&1 | tail -1); rc=${PIPESTATUS[0]}
    if [ -z "$out" ]; then R L7 aarch64 $ENV kcc-a64 $p checksum 1 "" int NOTRUN "NOT RUN: a64emu не уложился в ${A64_TIMEOUT:-100} с (интерпретатор) или нет вывода"
    else m=0; [ "$out" = "$ref" ] && m=1; R L7 aarch64 $ENV kcc-a64 $p checksum 1 "$out" int $((1-m)) "a64emu (не QEMU); эталон x86 clang-O0=$ref; match=$m"; fi
  else R L7 aarch64 $ENV kcc-a64 $p text_bytes 1 "" B 1 "FAIL: kcc не собрал: $(head -c 120 $D/err | tr '\n"' '  ')"; fi
  echo "done $p"
done
R L7 aarch64 $ENV clang-a64 all run 0 "" "" NOTRUN "NOT RUN: нет qemu-user; qemu-system-aarch64 есть, но нужен загрузочный образ aarch64 с запуском программы (в репозитории - только ядро Киры, ci/arm64_vm.py); время в TCG не скорость"
if rustup target list --installed 2>/dev/null | grep -q aarch64; then R L7 aarch64 $ENV Rust-a64 all run 0 "" "" 1 "цель установлена, но замер не выполнен (скрипт не реализован)"; else R L7 aarch64 $ENV Rust-a64 all run 0 "" "" NOTRUN "NOT RUN: rustup target list --installed = $(rustup target list --installed | tr '\n' ' '); цели aarch64 нет, нет и сети для установки/ -Zbuild-std"; fi
echo L7 done
# база: размер PT_LOAD R-X пустой программы (стартовый код + рантайм kcc a64), чтобы вычесть его из сравнения
echo 'int main(void){return 0;}' > $D/empty.c
$KCC -target aarch64 -o $D/empty.elf $D/empty.c && R L7 aarch64 $ENV kcc-a64 empty_main text_bytes 1 $(seg_text $D/empty.elf) B 0 "PT_LOAD R-X пустого main (база без нагрузки)"
$KCC -o $D/empty.x.elf $D/empty.c && R L7 x86_64 $ENV kcc-x86 empty_main text_bytes 1 $(seg_text $D/empty.x.elf) B 0 "PT_LOAD R-X пустого main x86-64 (для справки)"
