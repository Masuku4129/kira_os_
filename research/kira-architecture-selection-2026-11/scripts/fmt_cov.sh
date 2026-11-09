#!/bin/bash
# [E] Покрытие строк разбираемых файлов после N мутаций (gcc --coverage, без санитайзеров), по цели. Ядро 0. Использование: fmt_cov.sh [N=1000000] [NSIG=30000]
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_cov; cd "$FMT_REPO" || exit 1
N=${1:-1000000}; NSIG=${2:-30000}; R=research/kira-architecture-selection-2026-11/scripts; T=$(mktemp -d -p ${TMPDIR:-/tmp}); L=$FMT_LOGS/fmt_cov.log; : > $L
SRCS="$R/fmt_fuzz.c $R/fmt_kex.c elf.c kdrv_fmt.c kdrv_sig.c kdrv_state.c pk.c crypto.c sha256.c bootinfo.c bootinfo_mb2.c bootinfo_uefi.c bootinfo_fdt.c fdt.c kfs.c store.c"
mkdir -p $T/o; for s in $SRCS; do gcc --coverage -O0 -g -DKIRA_HOST_TEST -DKFS_NO_THREADS -I. -Iarch/x86_64 -w -c $s -o $T/o/$(echo $s | tr / _).o || exit 1; done
gcc --coverage -o $T/ff $T/o/*.o
python3 -I $R/../scripts/fmt_mkkex.py $FMT_ART/userland/edit.elf $T/e.kex --synth-relocs 20
python3 -I - $T <<'PY'
import re, struct, sys
t = sys.argv[1]; s = open('tests/kdrv_sig_test.c').read(); blk = s[s.index('valid_signature = {'):]; hb = lambda b: bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', b))
b = struct.pack('<IHH', 0x31474953, 1, 144) + b'TEST-ED25519-001' + hb(blk[blk.index('.package_sha256'):blk.index('.package_size')]) + struct.pack('<QQQ', 35, 1700000000, 4102444800) + hb(blk[blk.index('.signature'):])[:64]; open(t + '/sig.bin', 'wb').write(b)
d = bytearray(open('research/kira-architecture-selection-2026-11/data/fuzz_seeds/virt.dtb', 'rb').read()); ost, ostr, sstr, sst = struct.unpack('>IIII', d[8:12] + d[12:16] + d[32:36] + d[36:40]); end = (max(ostr + sstr, ost + sst) + 7) & ~7; d[4:8] = struct.pack('>I', end); open(t + '/virt.dtb', 'wb').write(d[:end])
PY
cov() { # цель N файл-для-gcov... -- сиды
  local tg=$1 n=$2; shift 2; local files=(); while [ "$1" != "--" ]; do files+=("$1"); shift; done; shift
  rm -f $T/o/*.gcda; $C0 $T/ff $tg $n 1 $T "$@" > $T/out.txt 2>&1 || { echo "$tg: падение при сборе покрытия"; return; }
  for f in "${files[@]}"; do
    g=$(cd $T/o && gcov -b -o . $T/o/$(echo $f | tr / _).gcno 2>/dev/null | grep -A3 "File '$f'" | head -4 | tr '\n' ' ')
    pct=$(echo "$g" | grep -o 'Lines executed:[0-9.]*% of [0-9]*' | head -1); br=$(echo "$g" | grep -o 'Branches executed:[0-9.]*% of [0-9]*' | head -1); tk=$(echo "$g" | grep -o 'Taken at least once:[0-9.]*% of [0-9]*' | head -1)
    echo "$tg N=$n $f: $pct; $br; $tk" | tee -a $L
    v=$(echo "$pct" | grep -o '[0-9.]*%' | tr -d %); [ -n "$v" ] && frec x86_64 cov/$tg $f line_coverage 1 $v percent 0 "gcov, N=$n; весь файл, включая строки вне разбора (strerror, encode)"
    v=$(echo "$tk" | grep -o ':[0-9.]*%' | tr -d ':%'); [ -n "$v" ] && frec x86_64 cov/$tg $f branch_taken_coverage 1 $v percent 0 "gcov -b, N=$n"
  done; rm -f *.gcov
}
cov elf $N elf.c -- $FMT_ART/userland/edit.elf $FMT_ART/userland/paint.elf $FMT_ART/userland/atk-cap.elf
cov kex $N $R/fmt_kex.c -- $T/e.kex
cov kdrv $N kdrv_fmt.c --
cov krec $N kdrv_fmt.c --
cov ksighdr $N kdrv_sig.c -- $T/sig.bin
cov ksig $NSIG kdrv_sig.c -- $T/sig.bin
cov kstate $N kdrv_state.c --
cov kbi $N bootinfo.c --
cov mb2 $N bootinfo_mb2.c --
cov kbi2mb $N bootinfo_uefi.c --
cov fdt $N fdt.c bootinfo_fdt.c -- $T/virt.dtb
cov kfs $((N/5)) kfs.c --
rm -rf $T
