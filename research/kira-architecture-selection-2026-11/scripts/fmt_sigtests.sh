#!/bin/bash
# [E] Подпись/манифест/анти-откат: прогон существующих хост-тестов KIRA (команды сборки как в tests/run_host_tests.sh) на ядре 0.
# Исходники берутся из клона (тот же коммит, что и исследование). Результат: logs/fmt_sigtests.log + строки PASS/FAIL в results_fmt.csv
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_sig; cd "$FMT_REPO" || exit 1
L=$FMT_LOGS/fmt_sigtests.log; : > $L; T=$(mktemp -d)
FLAGS="-O1 -g -Wall -Werror -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie"
run() { # имя; остальное - команда сборки+запуска
  local n=$1; shift; echo "== $n" >> $L
  if $C0 bash -c "$*" >> $L 2>&1; then echo "-> PASS" >> $L; frec x86_64 host-test "$n" status 1 PASS flag 0 "сборка gcc -fsanitize=address,undefined"; else echo "-> FAIL" >> $L; frec x86_64 host-test "$n" status 1 FAIL flag 1 "см. logs/fmt_sigtests.log"; fi; }
run kdrv_format "gcc $FLAGS -fsanitize=address,undefined -o $T/kdrv tests/kdrv_test.c kdrv_fmt.c && $T/kdrv"
run pk_ed25519_rsa_ecdsa "gcc $FLAGS -fsanitize=address,undefined -o $T/pk tests/pk_test.c pk.c crypto.c sha256.c && $T/pk"
run kdrv_sig "gcc $FLAGS -fsanitize=address,undefined -o $T/ks tests/kdrv_sig_test.c kdrv_sig.c pk.c crypto.c sha256.c && $T/ks"
run kdrvsign_py "python3 -m unittest ci.test_kdrvsign"
run kdrv_state "gcc $FLAGS -fsanitize=address,undefined -o $T/kst tests/kdrv_state_test.c kdrv_state.c kdrv_sig.c kdrv_fmt.c pk.c crypto.c sha256.c && $T/kst && python3 ci/gen_kdrv_state_vectors.py | cmp - tests/kdrv_state_vectors.h"
run kdrv_catalogue "gcc $FLAGS -fsanitize=address,undefined -o $T/kc tests/kdrv_catalogue_test.c drivers/kdrv/kdrv_catalogue.c drivers/kdrv/kdrv_trust.c pk.c crypto.c sha256.c && $T/kc"
run drvget "gcc $FLAGS -fsanitize=address,undefined -o $T/dg tests/drvget_test.c userland/drvget_core.c sha256.c && $T/dg"
run elf_loader_kloader_host_BUILD_ONLY "gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c && echo built"
run bootinfo "gcc $FLAGS -fsanitize=address,undefined -o $T/bi tests/bootinfo_test.c bootinfo.c && $T/bi"
run bootinfo_mb2 "gcc $FLAGS -fsanitize=address,undefined -o $T/bm tests/bootinfo_mb2_test.c bootinfo_mb2.c bootinfo.c && $T/bm"
run fdt "gcc $FLAGS -fsanitize=address,undefined -o $T/fdt tests/fdt_test.c fdt.c bootinfo_fdt.c bootinfo.c && $T/fdt"
run kfs "gcc $FLAGS -fsanitize=address,undefined -Itests -o $T/kfs tests/kfs_test.c tests/kfs_v1_ref.c kfs.c store.c && $T/kfs"
echo "== состав тестов kdrv_test/ks по тестам-подписи: сколько CHECK" >> $L; grep -c 'CHECK(' tests/kdrv_sig_test.c tests/kdrv_state_test.c tests/kdrv_test.c >> $L
rm -rf $T; grep -c PASS $L; grep -B1 -- '-> FAIL' $L | head
