#!/bin/bash
# [E] Фаззинг парсеров форматов KIRA: свой мутатор fmt_fuzz.c + gcc ASAN/UBSAN (-fno-sanitize-recover=all), N запусков на цель, ядро 0.
# Использование: scripts/fmt_fuzz.sh [N=1000000] [NSIG=200000] ; цели и сиды описаны в fmt_fuzz.c. Результат: logs/fmt_fuzz_*.log, results_fmt.csv, logs/fmt_fuzz_crash/ (при падении).
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_fuzz; cd "$FMT_REPO" || exit 1
N=${1:-1000000}; NSIG=${2:-300000}; R=research/kira-architecture-selection-2026-11/scripts; T=$(mktemp -d -p ${TMPDIR:-/tmp}); CR=$FMT_LOGS/fmt_fuzz_crash; mkdir -p $CR
SRCS="$R/fmt_fuzz.c $R/fmt_kex.c elf.c kdrv_fmt.c kdrv_sig.c kdrv_state.c pk.c crypto.c sha256.c bootinfo.c bootinfo_mb2.c bootinfo_uefi.c bootinfo_fdt.c fdt.c kfs.c store.c"
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DKIRA_HOST_TEST -DKFS_NO_THREADS -I. -Iarch/x86_64 -w -o $T/ff $SRCS || { frec x86_64 fmt_fuzz build build 1 FAIL flag 1 "gcc не собрал"; exit 1; }
# сиды: настоящие userland-ELF, KEX из них, DTB QEMU virt, подпись из tests/kdrv_sig_test.c
python3 -I - $T <<'PY'
import sys, re
t = sys.argv[1]; s = open('tests/kdrv_sig_test.c').read()
blk = s[s.index('valid_signature = {'):]; hexb = lambda b: bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', b))
sha = hexb(blk[blk.index('.package_sha256'):blk.index('.package_size')]); sg = hexb(blk[blk.index('.signature'):])[:64]
import struct
b = struct.pack('<IHH', 0x31474953, 1, 144) + b'TEST-ED25519-001' + sha + struct.pack('<QQQ', 35, 1700000000, 4102444800) + sg
assert len(b) == 144; open(t + '/sig.bin', 'wb').write(b)
PY
for p in edit paint atk-cap drvedu; do python3 -I $R/fmt_mkkex.py $FMT_ART/userland/$p.elf $T/$p.kex --synth-relocs $([ $p = edit ] && echo 20 || echo 0); done
python3 -I - $T <<'PY'
import struct, sys
d = bytearray(open('research/kira-architecture-selection-2026-11/data/fuzz_seeds/virt.dtb', 'rb').read())
ost, ostr, sstr, sst = struct.unpack('>IIII', d[8:12] + d[12:16] + d[32:36] + d[36:40]); end = (max(ostr + sstr, ost + sst) + 7) & ~7
d[4:8] = struct.pack('>I', end); open(sys.argv[1] + '/virt.dtb', 'wb').write(d[:end]); print('dtb trimmed to', end)
PY
DTB=$T/virt.dtb   # исходный дамп: qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=virt.dtb -cpu cortex-a72 -smp 4 -m 1G (QEMU пишет totalsize=1 МиБ; в data/fuzz_seeds лежит уже обрезанный 8 КБ)
run() { # цель N файлы...
  local tg=$1 n=$2; shift 2; local t0=$(date +%s)
  $C0 $T/ff $tg $n 1 $CR "$@" > $T/$tg.out 2> $T/$tg.err; local rc=$?; local dt=$(( $(date +%s)-t0 ))
  { echo "== $tg N=$n rc=$rc wall=${dt}s"; cat $T/$tg.out; tail -5 $T/$tg.err; } >> $FMT_LOGS/fmt_fuzz_$tg.log
  frec x86_64 fmt_fuzz $tg runs 1 $n count $rc "ASAN+UBSAN, N запусков, wall ${dt} с; rc=0 - падений нет"
  grep -o 'принято парсером [0-9]*' $T/$tg.out | grep -o '[0-9]*' | while read a; do frec x86_64 fmt_fuzz $tg accepted_by_parser 1 $a count $rc "принятые мутанты (прошли все проверки)"; done
  echo "$tg rc=$rc wall=${dt}s: $(head -c 400 $T/$tg.out | head -1)"
}
rm -f $FMT_LOGS/fmt_fuzz_*.log
run elf $N $FMT_ART/userland/edit.elf $FMT_ART/userland/paint.elf $FMT_ART/userland/atk-cap.elf $FMT_ART/userland/drvedu.elf
run kex $N $T/edit.kex $T/paint.kex $T/atk-cap.kex $T/drvedu.kex
run kdrv $N
run krec $N
run ksighdr $N $T/sig.bin
run ksig $NSIG $T/sig.bin
run kstate $N
run kbi $N
run mb2 $N
run kbi2mb $N
run fdt $N $DTB
run kfs $N
rm -rf $T; ls $CR
