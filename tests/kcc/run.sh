#!/bin/bash
# Тесты kcc (этап M1). Принятие/отклонение сверяется с clang -std=c99, значения констант - с clang.
#   ok/     обе программы принимают (clang -std=c99)           gnu/   то же, clang -std=gnu99
#   bad/    оба отвергают, у kcc ошибка вида файл:строка: error  strict/ kcc отвергает намеренно строже clang
#   unsup/  корректный C, который kcc честно отвергает с "unsupported"/"not supported" и номером строки
#   const/  значения globals `long long cN = ...;` сверяются с clang (программа печатает значения)
# Затем fuzz испорченных исходников. Сборка kcc под ASAN+UBSAN.
set -u
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
REF=${REF:-clang}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
if ! command -v "$REF" >/dev/null; then echo "kcc tests: нужен $REF для сверки"; exit 1; fi
export ASAN_OPTIONS=exitcode=99:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:exitcode=98:print_stacktrace=1
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -std=c99 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $SAN -o $T/kcc kcc/*.c || exit 1
INC=tests/kcc/inc
fail=0; total=0
note() { echo "FAIL: $*"; fail=$((fail+1)); }
# kc файл -> код возврата в $rc, сообщение в $T/err
kc() { $T/kcc -I $INC -I kcc/include -S-ir -o $T/out.ir "$1" 2>$T/err; rc=$?; }
lineinfo() { grep -Eq '^[^:]+:[0-9]+: error: .+' $T/err; }

for f in tests/kcc/ok/*.c tests/kcc/gnu/*.c; do
    total=$((total+1)); std=c99; case $f in */gnu/*) std=gnu99;; esac
    kc $f
    if [ $rc != 0 ]; then note "$f: kcc отвергает корректную программу: $(head -c 200 $T/err)"; continue; fi
    if ! $REF -std=$std -fsyntax-only -w -I $INC $f 2>$T/rerr; then note "$f: сам тест неверен, clang отвергает: $(head -1 $T/rerr)"; fi
done

for f in tests/kcc/bad/*.c; do
    total=$((total+1))
    kc $f
    if [ $rc != 1 ]; then note "$f: kcc принял или упал (rc=$rc): $(head -c 200 $T/err)"; continue; fi
    lineinfo || note "$f: ошибка без номера строки: $(head -c 200 $T/err)"
    if $REF -std=c99 -fsyntax-only -w -I $INC $f 2>/dev/null; then note "$f: сам тест неверен, clang принимает"; fi
done

for f in tests/kcc/strict/*.c; do
    total=$((total+1))
    kc $f
    if [ $rc != 1 ]; then note "$f: kcc должен отвергнуть (rc=$rc)"; continue; fi
    lineinfo || note "$f: ошибка без номера строки"
    $REF -std=c99 -fsyntax-only -w -I $INC $f 2>/dev/null || note "$f: сам тест неверен, clang отвергает"
done

for f in tests/kcc/unsup/*.c; do
    total=$((total+1))
    kc $f
    if [ $rc != 1 ]; then note "$f: kcc должен отвергнуть (rc=$rc): $(head -c 200 $T/err)"; continue; fi
    lineinfo || note "$f: ошибка без номера строки: $(head -c 200 $T/err)"
    grep -Eqi 'unsupported|not supported' $T/err || note "$f: сообщение не говорит, что конструкция не поддержана: $(head -c 200 $T/err)"
    $REF -std=gnu99 -fsyntax-only -w -I $INC $f 2>/dev/null || note "$f: сам тест неверен, clang отвергает"
done

for f in tests/kcc/const/*.c; do
    total=$((total+1))
    kc $f
    if [ $rc != 0 ]; then note "$f: kcc отвергает: $(head -c 200 $T/err)"; continue; fi
    names=$(grep -Eo '^long long c[0-9]+' $f | awk '{print $3}')
    { echo "#include <stdio.h>"; echo "#include \"$PWD/$f\""; echo "int main(void) {";
      for n in $names; do echo "printf(\"$n %lld\\n\", $n);"; done; echo "return 0; }"; } >$T/ref.c
    if ! $REF -std=gnu99 -w -o $T/ref $T/ref.c 2>$T/rerr; then note "$f: clang не собрал: $(head -1 $T/rerr)"; continue; fi
    $T/ref | sort >$T/want
    for n in $names; do
        v=$(grep -E "^data $n size=8 .* val=" $T/out.ir | sed 's/.* val=//')
        echo "$n $v"
    done | sort >$T/got
    if ! diff $T/want $T/got >$T/d; then note "$f: значения расходятся с clang: $(head -4 $T/d | tr '\n' ' ')"; fi
done

$CC -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $SAN -o $T/irrun tests/kcc/irrun.c || exit 1
for f in tests/kcc/run/*.c; do
    total=$((total+1))
    kc $f
    if [ $rc != 0 ]; then note "$f: kcc отвергает: $(head -c 200 $T/err)"; continue; fi
    if ! $T/irrun $T/out.ir >$T/got 2>$T/err; then note "$f: интерпретатор IR упал: $(head -c 200 $T/err)"; continue; fi
    { cat $f; echo '#undef main'; echo '#include <stdio.h>'; echo 'int main(void) { printf("%d\n", kmain()); return 0; }'; } >$T/ref.c
    if ! $REF -std=gnu99 -w -O0 -Dmain=kmain -o $T/ref $T/ref.c 2>$T/rerr; then note "$f: clang не собрал: $(head -1 $T/rerr)"; continue; fi
    $T/ref >$T/want
    cmp -s $T/want $T/got || note "$f: результат IR $(cat $T/got) != clang $(cat $T/want)"
done

# враждебные входы: глубокая вложенность и длинные цепочки не должны ронять стек
rep() { yes "$1" | head -n "$2" | tr -d '\n'; }
stress() { # имя, ожидаемый rc (0 или 1)
    total=$((total+1))
    kc $T/stress.c
    if [ $rc != $2 ]; then note "stress $1: rc=$rc (ждали $2): $(head -c 160 $T/err)"; fi
}
{ printf 'int x = '; rep '(' 50000; printf 1; rep ')' 50000; echo ';'; } >$T/stress.c; stress parens 1
{ printf 'void f(void) { '; rep '{' 50000; rep '}' 50000; echo ' }'; } >$T/stress.c; stress braces 1
{ printf 'int f(int a) { return a'; rep '+a' 100000; echo '; }'; } >$T/stress.c; stress long_sum 1
{ printf 'int f(int a) { return '; rep '-' 100000; echo 'a; }'; } >$T/stress.c; stress unary_chain 1
{ printf '#define A(x) x\nint y = '; rep 'A(' 20000; printf 1; rep ')' 20000; echo ';'; } >$T/stress.c; stress macro_nest 1
{ printf 'int a'; rep '[1]' 100000; echo ';'; } >$T/stress.c; stress dims 1
{ printf 'int '; rep '(' 50000; printf x; rep ')' 50000; echo ';'; } >$T/stress.c; stress declarator 1
{ printf 'struct S { '; rep 'struct { ' 30000; echo '}; };'; } >$T/stress.c; stress structs 1
{ printf 'int f(int a) { if (a) ; '; rep 'else if (a) ; ' 5000; echo '}'; } >$T/stress.c; stress else_if 1
{ printf '#define E(x) x x\n#define E2(x) E(E(x))\n#define E4(x) E2(E2(x))\n#define E8(x) E4(E4(x))\n#define E16(x) E8(E8(x))\n#define E32(x) E16(E16(x))\n#define E64(x) E32(E32(x))\nint x = E64(E64(E64(E64(1))));\n'; } >$T/stress.c; stress macro_blowup 1
{ for i in $(seq 1 20000); do echo "int v$i = $i; int f$i(int a) { return a + v$i; }"; done; } >$T/stress.c; stress many_decls 0

echo "kcc: файлов-тестов $total, провалов $fail"
[ $total -ge 150 ] || { echo "kcc: корпус меньше 150 программ"; fail=$((fail+1)); }

echo "== kcc fuzz =="
$CC -std=c99 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $SAN -o $T/fuzz tests/kcc/fuzz.c $(ls kcc/*.c | grep -v main.c) || exit 1
if ! $T/fuzz ${KCC_FUZZ_ITERS:-3000} tests/kcc/ok/*.c tests/kcc/gnu/*.c tests/kcc/const/*.c tests/kcc/bad/*.c tests/kcc/run/*.c tests/kcc/unsup/*.c; then note "fuzz упал"; fi

[ $fail = 0 ] && echo "kcc tests: OK" || { echo "kcc tests: ПРОВАЛОВ $fail"; exit 1; }
