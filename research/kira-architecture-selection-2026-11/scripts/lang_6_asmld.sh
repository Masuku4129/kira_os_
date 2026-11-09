#!/bin/bash
# L6: ассемблер kas против nasm (и попытка llvm-mc / GNU as) на .s ядра и userland; компоновщик klink против ld.lld и ld.bfd на ядре x86-64 и userland.
. "$(dirname "$0")/lang_common.sh"; build_kcc
D=$WORK/l6; mkdir -p $D; cd $REPO
CMPELF=$REPO/tests/klink/cmp_elf.py
[ -x $WORK/kas ] || taskset -c 0 gcc -std=c99 -O2 -w -o $WORK/kas kas/kas.c kas/kas_main.c
[ -x $WORK/klink ] || taskset -c 0 gcc -std=c99 -O2 -w -o $WORK/klink klink/klink.c klink/main.c
KAS=$WORK/kas; KL=$WORK/klink
dump() { objdump -h "$1" | awk 'NR>5{ if ($1 ~ /^[0-9]+$/) printf "%s %s %s | ", $2, $3, $NF; else print }'; objdump -s "$1" | tail -n +4; readelf -r -W "$1" | sed '/^Relocation section/ s/ at offset.*contains/ contains/'; readelf -s -W "$1" | sed 's/\.\.@[0-9]*\./..@N./'; }
# ---------- 6a. ассемблеры ----------
for f in arch/x86_64/boot arch/x86_64/interrupts arch/x86_64/context arch/x86_64/ap_tramp arch/x86_64/uaccess userland/demo userland/atk-gs userland/atk-mem userland/atk-priv userland/atk-sys tests/kas/cases64; do
  n=$(basename $f); s=$f.s
  taskset -c 0 $KAS -f elf64 -o $D/$n.kas.o $s 2>$D/err.kas; rk=$?; taskset -c 0 nasm -f elf64 -w-all $s -o $D/$n.nasm.o 2>$D/err.nasm; rn=$?
  if [ $rk = 0 ] && [ $rn = 0 ]; then
    if cmp -s $D/$n.kas.o $D/$n.nasm.o; then eq=identical; elif [ "$(dump $D/$n.kas.o | sed 's/\.\.@[0-9]*\./..@N./')" = "$(dump $D/$n.nasm.o | sed 's/\.\.@[0-9]*\./..@N./')" ]; then eq=normalized-equal; else eq=DIFFERENT; fi
    v=0; [ $eq = DIFFERENT ] && v=1
    R L6 x86_64 $ENV kas-vs-nasm $n bytes_equal 1 $eq str $v "cmp целиком / нормализованный дамп (секции, байты, перемещения, символы; локальные метки макросов ..@N); kas $(stat -c%s $D/$n.kas.o) Б, nasm $(stat -c%s $D/$n.nasm.o) Б"
    for t in kas nasm; do for ((i=1;i<=BREPS;i++)); do
      if [ $t = kas ]; then c="$KAS -f elf64 -o $D/x.o $s"; else c="nasm -f elf64 -w-all $s -o $D/x.o"; fi
      read us kb rc < <(build_time $c); R L6 x86_64 $ENV $t $n asm_us $i $us us $rc "ядро 0"; done; done
  else R L6 x86_64 $ENV kas-vs-nasm $n bytes_equal 1 "" str 1 "FAIL: kas rc=$rk ($(head -c 100 $D/err.kas | tr '\n"' '  ')) nasm rc=$rn"; fi
  # llvm-mc / GNU as: диалект NASM ожидаемо не принимается
  llvm-mc -filetype=obj -triple=x86_64-pc-linux-gnu -x86-asm-syntax=intel -o $D/x.o $s 2>$D/err.mc; rm_=$?
  R L6 x86_64 $ENV llvm-mc $n assemble_rc 1 $rm_ int $([ $rm_ = 0 ] && echo 0 || echo 1) "$([ $rm_ = 0 ] && echo принят || echo "NOT RUN-совместимость: синтаксис NASM не принят: $(grep -m1 error $D/err.mc | cut -c1-110 | tr '"' ' ')")"
  as --64 -o $D/x.o $s 2>$D/err.as; ra=$?
  R L6 x86_64 $ENV GNU-as $n assemble_rc 1 $ra int $([ $ra = 0 ] && echo 0 || echo 1) "$([ $ra = 0 ] && echo принят || echo "NOT RUN-совместимость: синтаксис NASM не принят: $(grep -m1 -i error $D/err.as | cut -c1-110 | tr '"' ' ')")"
done
# ---------- 6b. компоновка ядра ----------
SRCS=$(sed -n '/^C_SOURCES=/p;/^ARCH_SOURCES=/p' build_clang.sh | sed 's/^[A-Z_]*="//;s/"$//' | tr ' ' '\n' | grep -v '^$' | sed 's|\$ARCH|arch/x86_64|')
oname() { echo $1 | tr / _ | sed 's/\.c$/.o/'; }
K=$D/k; mkdir -p $K $D/inc
# progs.c включает manifest.h, который создаёт build_clang.sh (kfstool manifest по собранным userland-ELF). Здесь манифест создаётся из ОДНОЙ программы (plasma), а не 25 в $D/inc: образ ядра
# отличается от боевого (в манифесте один хэш вместо 25), но для сравнения компоновщиков на одних и тех же объектах это не важно (ограничение записано в отчёте).
[ -x $D/kfstool ] || taskset -c 0 clang -O1 -w -DKFS_NO_THREADS -I. -Iarch/x86_64 -o $D/kfstool tools/kfstool.c kfs.c store.c sha256.c
clang --target=x86_64-unknown-none-elf -ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -w -c userland/plasma.c -o $D/plasma.o
ld.lld -T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro -o $D/plasma.elf $D/plasma.o
$D/kfstool manifest $D/plasma.elf > $D/inc/manifest.h
CF="-ffreestanding -fno-omit-frame-pointer -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -O2 -w -mno-mmx -mno-sse -mno-sse2 -I. -Iarch/x86_64 -I$D/inc"
for a in boot interrupts context ap_tramp; do nasm -f elf64 arch/x86_64/$a.s -o $K/$a.o; done; nasm -f elf64 arch/x86_64/uaccess.s -o $K/uaccess_s.o
for s in $SRCS; do echo "taskset -c 0 clang --target=x86_64-unknown-none-elf $CF -c $s -o $K/$(oname $s)"; done | sh
KO="$K/boot.o"; for s in $SRCS; do KO="$KO $K/$(oname $s)"; done; KO="$KO $K/interrupts.o $K/context.o $K/ap_tramp.o $K/uaccess_s.o"
echo "объектов ядра: $(echo $KO | wc -w)"
KLD="-T arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000"
linkset() { # linkset МЕТКА ОБЪЕКТЫ... (ядро): три-четыре компоновщика
  local tag=$1 ld; shift
  for ld in lld0 lld1 bfd kl; do
    case $ld in lld0) c="ld.lld -O0 $KLD $* -o $D/$tag.$ld";; lld1) c="ld.lld $KLD $* -o $D/$tag.$ld";; bfd) c="ld.bfd $KLD $* -o $D/$tag.$ld";; kl) c="$KL $KLD $* -o $D/$tag.$ld";; esac
    rm -f $D/$tag.$ld; read us kb rc < <(build_time $c)
    if [ -s $D/$tag.$ld ]; then
      for ((i=1;i<=BREPS;i++)); do read us kb rc < <(build_time $c); R L6 x86_64 $ENV $ld $tag link_us $i $us us $rc "ядро x86-64, ядро 0"; R L6 x86_64 $ENV $ld $tag link_rss_kb $i $kb KB $rc ""; done
      R L6 x86_64 $ENV $ld $tag image_bytes 1 $(stat -c%s $D/$tag.$ld) B 0 "размер файла"
      R L6 x86_64 $ENV $ld $tag text_bytes 1 $(text_size $D/$tag.$ld) B 0 ".text*"
    else R L6 x86_64 $ENV $ld $tag link_us 0 "" us 1 "FAIL: $(eval "$c" 2>&1 | head -c 150 | tr '\n"' '  ')"; fi
  done
  for ld in lld1 bfd kl; do [ -s $D/$tag.$ld ] && [ -s $D/$tag.lld0 ] && {
      if cmp -s $D/$tag.$ld $D/$tag.lld0; then e=identical; else e=differ; fi
      python3 $CMPELF $D/$tag.lld0 $D/$tag.$ld --syms >$D/cmp.out 2>&1 && ce=1 || ce=0
      R L6 x86_64 $ENV $ld-vs-lld0 $tag image_equal 1 $e str $((1-ce)) "cmp файлов: $e; cmp_elf.py --syms (вход, PT_LOAD, секции, символы): $([ $ce = 1 ] && echo совпало || head -c 160 $D/cmp.out | tr '\n"' '  ')"; }; done; }
linkset kernel $KO
# ядро из объектов kcc -c
KK=$D/kk; mkdir -p $KK; kf=0
for s in $SRCS; do echo "taskset -c 0 $KCC -c -I. -Iarch/x86_64 -Ikcc/include -I$D/inc -o $KK/$(oname $s) $s"; done | sh 2>$D/kerr || kf=1
if [ $kf = 0 ]; then for a in boot interrupts context ap_tramp; do cp $K/$a.o $KK/; done; cp $K/uaccess_s.o $KK/
  KKO="$KK/boot.o"; for s in $SRCS; do KKO="$KKO $KK/$(oname $s)"; done; KKO="$KKO $KK/interrupts.o $KK/context.o $KK/ap_tramp.o $KK/uaccess_s.o"; linkset kernel-kcc $KKO
else R L6 x86_64 $ENV kcc kernel-kcc compile_all 1 "" str 1 "FAIL: kcc не собрал ядро: $(head -c 150 $D/kerr | tr '\n"' '  ')"; fi
# ---------- 6c. userland: kcc.elf (крупнейшая программа из объектов clang) ----------
UC="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -w"
U=$D/u; mkdir -p $U; UO=""
{ for src in kcc/*.c; do [ "$src" = kcc/main.c ] || echo "clang --target=x86_64-unknown-none-elf $UC -nostdlibinc -Iuserland/kccrt -c $src -o $U/kcc_$(basename $src .c).o"; done
  echo "clang --target=x86_64-unknown-none-elf $UC -nostdlibinc -Iuserland/kccrt -Iuserland -c userland/kcc.c -o $U/kcc.o"
  echo "clang --target=x86_64-unknown-none-elf $UC -nostdlibinc -Iuserland -c userland/kccrt.c -o $U/u_kccrt.o"
  echo "clang --target=x86_64-unknown-none-elf $UC -c userland/libc_min.c -o $U/u_libc_min.o"
  echo "clang --target=x86_64-unknown-none-elf $UC -nostdlibinc -I. -Iuserland/kccrt -c mind_build.c -o $U/u_mbj_build.o"
  echo "clang --target=x86_64-unknown-none-elf $UC -nostdlibinc -I. -Iuserland/kccrt -c mind_build_core.c -o $U/u_mbj_core.o"
  echo "clang --target=x86_64-unknown-none-elf $UC -I. -c elf.c -o $U/u_elf.o"
  echo "clang --target=x86_64-unknown-none-elf $UC -I. -c sha256.c -o $U/u_sha256.o"; } | while read c; do taskset -c 0 sh -c "$c"; done
UO="$U/kcc.o $U/u_mbj_build.o $U/u_mbj_core.o $U/u_elf.o $U/u_sha256.o $(ls $U/kcc_*.o | tr '\n' ' ') $U/u_kccrt.o $U/u_libc_min.o"
ULINK="-T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro"
for ld in lld0 bfd kl; do
  case $ld in lld0) c="ld.lld $ULINK -O0 -o $D/kccelf.$ld $UO";; bfd) c="ld.bfd $ULINK -o $D/kccelf.$ld $UO";; kl) c="$KL $ULINK -o $D/kccelf.$ld $UO";; esac
  rm -f $D/kccelf.$ld; build_time $c >/dev/null
  if [ -s $D/kccelf.$ld ]; then for ((i=1;i<=BREPS;i++)); do read us kb rc < <(build_time $c); R L6 x86_64 $ENV $ld kcc.elf link_us $i $us us $rc "userland kcc.elf, $(echo $UO | wc -w) объектов"; done
    R L6 x86_64 $ENV $ld kcc.elf image_bytes 1 $(stat -c%s $D/kccelf.$ld) B 0 "размер файла"
  else R L6 x86_64 $ENV $ld kcc.elf link_us 0 "" us 1 "FAIL: $(eval "$c" 2>&1 | head -c 150 | tr '\n"' '  ')"; fi
done
for ld in bfd kl; do [ -s $D/kccelf.$ld ] && { cmp -s $D/kccelf.$ld $D/kccelf.lld0 && e=identical || e=differ; python3 $CMPELF $D/kccelf.lld0 $D/kccelf.$ld --syms >$D/cmp.out 2>&1 && ce=1 || ce=0
  R L6 x86_64 $ENV $ld-vs-lld0 kcc.elf image_equal 1 $e str $((1-ce)) "cmp: $e; cmp_elf.py --syms: $([ $ce = 1 ] && echo совпало || head -c 160 $D/cmp.out | tr '\n"' '  ')"; }; done
echo L6 done
# накладные расходы замера (для интерпретации asm_us/link_us малых входов): пустая команда через ту же обёртку
for ((i=1;i<=BREPS;i++)); do read us kb rc < <(build_time true); R L6 x86_64 $ENV floor true_wrapper wrapper_us $i $us us $rc "taskset+maxrss+exec на пустой команде"; done
