#!/bin/bash
# [E] Находка F1 (низкая серьёзность): воспроизведение под ASAN. Лог: logs/fmt_fuzz_fdt_finding.log
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_fuzz; cd "$FMT_REPO" || exit 1; T=$(mktemp -d -p ${TMPDIR:-/tmp}); L=$FMT_LOGS/fmt_fuzz_fdt_finding.log
R=research/kira-architecture-selection-2026-11
gcc -g -O0 -fsanitize=address,undefined -DKIRA_HOST_TEST -I. -w -o $T/r $R/scripts/fmt_fuzz_repro_fdt.c bootinfo_fdt.c fdt.c bootinfo.c
$C0 $T/r $R/data/fuzz_seeds/fdt-short-4B.bin > $L 2>&1; rc=$?; grep -m4 'ERROR\|READ of\|#[0-3] ' $L | cut -c1-200
frec x86_64 fdt_valid finding_F1 asan_exit 1 $rc code $rc "4-байтовый вход d00dfeed: heap-buffer-overflow READ 1 в fdt_u32 (fdt.c:9) через fdt_valid; rc!=0 - воспроизведено"
rm -rf $T
