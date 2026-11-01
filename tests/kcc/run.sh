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

echo "kcc: файлов-тестов $total, провалов $fail"
[ $total -ge 150 ] || { echo "kcc: корпус меньше 150 программ"; fail=$((fail+1)); }

echo "== kcc fuzz =="
$CC -std=c99 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $SAN -o $T/fuzz tests/kcc/fuzz.c $(ls kcc/*.c | grep -v main.c) || exit 1
if ! $T/fuzz ${KCC_FUZZ_ITERS:-3000} tests/kcc/ok/*.c tests/kcc/gnu/*.c tests/kcc/const/*.c tests/kcc/bad/*.c; then note "fuzz упал"; fi

[ $fail = 0 ] && echo "kcc tests: OK" || { echo "kcc tests: ПРОВАЛОВ $fail"; exit 1; }
