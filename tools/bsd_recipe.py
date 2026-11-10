#!/usr/bin/env python3
"""Рецепт drvget для драйвера из индекса BSD (tools/bsd_index.py): файлы корпуса OpenBSD по закреплённому коммиту с SHA-256, сгенерированные
kdmain.c (единая единица трансляции kcc: #pragma kcc unity) и kd_pcidevs.h (только упомянутые идентификаторы).
  bsd_recipe.py SYS_DIR INDEX PCIDEVS ИМЯ_ДРАЙВЕРА КОММИТ ВЫХОД_КАТАЛОГ [--corpus-base URL]
Выход: ВЫХОД/ИМЯ/{drvget.rec,kdmain.c,kd_pcidevs.h}; строки каталога (pci VVVV:DDDD имя url sha256 рецепта) - на стандартный вывод
(url берётся из --recipe-url-base, по умолчанию шаблонный маркер @URL@, который заменяет сборщик каталога)."""
import hashlib, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsd_pcidevs as P
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM = os.path.join(ROOT, 'kcc/include/bsd')

# драйверы с DMA, проверенные на слое (сценарии drv-bsd-*): права. 
DMA_OK = {'fxp_pci': 'mmio pcidev netdev irq dma', 'pcn': 'mmio pcidev netdev irq dma'}

# Формат пакета драйвера (kdrv_header_t) вмещает KDRV_MATCH_MAX = 8 идентификаторов. Формат не меняется (подписанные пакеты и их векторы остаются годными):
# если идентификаторов больше, рецепт делится на части по 8 - ИМЯ, ИМЯ2, ИМЯ3... Исходники и права у частей одинаковые, различаются списком pci
# и именем пакета; проверенные идентификаторы идут в первую часть. Собирается только та часть, в которой нашлась карта.
MATCH_MAX = 8
PREFER = {'fxp_pci': ['8086:1209'], 'pcn': ['1022:2000']}

def classify_license(data, path="<source>"):
    """Return one supported SPDX identifier; unknown, compound and mixed notices fail closed."""
    text = data.decode("latin-1", errors="strict")
    spdx = re.findall(r"SPDX-License-Identifier:\s*([^\r\n*]+)", text)
    if spdx:
        identifiers = [item.strip() for item in spdx]
        if len(set(identifiers)) != 1 or len(identifiers) != 1 or identifiers[0] not in {"BSD-2-Clause", "BSD-3-Clause", "MIT", "ISC", "LicenseRef-Greenman"}:
            raise ValueError("%s: unsupported or compound SPDX license: %s" % (path, ", ".join(identifiers)))
        return identifiers[0]

    low = " ".join(text.lower().split())
    if "all advertising materials mentioning" in low:
        raise ValueError("%s: BSD advertising clause is outside the allowed license set" % path)
    if ("may be used, modified, copied, distributed, and sold, in both source and binary form" in low and
            "under no circumstances is the author responsible for the proper functioning" in low):
        return "LicenseRef-Greenman"
    if ("redistribution and use in source and binary forms" in low and
            "this software is provided by" in low and
            "1. redistributions of source code must retain" in low and
            "2. redistributions in binary form must reproduce" in low):
        if (("neither the name of" in low or "the name of " in low) and
                "endorse or promote" in low):
            return "BSD-3-Clause"
        return "BSD-2-Clause"
    if ("permission is hereby granted, free of charge, to any person obtaining a copy" in low and
            "the software is provided \"as is\"" in low):
        return "MIT"
    if ("permission to use, copy, modify, and/or distribute this software" in low and
            "the software is provided \"as is\"" in low):
        return "ISC"
    raise ValueError("%s: cannot identify a supported single-file license; refusing to label the recipe" % path)


def recipe_license(sysd, files):
    found = {}
    for path in files:
        lic = classify_license(rd(os.path.join(sysd, path)), path)
        found.setdefault(lic, []).append(path)
    if len(found) != 1:
        detail = "; ".join("%s: %s" % (lic, ", ".join(paths)) for lic, paths in sorted(found.items()))
        raise ValueError("driver sources have mixed licenses; recipe format supports one license only: " + detail)
    return next(iter(found))


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
    try:
        license_id = recipe_license(sysd, allf)
    except (OSError, UnicodeError, ValueError) as exc:
        sys.exit('%s: license gate: %s' % (name, exc))
    bn = [os.path.basename(f) for f in allf]
    if len(set(bn)) != len(bn): sys.exit('совпадают имена файлов без каталогов: %s' % bn)
    idl = ids.split(',')
    idl = [i for i in PREFER.get(name, []) if i in idl] + [i for i in idl if i not in PREFER.get(name, [])]
    dn = devname(ca)
    parts = [idl[i:i + MATCH_MAX] for i in range(0, len(idl), MATCH_MAX)]
    if len(parts) > 1: print('%s: идентификаторов %d, частей рецепта %d (по %d)' % (name, len(idl), len(parts), MATCH_MAX), file=sys.stderr)
    vend, prod = P.parse(pcidevs)
    syms, bad = P.resolve(vend, prod, P.referenced([os.path.join(sysd, f) for f in allf]))
    if bad: sys.exit('неизвестные имена pcidevs: %s' % ' '.join(bad))
    pd = ('/* SPDX-License-Identifier: %s */\n/* Сгенерировано tools/bsd_recipe.py из pcidevs OpenBSD: только то, что упомянуто в исходниках драйвера. */\n' % license_id +
          ''.join('#define %s 0x%04x\n' % kv for kv in syms.items())).encode()
    inc = ''.join('#include "%s"\n' % os.path.basename(f) for f in cfiles)
    km = ('/* SPDX-License-Identifier: %s */\n/* Драйвер BSD %s: единая единица трансляции для kcc (tools/bsd_recipe.py). Файлы OpenBSD - по хешам рецепта. */\n'
          '#pragma kcc unity\n#define _KERNEL 1\n#include <bsd_core.h>\n%s'
          'extern const struct cfattach %s;\nconst struct cfattach* kd_ca = &%s;\nconst char* kd_devname = "%s";\n#include <bsd_rt.c>\n') % (license_id, name, inc, ca, ca, dn)
    km = km.encode()
    for n, pids in enumerate(parts, 1):
        pn = dn if n == 1 else '%s%d' % (dn, n)
        d = os.path.join(outd, pn); os.makedirs(d, exist_ok=True)
        open(os.path.join(d, 'kd_pcidevs.h'), 'wb').write(pd); open(os.path.join(d, 'kdmain.c'), 'wb').write(km)
        rec = ['# Рецепт drvget для драйвера OpenBSD %s (создан tools/bsd_recipe.py, корпус - коммит %s)%s' % (name, commit[:10], '' if len(parts) == 1 else ', часть %d из %d' % (n, len(parts))),
               'drvget-recipe 1', 'name ' + pn, 'version 1', 'license ' + license_id]
        rec += ['pci ' + i for i in pids]
        rec += ['caps ' + caps, 'main kdmain.c', 'file kdmain.c kdmain.c sha256 ' + sha(km), 'file kd_pcidevs.h kd_pcidevs.h sha256 ' + sha(pd)]
        for f in allf: rec.append('file %s %s/%s sha256 %s' % (os.path.basename(f), base, f, sha(rd(os.path.join(sysd, f)))))
        txt = ('\n'.join(rec) + '\n').encode()
        open(os.path.join(d, 'drvget.rec'), 'wb').write(txt)
        print('recipe', pn, sha(txt), len(rec), file=sys.stderr)
        for i in pids: print('pci %s %s @URL@ %s' % (i, pn, sha(txt)))

if __name__ == '__main__':
    a = sys.argv[1:]; base = 'https://raw.githubusercontent.com/openbsd/src/%s/sys' % a[4]
    if '--corpus-base' in a: base = a[a.index('--corpus-base') + 1]
    main(a[0], a[1], a[2], a[3], a[4], a[5], base)
