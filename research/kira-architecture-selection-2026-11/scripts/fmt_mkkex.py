#!/usr/bin/env python3 -I
"""fmt_mkkex.py in.elf out.kex [--synth-relocs N]: ELF64 ET_EXEC x86-64 -> KEX1 (прототип, см. fmt_kex.h). Сегменты PT_LOAD упаковываются подряд.
--synth-relocs N добавляет N СИНТЕТИЧЕСКИХ перемещений RELATIVE (только для замера стоимости разбора таблицы; реальных в x86-64 ET_EXEC нет)."""
import sys, struct, hashlib
def convert(elf, synth=0):
    assert elf[:4] == b'\x7fELF' and elf[4] == 2
    phoff, = struct.unpack_from('<Q', elf, 32); phnum, = struct.unpack_from('<H', elf, 56); entry, = struct.unpack_from('<Q', elf, 24)
    segs = []
    for i in range(phnum):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', elf, phoff + 56 * i)
        if t != 1 or msz == 0: continue
        segs.append((va, elf[off:off + fsz], msz, (1 if fl & 1 else 0) | (2 if fl & 2 else 0)))
    nseg = len(segs); rels = []
    if synth:
        # цели: слова внутри файловой части сегмента 0 (по 8 байт подряд, циклически)
        span = len(segs[0][1]) // 8
        rels = [struct.pack('<II', (8 * (i % span)), 1) for i in range(synth)]
    tab = KEX_HDR = 96
    off = KEX_HDR + 24 * nseg + 8 * len(rels)
    st = b''; data = b''
    for va, d, msz, fl in segs:
        st += struct.pack('<QIIII', va, off + len(data), len(d), msz, fl); data += d
    total = off + len(data)
    hdr = struct.pack('<IHHIHHQIIII', 0x3158454B, 1, 96, total, 62, nseg, entry, 96, len(rels), 96 + 24 * nseg, 0).ljust(64, b'\0') + bytes(32)
    body = hdr + st + b''.join(rels) + data
    h = hashlib.sha256(body).digest()
    return body[:64] + h + body[96:]
if __name__ == '__main__':
    a = sys.argv; n = int(a[a.index('--synth-relocs') + 1]) if '--synth-relocs' in a else 0
    open(a[2], 'wb').write(convert(open(a[1], 'rb').read(), n))
