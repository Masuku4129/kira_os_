#!/bin/bash
# Полный воспроизводимый прогон секции «форматы» (порядок важен: K3 в datalab берёт журнал из fmt_boot bios). ВСЁ на ядре 0. Занимает ~1.5 часа.
# Результат: data/results_fmt.csv (пересоздаётся), logs/fmt_*. Артефакты сборки читаются из $FMT_ART (по умолчанию /home/claude/kira_os_), там ничего не пишется.
cd "$(dirname "$0")" || exit 1; . ./fmt_env.sh; rm -f "$FMT_CSV"; echo "exp,arch,env,candidate,workload,metric,rep,value,unit,exit_code,note" > "$FMT_CSV"
./fmt_inventory.sh
$C0 python3 -I fmt_sizes.py > /dev/null
./fmt_sigtests.sh
./fmt_edbench.sh
./fmt_elfbench.sh
./fmt_fuzz.sh 1000000 300000
./fmt_fuzz_repro_fdt.sh
./fmt_cov.sh 1000000 30000
./fmt_fuzz_existing.sh 12000 50000 1000000 20000
./fmt_bios_boot_test.sh
$C0 python3 -I fmt_boot.py bios 6; $C0 python3 -I fmt_boot.py uefi 6; $C0 python3 -I fmt_boot.py arm64 5 150
./fmt_klog_capture.sh
$C0 python3 -I fmt_datalab.py
$C0 python3 -I fmt_wasm.py 9
