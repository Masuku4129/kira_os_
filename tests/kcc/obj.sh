#!/bin/bash
# Объектный режим kcc -c (ELF64 ET_REL): каждая программа tests/kcc/run собирается kcc в .o, линкуется ld.lld с общим стартом
# и исполняется на хосте; код возврата сверяется с clang -O0 (та же программа, тот же старт).
set -u
cd "$(dirname "$0")/../.."
KCC=${KCC:-/tmp/kcc_obj_test}
gcc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -o $KCC kcc/*.c || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > $T/start.c <<'EOS'
typedef unsigned long size_t;
int main(void);
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; while (n--) { if (*a != *b) return *a - *b; a++; b++; } return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
void cstart(void) { int r = main(); __asm__ volatile("syscall" :: "a"(60), "D"(r & 255)); for (;;) ; }
__asm__(".globl _start\n_start:\n and $-16, %rsp\n call cstart\n");   /* стек 16-байтовый, как у настоящего старта: clang -O0 кладёт xmm через movaps */
EOS
clang -O1 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -c $T/start.c -o $T/start.o || exit 1
total=0; fail=0; skip=0
for f in tests/kcc/run/*.c; do
    if grep -qE '#include|setjmp|printf|va_list|__builtin' $f; then skip=$((skip+1)); continue; fi
    total=$((total+1))
    if ! $KCC -c -o $T/p.o $f 2>$T/err; then echo "FAIL: $f: kcc -c: $(head -c 160 $T/err)"; fail=$((fail+1)); continue; fi
    if ! ld.lld -e _start -o $T/p $T/p.o $T/start.o 2>$T/err; then echo "FAIL: $f: линковка: $(head -c 160 $T/err)"; fail=$((fail+1)); continue; fi
    $T/p; got=$?
    clang -w -O0 -fwrapv -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -c $f -o $T/r.o 2>/dev/null && ld.lld -e _start -o $T/r $T/r.o $T/start.o && { $T/r; want=$?; } || { skip=$((skip+1)); total=$((total-1)); continue; }
    [ $got = $want ] || { echo "FAIL: $f: kcc=$got clang=$want"; fail=$((fail+1)); }
done
echo "kcc -c: программ $total, расхождений $fail, пропущено $skip"
[ $fail = 0 ] && echo "kcc obj: OK" || exit 1
