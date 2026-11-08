#!/usr/bin/env python3
"""Сверка двух ET_EXEC ELF64: точка входа, программные заголовки, содержимое PT_LOAD, секции (имя, адрес, размер, флаги, содержимое) и символы.
Использование: cmp_elf.py a.elf b.elf [--syms]   Код 0 - совпало, 1 - различия (печатаются), 2 - ошибка файла."""
import struct, sys

def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        raise SystemExit('%s: not ELF64' % path)
    et, em, ev, entry, phoff, shoff, fl, ehs, phes, phn, shes, shn, shstr = struct.unpack_from('<HHIQQQIHHHHHH', d, 16)
    ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + i * phes) for i in range(phn)]
    sh = []
    for i in range(shn):
        nm, ty, fg, ad, of, sz, lk, inf, al, es = struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shes)
        sh.append(dict(nm=nm, ty=ty, fg=fg, ad=ad, of=of, sz=sz, lk=lk, inf=inf, al=al, es=es))
    so = sh[shstr]['of']
    def nm(o):
        e = d.index(b'\0', so + o)
        return d[so + o:e].decode()
    for s in sh:
        s['name'] = nm(s['nm'])
    return d, entry, ph, sh

def syms(d, sh):
    st = [s for s in sh if s['ty'] == 2][0]
    strs = sh[st['lk']]
    out = []
    for i in range(st['sz'] // 24):
        n, info, oth, shn, val, size = struct.unpack_from('<IBBHQQ', d, st['of'] + i * 24)
        e = d.index(b'\0', strs['of'] + n)
        sec = sh[shn]['name'] if 0 < shn < len(sh) else shn      # по имени секции: у lld бывает лишняя пустая .got, индексы сдвигаются
        out.append((d[strs['of'] + n:e].decode(), info, str(sec), val, size))
    return out

def main():
    a, b = sys.argv[1], sys.argv[2]
    da, ea, pa, sa = load(a)
    db, eb, pb, sb = load(b)
    bad = []
    if ea != eb:
        bad.append('entry %#x != %#x' % (ea, eb))
    key = lambda p: (p[0], p[1], p[3], p[5], p[6], p[7])      # type flags vaddr filesz memsz align
    if [key(p) for p in pa] != [key(p) for p in pb]:
        bad.append('program headers differ:\n  %s\n  %s' % ([tuple(map(hex, key(p))) for p in pa], [tuple(map(hex, key(p))) for p in pb]))
    else:
        for x, y in zip(pa, pb):
            if x[0] == 1:
                ca, cb = da[x[2]:x[2] + x[5]], db[y[2]:y[2] + y[5]]
                if ca != cb:
                    k = next(i for i in range(len(ca)) if ca[i] != cb[i])
                    bad.append('PT_LOAD at %#x differs at +%#x' % (x[3], k))
    alloc = lambda sh: [s for s in sh if s['fg'] & 2 and s['sz']]      # пустые секции (.got у lld) не сравниваются
    la, lb = alloc(sa), alloc(sb)
    if [(s['name'], s['ad'], s['sz'], s['fg'] & ~0x30, s['al'], s['ty']) for s in la] != [(s['name'], s['ad'], s['sz'], s['fg'] & ~0x30, s['al'], s['ty']) for s in lb]:
        bad.append('allocatable sections differ:\n  %s\n  %s' % ([(s['name'], hex(s['ad']), hex(s['sz']), s['fg'], s['al'], s['ty']) for s in la], [(s['name'], hex(s['ad']), hex(s['sz']), s['fg'], s['al'], s['ty']) for s in lb]))
    if '--syms' in sys.argv:
        ya, yb = sorted(syms(da, sa)), sorted(syms(db, sb))
        if ya != yb:
            sa_, sb_ = set(ya), set(yb)
            bad.append('symbols differ: only in A %s, only in B %s' % (sorted(sa_ - sb_)[:5], sorted(sb_ - sa_)[:5]))
    if bad:
        print('\n'.join(bad))
        return 1
    return 0

sys.exit(main())
