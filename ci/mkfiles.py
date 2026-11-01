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

def gif_pal(n): return [((i * 37) & 255) << 16 | ((i * 91 + 13) & 255) << 8 | ((255 - i * 5) & 255) for i in range(n)]
def gif_idx(w, h, n): return [((x * 7 + y * 13) ^ (x * y // 3)) % n for y in range(h) for x in range(w)]
def gif_sum(w, h, n): pal = gif_pal(n); return sum(pal[i] for i in gif_idx(w, h, n)) & 0xFFFFFFFF

def lzw(idx, mcs):
    """LZW как у GIF: коды младшими битами вперёд, размер растёт до 12 бит, таблица полна - код очистки."""
    clear, eoi = 1 << mcs, (1 << mcs) + 1
    out = bytearray(); acc = nb = 0
    def put(c, size):
        nonlocal acc, nb
        acc |= c << nb; nb += size
        while nb >= 8: out.append(acc & 255); acc >>= 8; nb -= 8
    def reset(): return {bytes([i]): i for i in range(clear)}, eoi + 1, mcs + 1
    d, nxt, size = reset(); put(clear, size)
    w = b""
    for k in idx:
        wk = w + bytes([k])
        if wk in d: w = wk; continue
        put(d[w], size)
        if nxt < 4096:
            d[wk] = nxt; nxt += 1
            if nxt > (1 << size) and size < 12: size += 1
        else:
            put(clear, size); d, nxt, size = reset()
        w = bytes([k])
    if w: put(d[w], size)
    put(eoi, size)
    if nb: out.append(acc & 255)
    return bytes(out)

def gif(w, h, n, interlace=False, local=False):
    pal = gif_pal(n); idx = gif_idx(w, h, n); bits = max(1, (n - 1).bit_length()); size = 1 << bits
    if interlace:
        order = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
        idx = [idx[y * w + x] for y in order for x in range(w)]
    ct = b"".join(struct.pack("BBB", c >> 16, (c >> 8) & 255, c & 255) for c in pal) + bytes(3 * (size - n))
    data = lzw(idx, max(2, bits))
    blocks = b"".join(bytes([len(data[i:i + 255])]) + data[i:i + 255] for i in range(0, len(data), 255)) + b"\0"
    gflag = 0 if local else 0x80 | 0x70 | (bits - 1)
    head = b"GIF89a" + struct.pack("<HHBBB", w, h, gflag, 0, 0) + (b"" if local else ct)
    gce = b"\x21\xf9\x04\x00\x00\x00\x00\x00"                     # управление кадром без прозрачности
    desc = b"\x2c" + struct.pack("<HHHHB", 0, 0, w, h, (0x80 | (bits - 1) if local else 0) | (0x40 if interlace else 0)) + (ct if local else b"")
    return head + gce + desc + bytes([max(2, bits)]) + blocks + b"\x3b"

def make(out):
    global raw_rows
    os.makedirs(out, exist_ok=True)
    files = {}
    raw_rows = []; files["/pic.png"] = png(97, 61, 2)              # RGB, нечётный размер, все фильтры, dynamic Huffman
    raw_rows = []; files["/rgba.png"] = png(40, 30, 6, 0)          # RGBA, zlib без сжатия (блоки stored)
    files["/pic.bmp"] = bmp(33, 21)
    files["/pic.ppm"] = ppm(17, 9)
    files["/pic.gif"] = gif(97, 61, 256)                           # общая палитра, таблица LZW заполняется - коды очистки
    files["/lace.gif"] = gif(37, 29, 13, interlace=True, local=True)   # своя палитра кадра (16), чересстрочный
    files["/note.txt"] = "Привет, Кира!\nline 2\n".encode()
    files["/blob.bin"] = bytes(range(256)) * 2
    paths = {}
    for name, data in files.items():
        p = os.path.join(out, name.strip("/")); open(p, "wb").write(data); paths[name] = p
    try:                                                            # JPEG кодирует Pillow (libjpeg): если он есть
        import mkjpeg
        paths.update(mkjpeg.make(out))
    except ImportError:
        pass
    return paths

SUMS = {"/pic.png": checksum(97, 61), "/rgba.png": checksum(40, 30), "/pic.bmp": checksum(33, 21), "/pic.ppm": checksum(17, 9),
        "/pic.gif": gif_sum(97, 61, 256), "/lace.gif": gif_sum(37, 29, 13)}
raw_rows = []

if __name__ == "__main__":
    import sys
    print(make(sys.argv[1] if len(sys.argv) > 1 else "ci-files"))
    print({k: "%08x" % v for k, v in SUMS.items()})
