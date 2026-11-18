#!/bin/bash
# ksh (userland/wasm/ksh.c): скрипты из tests/ksh прогоняются
#   1) хост-сборкой gcc (ASan+UBSan), вывод сверяется с tests/ksh_expected.txt;
#   2) build-wasm/ksh.wasm в хост-интерпретаторе WASM (tests/wasm_host.c) - режим интерпретатора (WASM_NOJIT=1, как на ARM64) и с JIT;
#      вывод и код возврата должны совпасть с хост-сборкой.
# Пересоздать ожидаемый вывод по хост-сборке: KSH_UPDATE=1 tests/ksh_test.sh (результат просмотреть глазами!).
# Аргументы: [рабочий каталог]. Переменные: CC, KSH_WASM (по умолчанию build-wasm/ksh.wasm), WASMH (готовый интерпретатор tests/wasm_host.c).
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
T=${1:-$(mktemp -d)}
mkdir -p "$T/ksh"
KSH_WASM=${KSH_WASM:-build-wasm/ksh.wasm}
export ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 UBSAN_OPTIONS=halt_on_error=1
$CC -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=undefined -o "$T/ksh_host" userland/wasm/ksh.c
if [ ! -f "$KSH_WASM" ] || [ userland/wasm/ksh.c -nt "$KSH_WASM" ]; then tools/ksh_wasm.sh >/dev/null 2>&1 || true; fi
HAVE_WASM=1
if [ ! -f "$KSH_WASM" ] || [ userland/wasm/ksh.c -nt "$KSH_WASM" ]; then
    HAVE_WASM=0; echo "ksh: ВНИМАНИЕ: $KSH_WASM не собран (нужны clang с wasm32, lld и sysroot, см. tools/wasi_sysroot.sh) - проверена только хост-сборка"
fi
if [ $HAVE_WASM = 1 ]; then
    WASMH=${WASMH:-$T/ksh_wasmh}
    [ -x "$WASMH" ] || $CC -O1 -g -w -I. -fsanitize=address,undefined -o "$WASMH" tests/wasm_host.c -lm
fi

OUT=$T/ksh_actual.txt; : > "$OUT"
fail() { echo "ksh: $*"; exit 1; }
# run_case ИМЯ СКРИПТ "АРГУМЕНТЫ" ФАЙЛ_ВВОДА
run_case() {
    local name=$1 script=$2 args=$3 inf=$4 stdin="" d="$T/ksh/$1" rc av
    read -ra av <<< "$args"        # строка запуска Киры делится по пробелам
    if [ -n "$inf" ]; then stdin=$(cat "$inf"; echo x); stdin=${stdin%x}; fi
    rm -rf "$d"; mkdir -p "$d/g"
    [ -n "$script" ] && cp "$script" "$d/g/"
    ( cd "$d/g"; printf '%s' "$stdin" | "$T/ksh_host" "${av[@]}" > "$d/host.out" 2>&1; rc=$?
      [ -z "$(tail -c1 "$d/host.out")" ] || echo >> "$d/host.out"      # вывод без последнего перевода строки (приглашение) - дописать
      echo "[код $rc]" >> "$d/host.out" ) || true
    { echo "### $name"; cat "$d/host.out"; } >> "$OUT"
    [ $HAVE_WASM = 1 ] || return 0
    local mode
    for mode in interp jit; do
        if [ $mode = interp ]; then export WASM_NOJIT=1; else unset WASM_NOJIT; fi
        rc=0
        if [ -n "$script" ]; then export WASM_FILE=$script; else unset WASM_FILE; fi
        WASM_STDIN="$stdin" WASM_ARGS="$args" "$WASMH" "$KSH_WASM" > "$d/wasm_$mode.raw" 2>&1 || rc=$?
        unset WASM_FILE
        grep -q "finished with code" "$d/wasm_$mode.raw" || { cat "$d/wasm_$mode.raw"; fail "$name ($mode): программа не завершилась штатно"; }
        { sed -e '/^wasm: /d' -e 's/wasm: finished with code [0-9]* in [0-9]* ms$//' "$d/wasm_$mode.raw"; echo "[код $(sed -n 's/.*wasm: finished with code \([0-9]*\).*/\1/p' "$d/wasm_$mode.raw")]"; } > "$d/wasm_$mode.out"
        cmp -s "$d/host.out" "$d/wasm_$mode.out" || { diff "$d/host.out" "$d/wasm_$mode.out" | head -30; fail "$name: вывод WASM ($mode) не совпал с хост-сборкой"; }
    done
    unset WASM_NOJIT
}

for s in tests/ksh/t*.sh; do b=$(basename "$s"); run_case "${b%.sh}" "$s" "$b one two"; done
run_case c_simple "" '-c echo hi; echo $((1+2)) $0 $#'
run_case c_exit "" '-c exit 3'
run_case c_status "" '-c false; echo st=$?; true'
run_case c_nocode "" '-c'
run_case bad_option "" '-q'
run_case flag_e "" '-e -c echo a; false; echo b'
run_case flag_x "" '-x -c echo traced'
run_case no_script "" 'absent.sh'
run_case bad_script_name "" 'dir/x.sh'
run_case repl_session "" "" tests/ksh/r01_repl.in
run_case repl_eof "" "" tests/ksh/r02_repl_eof.in
run_case repl_empty "" "" /dev/null

if [ -n "$KSH_UPDATE" ]; then cp "$OUT" tests/ksh_expected.txt; echo "ksh: tests/ksh_expected.txt обновлён"; fi
cmp -s "$OUT" tests/ksh_expected.txt || { diff tests/ksh_expected.txt "$OUT" | head -40; fail "вывод хост-сборки не совпал с tests/ksh_expected.txt"; }
if [ $HAVE_WASM = 1 ]; then
    echo "ksh: все сценарии совпали с ожидаемым выводом (хост gcc+ASan/UBSan) и с ksh.wasm (интерпретатор и JIT)"
else
    echo "ksh: все сценарии совпали с ожидаемым выводом (только хост gcc+ASan/UBSan, WASM пропущен)"
fi
