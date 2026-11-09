#!/usr/bin/env python3
"""Рецепт drvget для драйвера из индекса BSD (tools/bsd_index.py): файлы корпуса OpenBSD по закреплённому коммиту с SHA-256, сгенерированные
kdmain.c (единая единица трансляции kcc: #pragma kcc unity) и kd_pcidevs.h (только упомянутые идентификаторы).
  bsd_recipe.py SYS_DIR INDEX PCIDEVS ИМЯ_ДРАЙВЕРА КОММИТ ВЫХОД_КАТАЛОГ [--corpus-base URL]
Выход: ВЫХОД/ИМЯ/{drvget.rec,kdmain.c,kd_pcidevs.h}; строки каталога (pci VVVV:DDDD имя url sha256 рецепта) - на стандартный вывод
(url берётся из --recipe-url-base, по умолчанию заглушка, которую подставляет сборщик каталога)."""
import hashlib, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsd_pcidevs as P
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM = os.path.join(ROOT, 'kcc/include/bsd')

# драйверы с DMA, проверенные на слое (сценарии drv-bsd-*): права. fxp - без irq (с INTx приём застревает, docs/BSD_COMPAT.md)
DMA_OK = {'fxp_pci': 'mmio pcidev netdev dma', 'pcn': 'mmio pcidev netdev irq dma'}

# Формат пакета драйвера (kdrv_header_t) вмещает KDRV_MATCH_MAX = 8 идентификаторов. Если у драйвера их больше, заявляются первые 8 (проверенные идут вперёд),
# в каталог тоже попадают только они: остальные устройства драйвер не получит, пока формат не расширен (docs/BSD_COMPAT.md).
MATCH_MAX = 8
PREFER = {'fxp_pci': ['8086:1209'], 'pcn': ['1022:2000']}

def sha(b): return hashlib.sha256(b).hexdigest()
def rd(p): return open(p, 'rb').read()

def headers(sysd, files):
    """заголовки корпуса, нужные файлам (кроме тех, что даёт слой совместимости): транзитивно"""
    out, todo = [], list(files)
    while todo:
        cur = todo.pop()
        for m in re.findall(r'#\s*include\s*[<"]([A-Za-z0-9_./]+\.h)[>"]', rd(os.path.join(sysd, cur)).decode('latin-1')):
            if os.path.exists(os.path.join(SHIM, m)): continue
            for cand in (m, os.path.join(os.path.dirname(cur), m)):
                if os.path.exists(os.path.join(sysd, cand)) and cand not in out and cand not in files:
                    out.append(cand); todo.append(cand); break
    return out

def devname(ca):
    n = re.sub(r'_ca$', '', ca)
    return re.sub(r'_pci$', '', n)

def main(sysd, index, pcidevs, name, commit, outd, base):
    ent = None
    for ln in open(index):
        p = ln.split()
        if p and p[0] == 'drv' and p[1] == name: ent = p
    if not ent: sys.exit('нет драйвера %s в индексе' % name)
    _, _, ca, pci, ic, ids, flags = ent
    caps = DMA_OK.get(name, 'mmio pcidev netdev irq')
    if 'dma' in flags.split(',') and name not in DMA_OK: sys.exit('%s: драйвер использует bus_dma, но не проверен на слое DMA (docs/BSD_COMPAT.md)' % name)
    cfiles = [pci] + ([] if ic == '-' else ic.split(','))
    hfiles = headers(sysd, cfiles)
    allf = cfiles + hfiles
    bn = [os.path.basename(f) for f in allf]
    if len(set(bn)) != len(bn): sys.exit('совпадают имена файлов без каталогов: %s' % bn)
    idl = ids.split(',')
    idl = [i for i in PREFER.get(name, []) if i in idl] + [i for i in idl if i not in PREFER.get(name, [])]
    if len(idl) > MATCH_MAX:
        print('%s: идентификаторов %d, заявлено %d (формат пакета), остальные не заявлены' % (name, len(idl), MATCH_MAX), file=sys.stderr)
        idl = idl[:MATCH_MAX]
    ids = ','.join(idl)
    dn = devname(ca)
    d = os.path.join(outd, dn); os.makedirs(d, exist_ok=True)
    vend, prod = P.parse(pcidevs)
    syms, bad = P.resolve(vend, prod, P.referenced([os.path.join(sysd, f) for f in allf]))
    if bad: sys.exit('неизвестные имена pcidevs: %s' % ' '.join(bad))
    pd = ('/* SPDX-License-Identifier: BSD-2-Clause */\n/* Сгенерировано tools/bsd_recipe.py из pcidevs OpenBSD: только то, что упомянуто в исходниках драйвера. */\n' +
          ''.join('#define %s 0x%04x\n' % kv for kv in syms.items())).encode()
    inc = ''.join('#include "%s"\n' % os.path.basename(f) for f in cfiles)
    km = ('/* SPDX-License-Identifier: BSD-2-Clause */\n/* Драйвер BSD %s: единая единица трансляции для kcc (tools/bsd_recipe.py). Файлы OpenBSD - по хешам рецепта. */\n'
          '#pragma kcc unity\n#define _KERNEL 1\n#include <bsd_core.h>\n%s'
          'extern const struct cfattach %s;\nconst struct cfattach* kd_ca = &%s;\nconst char* kd_devname = "%s";\n#include <bsd_rt.c>\n') % (name, inc, ca, ca, dn)
    km = km.encode()
    open(os.path.join(d, 'kd_pcidevs.h'), 'wb').write(pd); open(os.path.join(d, 'kdmain.c'), 'wb').write(km)
    rec = ['# Рецепт drvget для драйвера OpenBSD %s (создан tools/bsd_recipe.py, корпус - коммит %s)' % (name, commit[:10]), 'drvget-recipe 1', 'name ' + dn, 'version 1',
           'license BSD-3-Clause']
    rec += ['pci ' + i for i in ids.split(',')]
    rec += ['caps ' + caps, 'main kdmain.c', 'file kdmain.c kdmain.c sha256 ' + sha(km), 'file kd_pcidevs.h kd_pcidevs.h sha256 ' + sha(pd)]
    for f in allf: rec.append('file %s %s/%s sha256 %s' % (os.path.basename(f), base, f, sha(rd(os.path.join(sysd, f)))))
    txt = ('\n'.join(rec) + '\n').encode()
    open(os.path.join(d, 'drvget.rec'), 'wb').write(txt)
    print('recipe', dn, sha(txt), len(rec), file=sys.stderr)
    for i in ids.split(','): print('pci %s %s @URL@ %s' % (i, dn, sha(txt)))

if __name__ == '__main__':
    a = sys.argv[1:]; base = 'https://raw.githubusercontent.com/openbsd/src/%s/sys' % a[4]
    if '--corpus-base' in a: base = a[a.index('--corpus-base') + 1]
    main(a[0], a[1], a[2], a[3], a[4], a[5], base)
