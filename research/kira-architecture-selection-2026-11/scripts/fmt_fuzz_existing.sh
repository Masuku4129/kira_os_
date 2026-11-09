#!/bin/bash
# [E] Прогон УЖЕ СУЩЕСТВУЮЩИХ фаззеров KIRA (tests/kas_fuzz.c, tests/klink/fuzz.c, tests/kcc/fuzz.c, wasm_host fuzz) с увеличенным числом итераций; ASAN+UBSAN, ядро 0.
# Использование: fmt_fuzz_existing.sh [KAS_N KLINK_ITER KCC_ITER WASM_N]  (по умолчанию значения ниже). Каждый запуск ограничен timeout (по умолч. 840 с).
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_fuzzx; cd "$FMT_REPO" || exit 1
KAS_N=${1:-20000}; KLINK_ITER=${2:-20000}; KCC_ITER=${3:-60000}; WASM_N=${4:-60000}; TMO=${TMO:-840}
L=$FMT_LOGS/fmt_fuzz_existing.log; [ -n "${ONLY:-}" ] || : > $L; T=$(mktemp -d -p ${TMPDIR:-/tmp})
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
ONLY=${ONLY:-}   # ONLY=kcc - прогнать только один фаззер (дополнительный прогон с другим числом итераций; лог дописывается)
one() { # имя "число из вывода" команда...
  local nm=$1 re=$2; shift 2; case "$nm" in ${ONLY:-*}*) ;; *) return ;; esac; local t0=$(date +%s); echo "== $nm: $*" >> $L
  ASAN_OPTIONS=detect_leaks=0 $C0 timeout $TMO "$@" > $T/o.txt 2>&1; local rc=$?; local dt=$(( $(date +%s)-t0 ))
  tail -3 $T/o.txt >> $L; echo "rc=$rc wall=${dt}s" >> $L
  local cnt=$(grep -oE "$re" $T/o.txt | head -1 | grep -oE '[0-9]+' | head -1)
  frec x86_64 existing/$nm all runs 1 ${cnt:-0} count $rc "wall ${dt} с; rc=0 - падений нет; rc=124 - timeout; число взято из вывода самого фаззера"; echo "$nm: rc=$rc wall=${dt}s запусков=${cnt:-?}"
}
# kas (ассемблер)
gcc -std=c99 -O1 -g $SAN -Wall -o $T/kas_fuzz tests/kas_fuzz.c kas/kas.c 2>$T/e || { cat $T/e | head -3; }
for seed in 1 2; do one kas_seed$seed 'прогонов [0-9]+' $T/kas_fuzz -n $KAS_N -s $seed arch/x86_64/*.s tests/kas/cases64.s tests/kas/cases32.s tests/kas/bad/macro_args.s; done
# klink (компоновщик ET_REL -> ET_EXEC): объекты nasm ядра и скрипты ядра
gcc -std=c99 -D_DEFAULT_SOURCE -O1 -g $SAN -o $T/klink_fuzz tests/klink/fuzz.c klink/klink.c 2>$T/e || head -3 $T/e
OBJS="$FMT_ART/boot.o $FMT_ART/interrupts.o $FMT_ART/context.o $FMT_ART/ap_tramp.o $FMT_ART/uaccess_s.o"
for seed in 1 2 3; do one klink_seed$seed 'fuzz: [0-9]+ runs' $T/klink_fuzz $KLINK_ITER $seed arch/x86_64/linker.ld $OBJS; done
# kcc (компилятор C)
gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter $SAN -o $T/kcc_fuzz tests/kcc/fuzz.c $(ls kcc/*.c | grep -v main.c) 2>$T/e || head -3 $T/e
one kcc 'итераций [0-9]+' $T/kcc_fuzz $KCC_ITER tests/kcc/ok/*.c tests/kcc/gnu/*.c tests/kcc/const/*.c tests/kcc/bad/*.c tests/kcc/run/*.c tests/kcc/unsup/*.c
# WASM-интерпретатор KIRA (загрузчик модулей + исполнение с топливом): повреждённые копии настоящих модулей build-wasm
gcc -O1 -g -w -DWASM_FUEL -I. $SAN -o $T/wasmfz tests/wasm_host.c -lm 2>$T/e || head -3 $T/e
for m in wtest ftest wgfx wmix wmix0; do one wasm_$m 'copies|\([0-9]+ damaged' $T/wasmfz fuzz $FMT_ART/build-wasm/$m.wasm $WASM_N; done
rm -rf $T; cat $L | grep -c 'rc=0'
