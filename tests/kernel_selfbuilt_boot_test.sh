#!/bin/bash
# Ядро, собранное ТОЛЬКО своими инструментами Киры: kcc (C), kas (.s ядра), klink (компоновка). Без clang/nasm/ld.lld для ядра; BIOS-цепочка stage1/stage2 (16 бит) пока на nasm. Грузится под SeaBIOS.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
HOSTCC=${HOSTCC:-gcc}
$HOSTCC -std=c99 -O2 -o $T/kcc kcc/*.c && $HOSTCC -std=c99 -O2 -o $T/kas kas/kas.c kas/kas_main.c && $HOSTCC -std=c99 -O2 -o $T/klink klink/klink.c klink/main.c || exit 1
S=$(sed -n '/^C_SOURCES=/p;/^ARCH_SOURCES=/p' build_clang.sh | sed 's/^[A-Z_]*="//;s/"$//' | tr ' ' '\n' | grep -v '^$' | sed 's|\$ARCH|arch/x86_64|')
for a in boot interrupts context ap_tramp uaccess; do $T/kas -f elf64 arch/x86_64/$a.s -o $T/$a.o || exit 1; done
OBJ=""
for s in $S; do o=$T/$(echo $s | tr / _).o; $T/kcc -c -I. -Iarch/x86_64 -Ikcc/include -o $o $s 2>$T/err || { echo "FAIL: $s: $(head -c 200 $T/err)"; exit 1; }; OBJ="$OBJ $o"; done
$T/klink -T arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000 $T/boot.o $OBJ $T/interrupts.o $T/context.o $T/ap_tramp.o $T/uaccess.o -o $T/kira.bin || exit 1
nasm -f bin arch/x86_64/bios/stage1.asm -o $T/s1.bin && nasm -f bin arch/x86_64/bios/stage2.asm -o $T/s2.bin && python3 tools/mkbios.py $T/s1.bin $T/s2.bin $T/kira.bin $T/k.img >/dev/null || exit 1
OUT=$(timeout 30 qemu-system-x86_64 -drive file=$T/k.img,format=raw -m 256 -display none -serial stdio -no-reboot 2>&1 | tr -d '\r' | head -c 20000)
fail=0
for pat in "Kernel core starting" "4-level paging verified" "Preemptive scheduler online" "Command line ready" "Mind online"; do
    echo "$OUT" | grep -q "$pat" && echo "ok:   $pat" || { echo "FAIL: нет '$pat'"; fail=1; }
done
echo "$OUT" | grep -qi "kernel panic" && { echo "FAIL: kernel panic"; fail=1; }
[ $fail = 0 ] && echo "kernel by kcc+kas+klink: PASS" || exit 1
