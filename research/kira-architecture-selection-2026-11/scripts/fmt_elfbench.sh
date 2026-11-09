#!/bin/bash
# [E] время разбора elf_parse vs kex_parse (прототип) на реальных userland-ELF KIRA; >=9 повторов по 1000 разборов; ядро 0.
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_parse; cd "$FMT_REPO" || exit 1; L=$FMT_LOGS/fmt_elfbench.log; : > $L; T=$(mktemp -d -p ${TMPDIR:-/tmp})
R=research/kira-architecture-selection-2026-11/scripts
PROGS="edit kcc fetch paint atk-cap"; ARGS=""
for p in $PROGS; do python3 -I $R/fmt_mkkex.py $FMT_ART/userland/$p.elf $T/$p.kex; ARGS="$ARGS $FMT_ART/userland/$p.elf $T/$p.kex"; done
python3 -I $R/fmt_mkkex.py $FMT_ART/userland/edit.elf $T/edit_r1000.kex --synth-relocs 1000; python3 -I $R/fmt_mkkex.py $FMT_ART/userland/edit.elf $T/edit_r10000.kex --synth-relocs 10000
for cfg in "gcc-O2|gcc -O2" "clang-O2|clang -O2"; do
  nm=${cfg%%|*}; cc=${cfg#*|}
  $cc -DKIRA_HOST_TEST -I. -Iarch/x86_64 -w -o $T/b_$nm $R/fmt_elfbench.c $R/fmt_kex.c elf.c sha256.c || { frec x86_64 $nm parse build 1 FAIL flag 1 "не собралось"; continue; }
  echo "== $nm" >> $L; $C0 $T/b_$nm 11 1000 $ARGS $FMT_ART/userland/edit.elf $T/edit_r1000.kex $FMT_ART/userland/edit.elf $T/edit_r10000.kex > $T/$nm.out 2>&1; rc=$?; cat $T/$nm.out >> $L
  # edit_r*: ELF не содержит перемещений => различие в имени файла помечаем
  i=0; while read rep m f v; do [ "$rep" = OK ] && continue; frec x86_64 $nm "$m/$f" time $rep $v ns $rc "n=1000 разборов/повтор (для +sha256: 101); прогрев 2"; done < <(grep -E '^[0-9]+ ' $T/$nm.out)
  grep -E '^(OK|FAIL)' $T/$nm.out | tee -a $L > /dev/null
done
# статические метрики разбора
count_ret() { grep -c "return $2" "$1"; }
ELOC=$(grep -vE '^\s*(/\*.*\*/)?\s*$' elf.c | wc -l); KLOC=$(grep -vE '^\s*(/\*.*\*/)?\s*$' $R/fmt_kex.c | wc -l)
ECHK=$(grep -cE 'return ELF_ERR' elf.c); KCHK=$(grep -cE 'return KEX_ERR' $R/fmt_kex.c)
echo "== статика: elf.c непустых строк=$ELOC (из них elf_strerror ~12), return ELF_ERR=$ECHK; fmt_kex.c непустых строк=$KLOC, return KEX_ERR=$KCHK (включая kex_verify_hash)" | tee -a $L
frec x86_64 elf_parse code loc 1 $ELOC lines 0 "непустые строки elf.c"; frec x86_64 kex_parse code loc 1 $KLOC lines 0 "непустые строки fmt_kex.c (включая reloc и hash)"
frec x86_64 elf_parse code error_returns 1 $ECHK count 0 "число return ELF_ERR (мест отказа по границам/форме)"; frec x86_64 kex_parse code error_returns 1 $KCHK count 0 "число return KEX_ERR"
# размеры: ELF (strip) vs KEX по всем 26 программам
FMT_CSV=$FMT_CSV python3 -I - $FMT_ART $R <<'PY' | tee -a $L
import sys, glob, os, subprocess, tempfile
sys.path.insert(0, sys.argv[2]); import fmt_mkkex
import zlib, lzma
tot_e = tot_s = tot_k = 0; rows = []; blk_s = blk_k = 0; zs = zk = xs = xk = 0
for e in sorted(glob.glob(sys.argv[1] + '/userland/*.elf')):
    d = tempfile.mkdtemp(); st = d + '/s'; subprocess.check_call(['strip', '-s', '-o', st, e])
    k = fmt_mkkex.convert(open(st, 'rb').read()); k0 = fmt_mkkex.convert(open(e, 'rb').read())
    tot_e += os.path.getsize(e); tot_s += os.path.getsize(st); tot_k += len(k); rows.append((os.path.basename(e), os.path.getsize(st), len(k))); sb = open(st, 'rb').read(); blk_s += -(-len(sb) // 4096); blk_k += -(-len(k) // 4096); zs += len(zlib.compress(sb, 9)); zk += len(zlib.compress(k, 9)); xs += len(lzma.compress(sb, preset=9)); xk += len(lzma.compress(k, preset=9))
print('== KEX vs ELF по %d программам: ELF %d, strip-ELF %d, KEX(данные подряд, без symtab) %d; экономия KEX к strip-ELF = %d Б (%.1f%%)' % (len(rows), tot_e, tot_s, tot_k, tot_s - tot_k, 100 * (tot_s - tot_k) / tot_s))
print('== блоки KiraFS (4 КБ, KFS_BLOCK): strip-ELF %d блоков (%d КБ), KEX %d блоков (%d КБ)' % (blk_s, blk_s * 4, blk_k, blk_k * 4))
print('== после сжатия (все 26 по отдельности): zlib-9 strip-ELF %d / KEX %d; xz-9 strip-ELF %d / KEX %d' % (zs, zk, xs, xk))
for r in rows: print('  %-10s strip-ELF %7d  KEX %7d' % r)
for nm, v, u in (('elf_total', tot_e, 'B'), ('elf_strip_total', tot_s, 'B'), ('kex_total', tot_k, 'B'), ('kfs_blocks_strip_elf', blk_s, 'blocks'), ('kfs_blocks_kex', blk_k, 'blocks'), ('zlib9_strip_elf', zs, 'B'), ('zlib9_kex', zk, 'B'), ('xz9_strip_elf', xs, 'B'), ('xz9_kex', xk, 'B')):
    open(os.environ['FMT_CSV'], 'a').write('fmt_parse,x86_64,vm-core0,sizes26,user/ALL,%s,1,%d,%s,0,"26 программ userland"\n' % (nm, v, u))
PY
rm -rf $T
