#!/usr/bin/env python3
"""Тестовые файлы для программы просмотра и их эталонные суммы пикселей (сумма 0x00RRGGBB по модулю 2^32)."""
import struct, zlib, os

def img(w, h):
    return [((x * 255 // (w - 1)) << 16) | ((y * 255 // (h - 1)) << 8) | ((x * y) & 255) for y in range(h) for x in range(w)]

def png(w, h, ct, level=9):
    pix = img(w, h); raw = b""
    for y in range(h):
        row = b""
        for x in range(w):
            c = pix[y * w + x]; r, g, b = c >> 16, (c >> 8) & 255, c & 255
            row += bytes([r, g, b]) if ct == 2 else bytes([r, g, b, 255])
        f = y % 5                                                   # все пять фильтров - кодировщик тоже наш
        bpp = 3 if ct == 2 else 4
        prev = raw_rows[-1] if y else bytes(len(row))
        out = bytearray()
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 0: v = row[i]
            elif f == 1: v = row[i] - a
            elif f == 2: v = row[i] - b
            elif f == 3: v = row[i] - ((a + b) >> 1)
            else:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                v = row[i] - (a if pa <= pb and pa <= pc else b if pb <= pc else c)
            out.append(v & 255)
        raw_rows.append(row); raw += bytes([f]) + bytes(out)
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, level)) + chunk(b"IEND", b""))

def bmp(w, h):
    pix = img(w, h); stride = (w * 3 + 3) & ~3; data = b""
    for y in range(h - 1, -1, -1):
        row = b"".join(bytes([pix[y * w + x] & 255, (pix[y * w + x] >> 8) & 255, pix[y * w + x] >> 16]) for x in range(w))
        data += row + bytes(stride - len(row))
    hdr = b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    return hdr + data

def ppm(w, h):
    pix = img(w, h)
    return b"P6\n# kira\n%d %d\n255\n" % (w, h) + b"".join(bytes([c >> 16, (c >> 8) & 255, c & 255]) for c in pix)

def checksum(w, h): return sum(img(w, h)) & 0xFFFFFFFF

def make(out):
    global raw_rows
    os.makedirs(out, exist_ok=True)
    files = {}
    raw_rows = []; files["/pic.png"] = png(97, 61, 2)              # RGB, нечётный размер, все фильтры, dynamic Huffman
    raw_rows = []; files["/rgba.png"] = png(40, 30, 6, 0)          # RGBA, zlib без сжатия (блоки stored)
    files["/pic.bmp"] = bmp(33, 21)
    files["/pic.ppm"] = ppm(17, 9)
    files["/note.txt"] = "Привет, Кира!\nline 2\n".encode()
    files["/blob.bin"] = bytes(range(256)) * 2
    paths = {}
    for name, data in files.items():
        p = os.path.join(out, name.strip("/")); open(p, "wb").write(data); paths[name] = p
    return paths

SUMS = {"/pic.png": checksum(97, 61), "/rgba.png": checksum(40, 30), "/pic.bmp": checksum(33, 21), "/pic.ppm": checksum(17, 9)}
raw_rows = []

if __name__ == "__main__":
    import sys
    print(make(sys.argv[1] if len(sys.argv) > 1 else "ci-files"))
    print({k: "%08x" % v for k, v in SUMS.items()})
