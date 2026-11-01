#!/bin/bash
# Драйвер drivers/rl/rl.c против модели RTL8139 (tests/rl_test.c) на хосте: tests/rl_build.sh [каталог-сборки]
# Заголовки kira.h и kdrv_bsd.h копируются из userland; в копиях подменено только то, что нельзя сделать на хосте:
# системный вызов (ksys - функция теста, играющая ядро) и доступ к регистрам (bus_space_read/write - вызовы модели чипа).
set -e
cd "$(dirname "$0")/.."
T=${1:-$(mktemp -d)}
R=$T/rlh; mkdir -p $R
cp userland/kdrv.h userland/kdrv_bsd.h kdrv_format.h caps.h drivers/rl/if_rlreg.h $R/
python3 -I - "$R" <<'PY'
import re, sys
r = sys.argv[1]
s = open('userland/kira.h', encoding='utf-8').read()
s = s.replace('static inline i64 ksys(u64 n, u64 a, u64 b, u64 c) {', 'static inline i64 ksys_unused(u64 n, u64 a, u64 b, u64 c) {')
s = s.replace('typedef unsigned long long u64; typedef long long i64;', 'typedef unsigned long long u64; typedef long long i64;\ni64 ksys(u64 n, u64 a, u64 b, u64 c);', 1)
assert 'i64 ksys(u64 n, u64 a, u64 b, u64 c);' in s and s.count('ksys_unused') >= 1
open(r + '/kira.h', 'w', encoding='utf-8').write(s)
b = open('userland/kdrv_bsd.h', encoding='utf-8').read()
for w in '124':
    b, n = re.subn(r'#define bus_space_read_%s\(h, o\).*' % w, 'u32 rtl_model_read(int, int);\n#define bus_space_read_%s(h, o) ((u%d)rtl_model_read(%s, (o)))' % (w, {'1': 8, '2': 16, '4': 32}[w], w), b)
    assert n == 1
    b, n = re.subn(r'#define bus_space_write_%s\(h, o, v\).*' % w, 'void rtl_model_write(int, int, u32);\n#define bus_space_write_%s(h, o, v) rtl_model_write(%s, (o), (u32)(v))' % (w, w), b)
    assert n == 1
open(r + '/kdrv_bsd.h', 'w', encoding='utf-8').write(b)
PY
CC=${CC:-gcc}
$CC -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined -I$R -I. -Dmain=rl_main -c drivers/rl/rl.c -o $T/rl_drv.o
$CC -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -I. -c tests/rl_test.c -o $T/rl_test.o
$CC -fsanitize=address,undefined -o $T/rl_test $T/rl_test.o $T/rl_drv.o
export ASAN_OPTIONS=detect_leaks=0
for m in 0 1 2; do $T/rl_test $m | tail -1; done
# тот же драйвер собирает kcc обеих архитектур (так он собирается на устройстве: drvget confirm rl)
$CC -std=c99 -O1 -o $T/kcc_host kcc/*.c
$T/kcc_host -I kcc/include -o $T/rl_x86.elf drivers/rl/rl.c
$T/kcc_host -target aarch64 -I kcc/include -o $T/rl_a64.elf drivers/rl/rl.c
echo "rl.c: kcc x86-64 $(stat -c %s $T/rl_x86.elf) байт, kcc AArch64 $(stat -c %s $T/rl_a64.elf) байт - собирается"
