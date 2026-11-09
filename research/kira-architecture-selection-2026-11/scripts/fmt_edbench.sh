#!/bin/bash
# [E] мкс на проверку подписи Ed25519 / пакета KDRV / SHA-256 1 МиБ. gcc -O2 и clang -O2 -mno-sse -mno-mmx (флаги ядра). Ядро 0.
. "$(dirname "$0")/fmt_env.sh"; FMT_EXP=fmt_ed; cd "$FMT_REPO" || exit 1; L=$FMT_LOGS/fmt_edbench.log; : > $L; T=$(mktemp -d)
S="research/kira-architecture-selection-2026-11/scripts/fmt_edbench.c kdrv_sig.c pk.c crypto.c sha256.c"
for cfg in "gcc-O2|gcc -O2" "clang-O2-nosse|clang -O2 -mno-sse -mno-mmx"; do
  nm=${cfg%%|*}; cc=${cfg#*|}
  $cc -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie -w -o $T/$nm $S || { frec x86_64 $nm ed25519 build 1 FAIL flag 1 "не собралось"; continue; }
  echo "== $nm ($cc)" >> $L; $C0 $T/$nm 300 > $T/$nm.out 2>&1; rc=$?; cat $T/$nm.out >> $L
  while read rep metric v; do frec x86_64 $nm "${metric%_ns}" time $rep $(awk "BEGIN{printf \"%.2f\", $v/1000}") us $rc "хост, ns/1000; Ed25519 pk.c KIRA"; done < <(grep -E '^[0-9]+ ' $T/$nm.out)
done
python3 - >> $L <<'PY'
import time, statistics
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
k = Ed25519PrivateKey.generate(); m = b'x' * 88; s = k.sign(m); pk = k.public_key()
ts = []
for r in range(11):
    t = time.perf_counter()
    for _ in range(2000): pk.verify(s, m)
    ts.append((time.perf_counter() - t) / 2000 * 1e6)
print('== OpenSSL (python cryptography) ed25519 verify us: med %.1f min %.1f max %.1f (n=11 x 2000)' % (statistics.median(ts), min(ts), max(ts)))
PY
grep 'OpenSSL' $L | sed 's/.*med \([0-9.]*\).*/\1/' | while read v; do frec x86_64 openssl-libcrypto ed25519_verify time 1 $v us 0 "[X] справка: python cryptography, медиана из 11x2000"; done
rm -rf $T
python3 -I - "$L" <<'PY'
import sys, statistics
cur = None; rows = {}
for l in open(sys.argv[1]):
    p = l.split()
    if l.startswith('=='): cur = p[1]
    elif len(p) == 3 and p[0].isdigit() and cur: rows.setdefault((cur, p[1]), []).append(int(p[2]))
for (c, m), v in rows.items(): print('%-16s %-20s med %8d ns  min %8d  max %8d  n=%d' % (c, m, statistics.median(v), min(v), max(v), len(v)))
PY
