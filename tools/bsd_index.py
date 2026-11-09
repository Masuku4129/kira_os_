#!/usr/bin/env python3
"""Индекс драйверов PCI-сетевых карт OpenBSD: какие файлы нужны драйверу и каким PCI-идентификаторам он отвечает.
  bsd_index.py SYS_DIR PCIDEVS > index.txt     (SYS_DIR - каталог sys/ закреплённого коммита; нужны dev/pci, dev/ic, dev/mii)
Строка индекса (поля через пробел, списки через запятую):
  drv ИМЯ CFATTACH ФАЙЛ_PCI ФАЙЛЫ_IC ID_VVVV:PPPP,... ФЛАГИ        ФЛАГИ: mii - нужны драйверы PHY, dma - bus_dma, none - нет
Только драйверы, где есть ether_ifattach (сетевая карта) и хотя бы один PCI-идентификатор из pcidevs."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsd_pcidevs as P

def rd(p):
    try: return open(p, encoding='latin-1', errors='replace').read()
    except OSError: return ''

def closure(sysd, f, seen):
    """dev/ic/*.c, нужные файлу: через #include <dev/ic/X.h> -> X.c (имя без суффикса var; заголовки reg - только регистры, исходник не тянут), транзитивно по заголовкам и исходникам"""
    out = []; todo = [f]; hseen = set()
    while todo:
        cur = todo.pop()
        for inc in re.findall(r'#\s*include\s*<(dev/ic/[A-Za-z0-9_]+)\.h>', rd(os.path.join(sysd, cur))):
            if inc in hseen: continue
            hseen.add(inc)
            if os.path.exists(os.path.join(sysd, inc + '.h')): todo.append(inc + '.h')
            base = inc
            for cand in (base, re.sub(r'var$', '', base)) if not base.endswith('reg') else ():
                c = cand + '.c'
                if os.path.exists(os.path.join(sysd, c)):
                    if c not in seen: seen.add(c); out.append(c); todo.append(c)
                    break
    return out

def main(sysd, pcidevs):
    vend, prod = P.parse(pcidevs)
    byname = {}
    for (v, q), pid in prod.items(): byname['PCI_PRODUCT_%s_%s' % (v, q)] = (vend[v], pid)
    pcid = os.path.join(sysd, 'dev/pci'); n = 0
    for fn in sorted(os.listdir(pcid)):
        if not fn.endswith('.c'): continue
        rel = 'dev/pci/' + fn; txt = rd(os.path.join(sysd, rel))
        cas = re.findall(r'struct\s+cfattach\s+([A-Za-z0-9_]+)\s*=', txt)
        if not cas: continue
        ic = closure(sysd, rel, set())
        alltxt = txt + ''.join(rd(os.path.join(sysd, f)) for f in ic)
        if 'ether_ifattach' not in alltxt: continue
        ids = sorted({'%04x:%04x' % byname[s] for s in set(re.findall(r'PCI_PRODUCT_[A-Za-z0-9_]+', txt)) if s in byname})
        if not ids: continue
        flags = [f for f, pat in (('mii', r'mii_attach|miibus|mii_pollstat'), ('dma', r'bus_dmamap|bus_dma_tag')) if re.search(pat, alltxt)]
        print('drv %s %s %s %s %s %s' % (fn[:-2].replace('if_', '', 1), cas[0], rel, ','.join(ic) or '-', ','.join(ids), ','.join(flags) or 'none')); n += 1
    print('# драйверов: %d' % n, file=sys.stderr)

if __name__ == '__main__': main(sys.argv[1], sys.argv[2])
