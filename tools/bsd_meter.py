#!/usr/bin/env python3
"""Счётчик совместимости по индексу: компилирует (clang -fsyntax-only) все файлы каждого драйвера индекса через слой kcc/include/bsd.
  bsd_meter.py SYS_DIR INDEX PCIDEVS [имя...]    итог: сколько драйверов целиком компилируется; для остальных - первая ошибка и частота причин"""
import collections, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsd_pcidevs as P
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def main(sysd, index, pcidevs, only):
    vend, prod = P.parse(pcidevs); ok = 0; tot = 0; reasons = collections.Counter(); per = {}
    for ln in open(index):
        if not ln.startswith('drv '): continue
        _, name, ca, pci, ic, ids, flags = ln.split()
        if only and name not in only: continue
        files = [pci] + ([] if ic == '-' else ic.split(','))
        tot += 1
        with tempfile.TemporaryDirectory() as g:
                        srcs = [os.path.join(sysd, f) for f in files]
            names = P.referenced(srcs); out, bad = P.resolve(vend, prod, names)
            open(g + '/kd_pcidevs.h', 'w').write(''.join('#define %s 0x%04x\n' % kv for kv in out.items()))
            first = None
            for s in srcs:
                r = subprocess.run(['clang', '-fsyntax-only', '-D_KERNEL', '-ffreestanding', '-Wno-everything', '-I' + g, '-I' + ROOT + '/kcc/include/bsd', '-I' + sysd,
                                    '-I' + ROOT + '/kcc/include', '-I' + ROOT + '/userland', '-I' + ROOT, s], capture_output=True, text=True)
                if r.returncode:
                    errs = [l for l in r.stderr.splitlines() if 'error' in l]
                    first = errs[0] if errs else r.stderr[:200]
                    for e in errs:
                        m = re.search(r"(file not found|undeclared identifier|undeclared function|incomplete type|no member named|unknown type name).*?'([^']+)'", e)
                        reasons[(m.group(1) + ' ' + m.group(2)) if m else e.split('error:')[-1][:60]] += 1
                    break
            if first is None: ok += 1; print('ok   ', name)
            else: print('FAIL ', name, '|', first.split('error:')[-1].strip()[:110])
    print('meter: %d из %d драйверов компилируются целиком' % (ok, tot))
    for k, v in reasons.most_common(25): print('%5d  %s' % (v, k))
    return 0 if ok == tot else 1

if __name__ == '__main__': sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]))
