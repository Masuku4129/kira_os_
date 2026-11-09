#!/usr/bin/env python3
"""pcidevs OpenBSD -> pcidevs.h для слоя совместимости BSD и поиск драйвера по PCI-идентификатору.
  bsd_pcidevs.py gen PCIDEVS ВЫХОД.h ИСХОДНИК...   только PCI_VENDOR_*/PCI_PRODUCT_*, упомянутые в исходниках (неизвестное имя - ошибка)
  bsd_pcidevs.py ids PCIDEVS ИСХОДНИК...           строки "vendor product символ" для упомянутых пар (таблица поиска драйвера)
  bsd_pcidevs.py find PCIDEVS VVVV:PPPP            символы устройства по идентификатору"""
import re, sys

def parse(path):
    vend, prod = {}, {}
    for ln in open(path, encoding='latin-1'):
        p = ln.split()
        if len(p) >= 3 and p[0] == 'vendor': vend[p[1]] = int(p[2], 16)
        elif len(p) >= 4 and p[0] == 'product': prod[(p[1], p[2])] = int(p[3], 16)
    return vend, prod

def referenced(srcs):
    names = set()
    for s in srcs: names |= set(re.findall(r'\bPCI_(?:VENDOR|PRODUCT)_[A-Za-z0-9_]+', open(s, encoding='latin-1', errors='replace').read()))
    return names

def resolve(vend, prod, names):
    out, bad = {}, []
    for n in sorted(names):
        if n.startswith('PCI_VENDOR_'):
            k = n[11:]
            if k in vend: out[n] = vend[k]
            else: bad.append(n)
        else:
            k = n[12:]; hit = [(v, q) for (v, q) in prod if k.startswith(v + '_') and k[len(v) + 1:] == q]
            if hit: out[n] = prod[hit[0]]
            else: bad.append(n)
    return out, bad

if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) < 3: print(__doc__); sys.exit(2)
    vend, prod = parse(a[1])
    if a[0] == 'gen':
        out, bad = resolve(vend, prod, referenced(a[3:]))
        if bad: print('неизвестные имена pcidevs:', ' '.join(bad), file=sys.stderr); sys.exit(1)
        open(a[2], 'w').write('/* Сгенерировано tools/bsd_pcidevs.py из pcidevs OpenBSD: только то, что упомянуто в исходниках. */\n#ifndef KIRA_PCIDEVS_GEN_H\n#define KIRA_PCIDEVS_GEN_H\n' +
                              ''.join('#define %s 0x%04x\n' % (k, v) for k, v in out.items()) + '#endif\n')
    elif a[0] == 'ids':
        names = referenced(a[2:]); out, bad = resolve(vend, prod, names)
        for (v, q), pid in sorted(prod.items(), key=lambda x: (x[0][0], x[1])):
            sym = 'PCI_PRODUCT_%s_%s' % (v, q)
            if sym in out: print('0x%04x 0x%04x %s' % (vend[v], pid, sym))
    elif a[0] == 'find':
        vv, pp = [int(x, 16) for x in a[2].split(':')]
        for (v, q), pid in prod.items():
            if vend.get(v) == vv and pid == pp: print('PCI_VENDOR_%s PCI_PRODUCT_%s_%s' % (v, v, q))
