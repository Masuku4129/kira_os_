#!/bin/bash
# [E] Существующий негативный тест BIOS-цепочки (tests/bios_boot_test.sh: загрузка, порча CRC32, p_paddr<1 МиБ, нехватка RAM) на готовом kira.bin; ядро 0.
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_boot; cd "$FMT_REPO" || exit 1
$C0 bash tests/bios_boot_test.sh $FMT_ART/kira.bin > $FMT_LOGS/fmt_bios_boot_test.log 2>&1; rc=$?
tail -1 $FMT_LOGS/fmt_bios_boot_test.log; frec x86_64 bios_boot_test negative_cases status 1 $([ $rc = 0 ] && echo PASS || echo FAIL) flag $rc "6 проверок: загрузка, W^X, CRC, p_paddr, 16 МиБ, 64 МиБ"
