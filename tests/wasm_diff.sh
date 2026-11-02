#!/bin/bash
# Сверка JIT x86-64 с интерпретатором на сгенерированных модулях (tests/wasm_gen.py): вывод должен совпасть байт в байт,
# ловушки - совпасть по факту остановки. Использование: tests/wasm_diff.sh ХОСТ-ПРОГРАММА РАБОЧАЯ-ПАПКА [число случайных]
H=$1; W=$2; NR=${3:-60}
rm -rf "$W/ops" "$W/traps" "$W/rnd" "$W/deep"; mkdir -p "$W/ops" "$W/traps" "$W/rnd" "$W/deep"
python3 tests/wasm_gen.py ops "$W/ops" && python3 tests/wasm_gen.py traps "$W/traps" && python3 tests/wasm_gen.py random "$W/rnd" 1 "$NR" && python3 tests/wasm_gen.py deep "$W/deep" || exit 1
bad=0; n=0; jitted=0
run() {   # режим файл -> нормализованный вывод
    if [ "$1" = i ]; then WASM_NOJIT=1 "$H" "$2" 2>&1; else "$H" "$2" 2>&1; fi | grep -a -v "^wasm: .* functions" | sed -e 's/ in [0-9]* ms//' -e 's/^wasm: stopped: .*/wasm: STOPPED/' -e '/^wasm: x86-64 native code/d' -e '/^wasm: interpreter mode/d'
}
for f in "$W"/ops/*.wasm "$W"/traps/*.wasm "$W"/rnd/*.wasm "$W"/deep/*.wasm; do
    n=$((n+1))
    run j "$f" > "$W/j.out"; run i "$f" > "$W/i.out"
    if "$H" "$f" 2>&1 | grep -aq "native code"; then jitted=$((jitted+1)); fi
    if ! cmp -s "$W/j.out" "$W/i.out"; then bad=$((bad+1)); echo "РАЗЛИЧИЕ: $f"; [ $bad -le 3 ] && cmp "$W/j.out" "$W/i.out" | head -2; fi
done
echo "wasm_diff: $n модулей, из них JIT-компилировано $jitted, различий $bad"
[ $bad -eq 0 ] && [ $jitted -ge $((n * 9 / 10)) ]
