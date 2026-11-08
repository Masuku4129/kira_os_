#!/bin/bash
# Ассемблер kas (kas/): сборка, побайтная сверка с nasm на пяти файлах ядра и на наборах форм,
# негативные тесты (ошибка с номером строки), фаззинг повреждёнными исходниками, самопроверка на заглушки.
# Запуск из корня репозитория или откуда угодно: tests/kas_test.sh. Без nasm сверка пропускается ЯВНО (SKIP), остальное идёт.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
HOSTCC=${HOSTCC:-gcc}
fail=0; skip=0
ok()  { echo "ok:   $*"; }
bad() { echo "FAIL: $*"; fail=1; }

$HOSTCC -std=c99 -O2 -Wall -Wextra -Werror -o $T/kas kas/kas.c kas/kas_main.c || { echo "FAIL: сборка kas"; exit 1; }
ok "kas собран (-std=c99 -Wall -Wextra -Werror)"

# нормализованный дамп объекта: секции (имя, размер, выравнивание, флаги), байты секций, перемещения, символы.
# Нормализуются только смещения служебных таблиц и номера локальных меток макросов (..@N.имя).
dump() {
    objdump -h "$1" | awk 'NR>5{ if ($1 ~ /^[0-9]+$/) printf "%s %s %s | ", $2, $3, $NF; else print }'
    objdump -s "$1" | tail -n +4
    readelf -r -W "$1" | sed '/^Relocation section/ s/ at offset.*contains/ contains/'
    readelf -s -W "$1" | sed 's/\.\.@[0-9]*\./..@N./'
}
compare() { # имя файл.s
    local name=$1 src=$2
    if ! $T/kas -f elf64 -o $T/k.o $src 2>$T/err; then bad "$name: kas не собрал: $(head -c 300 $T/err)"; return; fi
    if ! nasm -f elf64 -w-all $src -o $T/n.o 2>$T/nerr; then bad "$name: nasm не собрал: $(head -c 200 $T/nerr)"; return; fi
    if cmp -s $T/k.o $T/n.o; then ok "$name: объектный файл совпадает с nasm целиком (cmp)"; return; fi
    if [ "$(dump $T/k.o)" = "$(dump $T/n.o)" ]; then
        ok "$name: секции, байты, перемещения и символы совпадают; файл не идентичен ТОЛЬКО именами локальных меток макросов ..@N (nasm нумерует с 108, kas с 1)"
    else
        bad "$name: расхождение с nasm"; diff <(dump $T/n.o) <(dump $T/k.o) | head -20
    fi
}

KERNEL="boot interrupts context ap_tramp uaccess"
if [ -z "${KAS_NO_NASM:-}" ] && command -v nasm >/dev/null 2>&1; then   # KAS_NO_NASM=1 имитирует отсутствие nasm
    for f in $KERNEL; do compare "arch/x86_64/$f.s" arch/x86_64/$f.s; done
    for f in cases64 cases32; do compare "tests/kas/$f.s" tests/kas/$f.s; done
else
    echo "SKIP: nasm не найден - сверка с nasm НЕ выполнена (пропущено 7 файлов)"; skip=1
    for f in $KERNEL; do $T/kas -f elf64 -o $T/k.o arch/x86_64/$f.s && ok "arch/x86_64/$f.s собирается (без сверки)" || bad "arch/x86_64/$f.s не собрался"; done
fi

# детерминизм: два запуска дают одинаковый файл
for f in $KERNEL; do
    $T/kas -o $T/a.o arch/x86_64/$f.s && $T/kas -o $T/b.o arch/x86_64/$f.s && cmp -s $T/a.o $T/b.o || bad "$f: недетерминированный вывод"
done
ok "детерминизм: повторная сборка даёт тот же файл"

# негативные тесты: первая строка ';; ожидается: N: подстрока' (или 'OK')
for f in tests/kas/bad/*.s; do
    exp=$(head -1 $f | sed -n 's/^;; ожидается: //p')
    rm -f $T/bad.o
    if [ "$exp" = "OK" ]; then
        $T/kas -o $T/bad.o $f 2>$T/err && ok "$f: собрался, как и ожидалось" || bad "$f: $(head -c 200 $T/err)"
        continue
    fi
    ln=${exp%%:*}; sub=${exp#*: }
    if $T/kas -o $T/bad.o $f 2>$T/err; then bad "$f: ожидалась ошибка '$exp', сборка прошла"; continue; fi
    if [ -e $T/bad.o ]; then bad "$f: при ошибке создан выходной файл"; continue; fi
    if grep -q "$f:$ln: ошибка: .*$sub" $T/err; then ok "$f: ошибка на строке $ln ($sub)"
    else bad "$f: ожидалось '$f:$ln: ошибка: ...$sub', получено: $(head -c 200 $T/err)"; fi
done

# фаззинг: sanitizer-ы, если компилятор их умеет
FZ="-O1 -g"
echo 'int main(void){return 0;}' > $T/s.c
$HOSTCC -fsanitize=address,undefined $T/s.c -o $T/s 2>/dev/null && FZ="$FZ -fsanitize=address,undefined -fno-sanitize-recover=undefined" || echo "SKIP: sanitizer-ы недоступны, фаззинг без них"
if $HOSTCC -std=c99 $FZ -Wall -Wextra -o $T/fuzz tests/kas_fuzz.c kas/kas.c; then
    for seed in 1 2; do
        out=$($T/fuzz -n ${KAS_FUZZ_N:-700} -s $seed arch/x86_64/*.s tests/kas/cases64.s tests/kas/cases32.s tests/kas/bad/macro_args.s 2>&1)
        [ $? = 0 ] && ok "фаззинг (зерно $seed): $(echo "$out" | tail -1)" || { bad "фаззинг (зерно $seed): $(echo "$out" | tail -3)"; }
    done
else bad "сборка фаззера"; fi

# самопроверка на заглушки теми же правилами, что ci/lint.sh (он работает по git diff, здесь файлы целиком)
pat='TODO|FIXME|XXX|HACK|stub|not implemented|unimplemented|NotImplemented|заглушк|пока не реализ|упрощённо вместо|имитаци|fake|dummy|placeholder'
L="kas/kas.c kas/kas.h kas/kas_main.c tests/kas_fuzz.c"
if grep -inE "$pat" $L | grep -v "pat='" ; then bad "lint: признаки заглушек"; fi
if grep -nE '\)\s*\{\s*(return\s+(0|-1|NULL);)?\s*\}\s*$' $L; then bad "lint: пустые функции"; fi
if grep -nE '\{\s*\}' kas/kas.c kas/kas_main.c tests/kas_fuzz.c; then bad "lint: пустой блок"; fi
ok "lint: заглушек и пустых блоков нет"

[ $fail = 0 ] && { [ $skip = 1 ] && echo "kas: PASS (сверка с nasm ПРОПУЩЕНА)" || echo "kas: PASS"; exit 0; }
echo "kas: FAIL"; exit 1
