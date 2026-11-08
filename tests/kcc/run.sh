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

# M2: ELF Киры (x86-64) -> хост-загрузчик (elf_parse ядра + seccomp-таблица k_*); вывод и код возврата как у clang -O0
$CC -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c || exit 1
refbuild() { # исходник -> $T/want (stdout эталона + код возврата main)
    { cat $1; echo '#undef main'; echo '#include <stdio.h>'; echo 'int main(void) { printf("%d\n", kmain()); return 0; }'; } >$T/ref.c
    $REF -std=gnu99 -w -O0 -fwrapv -I $INC -Dmain=kmain -o $T/ref $T/ref.c 2>$T/rerr || return 1
    $T/ref >$T/want
}
for f in tests/kcc/run/*.c tests/kcc/exec/*.c; do
    total=$((total+1))
    if ! $T/kcc -I $INC -I kcc/include -o $T/p.elf $f 2>$T/err; then note "$f: ELF не собран: $(head -c 200 $T/err)"; continue; fi
    if ! refbuild $f; then note "$f: clang не собрал: $(head -1 $T/rerr)"; continue; fi
    $T/kloader $T/p.elf >$T/got 2>&1
    cmp -s $T/want $T/got || note "$f: вывод ELF расходится с clang: $(head -c 120 $T/got | tr '\n' ' ') != $(head -c 120 $T/want | tr '\n' ' ')"
done
# Каждый проход оптимизации отключается флагом -fno-xxx; результат с любым отключённым проходом обязан совпадать с clang
# (так ошибка отдельного прохода отделяется от ошибки остального кода).
PASSFLAGS="-fno-regs -fno-dce -fno-align -fno-nnshift -fno-mrot -fno-disp -fno-extld -fno-rec-inline -fno-once-inline -fno-popcount"
for f in tests/kcc/run/*.c; do
    refbuild $f || continue
    for fl in $PASSFLAGS; do
        total=$((total+1))
        if ! $T/kcc -I $INC -I kcc/include $fl -o $T/p.elf $f 2>$T/err; then note "$f ($fl): ELF не собран: $(head -c 200 $T/err)"; continue; fi
        $T/kloader $T/p.elf >$T/got 2>&1
        cmp -s $T/want $T/got || note "$f ($fl): вывод расходится с clang: $(head -c 120 $T/got | tr '\n' ' ') != $(head -c 120 $T/want | tr '\n' ' ')"
    done
done
for f in tests/kcc/elfbad/*.c; do
    total=$((total+1))
    $T/kcc -I $INC -I kcc/include -o $T/p.elf $f 2>$T/err; rc=$?
    if [ $rc != 1 ]; then note "$f: kcc должен отвергнуть при генерации кода (rc=$rc)"; continue; fi
    lineinfo || note "$f: ошибка без номера строки: $(head -c 200 $T/err)"
    want=$(sed -n 's|^// expect: ||p' $f | head -1)
    grep -qF -- "$want" $T/err || note "$f: ожидалось '$want': $(head -c 200 $T/err)"
done

# M3: libc-lite для программ Киры (kcc/include: kira.h, stdio.h, stdlib.h, string.h...) и сам kcc как программа Ring 3.
# Программа компилируется хост-kcc, выполняется загрузчиком; ожидаемый вывод записан в tests/kcc/native/*.out.
total=$((total+1))
if ! $T/kcc -I kcc/include -o $T/n.elf tests/kcc/native/hello.c 2>$T/err; then note "native/hello.c: не собран: $(head -c 200 $T/err)"
else
    $T/kloader $T/n.elf >$T/got 2>&1
    cmp -s tests/kcc/native/hello.out $T/got || note "native/hello.c: вывод расходится с ожидаемым: $(head -c 160 $T/got | tr '\n' ' ')"
fi
total=$((total+1))
$T/kcc -I kcc/include -o $T/n.elf tests/kcc/native/net.c 2>$T/err || note "native/net.c: не собран: $(head -c 200 $T/err)"
total=$((total+1))
$T/kcc -I kcc/include -o $T/n.elf tests/kcc/native/bad.c 2>$T/err; rc=$?
if [ $rc != 1 ] || ! grep -q '^tests/kcc/native/bad.c:9: error: ' $T/err; then note "native/bad.c: ждали ошибку на строке 9: $(head -c 200 $T/err)"; fi
# kcc, собранный как программа Киры (userland/kcc.c + kcc/*.c + userland/kccrt*), работает под тем же загрузчиком:
# K_FS - каталоги хоста (песочница и заголовки), K_ARGS - строка запуска, арена растёт через K_HEAP.
if command -v ld.lld >/dev/null && clang --target=x86_64-unknown-none-elf -ffreestanding -c -x c /dev/null -o $T/probe.o 2>/dev/null; then
    UC="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -nostdlibinc"
    KC="clang --target=x86_64-unknown-none-elf $UC"
    objs=""; built=1
    for src in kcc/*.c; do
        [ $src = kcc/main.c ] && continue
        $KC -Iuserland/kccrt -c $src -o $T/ku_$(basename $src .c).o 2>$T/err || { built=0; break; }
        objs="$objs $T/ku_$(basename $src .c).o"
    done
    $KC -Iuserland/kccrt -Iuserland -c userland/kcc.c -o $T/ku_main.o 2>>$T/err && $KC -Iuserland -c userland/kccrt.c -o $T/ku_rt.o 2>>$T/err &&
        $KC -c userland/libc_min.c -o $T/ku_lm.o 2>>$T/err && ld.lld -T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro -o $T/kcc_kira.elf $T/ku_main.o $objs $T/ku_rt.o $T/ku_lm.o 2>>$T/err || built=0
    total=$((total+1))
    if [ $built != 1 ]; then note "kcc как программа Киры не собрана: $(head -c 300 $T/err)"
    else
        mkdir -p $T/sb; cp tests/kcc/native/*.c $T/sb/
        $T/kloader -s $T/sb -i kcc/include -a "hello.c hello" $T/kcc_kira.elf >$T/got 2>&1
        grep -q '^kcc: ok: hello.c -> hello' $T/got || note "kcc-на-Кире: hello.c не собран: $(head -c 200 $T/got)"
        total=$((total+1))
        $T/kloader $T/sb/hello >$T/got 2>&1
        cmp -s tests/kcc/native/hello.out $T/got || note "kcc-на-Кире: собранная программа печатает другое: $(head -c 160 $T/got | tr '\n' ' ')"
        total=$((total+1))
        $T/kloader -s $T/sb -i kcc/include -a "bad.c bad" $T/kcc_kira.elf >$T/got 2>&1
        grep -q '^bad.c:9: error: ' $T/got || note "kcc-на-Кире: ошибка без номера строки: $(head -c 200 $T/got)"
        total=$((total+1))
        $T/kloader -s $T/sb -i kcc/include -a "nofile.c out" $T/kcc_kira.elf >$T/got 2>&1
        grep -q 'nofile.c' $T/got && grep -q 'not found\|cannot\|No such' $T/got || note "kcc-на-Кире: нет файла должно давать сообщение: $(head -c 200 $T/got)"
        total=$((total+1))
        $T/kloader -s $T/sb -i kcc/include -a "one" $T/kcc_kira.elf >$T/got 2>&1
        grep -q 'использование: cc' $T/got || note "kcc-на-Кире: без результата должна быть справка: $(head -c 200 $T/got)"
    fi
else
    echo "kcc: сборка kcc как программы Киры пропущена: нет ld.lld или clang для x86_64-unknown-none-elf"
fi

# M4: AArch64. Кодировщик сверяется с эталоном llvm-mc; программы выполняет хост-эмулятор EL0 (tests/kcc/a64emu.c: только подмножество A64,
# которое выдаёт kcc; ELF проверяется elf_parse() ядра ARM64): вывод и код возврата как у clang -O0 на хосте.
total=$((total+1))
if ! $CC -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -o $T/a64enc tests/kcc/a64_enc.c kcc/asm_a64.c; then note "a64_enc не собран"
else
    $T/a64enc >$T/a64.txt
    diff tests/kcc/a64_ref.txt $T/a64.txt >$T/d || note "кодировки A64 расходятся с эталоном llvm-mc: $(head -4 $T/d | tr '\n' ' ')"
fi
$CC -std=gnu99 -O2 -g -Wall -Wextra -Werror -DKIRA_HOST_TEST -DKIRA_HOST_ARM64 -o $T/a64emu tests/kcc/a64emu.c elf.c || exit 1
for f in tests/kcc/run/*.c tests/kcc/exec/*.c; do
    total=$((total+1))
    if ! $T/kcc -target aarch64 -I $INC -I kcc/include -o $T/p64.elf $f 2>$T/err; then note "$f: ELF AArch64 не собран: $(head -c 200 $T/err)"; continue; fi
    if ! refbuild $f; then note "$f: clang не собрал: $(head -1 $T/rerr)"; continue; fi
    $T/a64emu $T/p64.elf >$T/got 2>&1
    cmp -s $T/want $T/got || note "$f: вывод AArch64 расходится с clang: $(head -c 120 $T/got | tr '\n' ' ') != $(head -c 120 $T/want | tr '\n' ' ')"
done
for f in tests/kcc/elfbad_a64/*.c; do
    total=$((total+1))
    $T/kcc -target aarch64 -I $INC -I kcc/include -o $T/p64.elf $f 2>$T/err; rc=$?
    if [ $rc != 1 ]; then note "$f: kcc должен отвергнуть при генерации кода AArch64 (rc=$rc)"; continue; fi
    lineinfo || note "$f: ошибка без номера строки: $(head -c 200 $T/err)"
    want=$(sed -n 's|^// expect: ||p' $f | head -1)
    grep -qF -- "$want" $T/err || note "$f: ожидалось '$want': $(head -c 200 $T/err)"
done
total=$((total+1))
$T/kcc -target aarch64 -I kcc/include -o $T/n64.elf tests/kcc/native/hello.c 2>$T/err && $T/a64emu $T/n64.elf >$T/got 2>&1
cmp -s tests/kcc/native/hello.out $T/got || note "native/hello.c (AArch64): вывод расходится с ожидаемым: $(head -c 160 $T/got | tr '\n' ' ')"
# расширения языка (атомарные операции, struct по значению, va_list, case-диапазоны, ?:, printf): оба бэкенда печатают то же, что clang на хосте
total=$((total+2))
$T/kcc -I kcc/include -o $T/n.elf tests/kcc/native/lang.c 2>$T/err && $T/kloader $T/n.elf >$T/got 2>&1
cmp -s tests/kcc/native/lang.out $T/got || note "native/lang.c (x86-64): вывод расходится с ожидаемым: $(head -c 200 $T/got | tr '\n' ' ') $(head -c 100 $T/err)"
$T/kcc -target aarch64 -I kcc/include -o $T/n64.elf tests/kcc/native/lang.c 2>$T/err && $T/a64emu $T/n64.elf >$T/got 2>&1
cmp -s tests/kcc/native/lang.out $T/got || note "native/lang.c (AArch64): вывод расходится с ожидаемым: $(head -c 200 $T/got | tr '\n' ' ') $(head -c 100 $T/err)"

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
