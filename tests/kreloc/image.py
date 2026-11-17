#!/usr/bin/env python3
"""image.py elf выход.bin: файловая часть образа от начала первого PT_LOAD до начала секции .kreloc (между сегментами - нули)."""
import struct, sys

d = open(sys.argv[1], "rb").read()
phoff, = struct.unpack_from("<Q", d, 32)
phs, phn = struct.unpack_from("<HH", d, 54)
shoff, = struct.unpack_from("<Q", d, 40)
shs, shn, shstr = struct.unpack_from("<HHH", d, 58)
secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shs) for i in range(shn)]
stroff = secs[shstr][4]
kr = [s for s in secs if d[stroff + s[0]:d.index(b"\0", stroff + s[0])] == b".kreloc"][0][3]
loads = [struct.unpack_from("<IIQQQQQQ", d, phoff + i * phs) for i in range(phn)]
loads = [p for p in loads if p[0] == 1]
base = min(p[3] for p in loads)
out = bytearray(kr - base)
for t, fl, off, va, pa, fsz, msz, al in loads:
    for k in range(fsz):
        if 0 <= va - base + k < len(out):
            out[va - base + k] = d[off + k]
open(sys.argv[2], "wb").write(out)
