#!/usr/bin/env python3
"""mkkreloc.py - таблица перемещений образа ядра для KASLR (x86-64).

  mkkreloc.py build  kira.reloc.elf kreloc.bin   таблица из ELF, слинкованного с --emit-relocs
  mkkreloc.py check  kira.reloc.elf kreloc.bin   таблица в файле совпадает с тем, что требует этот ELF
  mkkreloc.py same   a.elf b.elf                 загружаемое содержимое двух ELF одинаково (кроме секции .kreloc)

Ядро - ET_EXEC по адресу 1 МБ (так его грузят GRUB, BIOS-загрузчик и UEFI-загрузчик); при загрузке оно копирует себя
по случайному адресу и прибавляет сдвиг ко всем словам, где записан абсолютный адрес образа. Такие слова компоновщик
помечает перемещениями R_X86_64_64 (8 байт) и R_X86_64_32 / R_X86_64_32S (4 байта; образ ниже 2 ГБ, поэтому
расширение знаком не мешает); остальные виды (PC32, PLT32) - относительные внутри образа и от сдвига не зависят.
Перемещения против абсолютных символов (SHN_ABS: смещения внутри трамплина AP и т.п.) и слабых неопределённых
(их значение 0, не адрес образа) от сдвига не зависят и пропускаются. Формат таблицы - kaslr.h (kreloc_hdr_t)."""
import struct, sys

MAGIC, VERSION = 0x434C524B, 1
R64, R32, R32S = 1, 10, 11
SHN_ABS, SHN_UNDEF = 0xFFF1, 0
SHF_ALLOC = 2
STB_WEAK = 2
SHT_RELA, SHT_NOBITS, SHT_SYMTAB = 4, 8, 2


def die(msg):
    sys.exit("mkkreloc: " + msg)


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            die(path + ": не ELF64 little-endian")
        (self.type, self.machine, _v, self.entry, self.phoff, self.shoff, _f, _eh, self.phentsize, self.phnum,
         self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        if self.type != 2 or self.machine != 62:
            die(path + ": нужен ET_EXEC x86-64 (ядро линкуется по фиксированному адресу)")
        self.sec = []
        for i in range(self.shnum):
            n, t, fl, addr, off, sz, link, info, al, es = struct.unpack_from("<IIQQQQIIQQ", d, self.shoff + i * self.shentsize)
            self.sec.append(dict(name=n, type=t, flags=fl, addr=addr, off=off, size=sz, link=link, info=info, entsize=es))
        self.loads = []
        for i in range(self.phnum):
            t, fl, off, va, pa, fsz, msz, al = struct.unpack_from("<IIQQQQQQ", d, self.phoff + i * self.phentsize)
            if t == 1:
                self.loads.append(dict(off=off, va=va, fsz=fsz, msz=msz))
        if not self.loads:
            die(path + ": нет PT_LOAD")
        self.base = min(p["va"] for p in self.loads)

    def sym(self, symtab, idx):
        s = self.sec[symtab]
        o = s["off"] + idx * 24
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.d, o)
        return shndx, value, info >> 4

    def file_extent(self):
        """(va_lo, va_hi) части образа, лежащей в файле: от базы до конца последнего PT_LOAD по filesz."""
        return self.base, max(p["va"] + p["fsz"] for p in self.loads if p["fsz"])

    def secname(self, s):
        so = self.sec[self.shstrndx]["off"]
        o = so + s["name"]
        return self.d[o:self.d.index(b"\0", o)].decode()


def collect(e):
    """Смещения слов, которые надо исправить: (список 64-битных, список 32-битных), от начала образа."""
    r64, r32 = set(), set()
    lo, hi = e.file_extent()
    for s in e.sec:
        if s["type"] != SHT_RELA:
            continue
        tgt = e.sec[s["info"]]
        if not (tgt["flags"] & SHF_ALLOC):
            continue                                     # отладочные секции
        if tgt["type"] == SHT_NOBITS:
            die("перемещение внутри bss (%s)" % e.secname(tgt))
        n = s["size"] // 24
        for i in range(n):
            r_off, r_info, r_add = struct.unpack_from("<QQq", e.d, s["off"] + i * 24)
            typ, symi = r_info & 0xFFFFFFFF, r_info >> 32
            if typ not in (R64, R32, R32S):
                continue                                 # PC32/PLT32/... - относительные, от сдвига не зависят
            if symi == 0:
                die("перемещение без символа в %s+%#x: адрес не определить" % (e.secname(tgt), r_off - tgt["addr"]))
            shndx, _, bind = e.sym(s["link"], symi)
            if shndx == SHN_ABS:
                continue
            if shndx == SHN_UNDEF and bind == STB_WEAK:
                continue                                 # слабый неопределённый символ (например platform_dma_pool): значение 0, не адрес образа
            if shndx == SHN_UNDEF:
                die("неопределённый символ в перемещении (%s+%#x)" % (e.secname(tgt), r_off - tgt["addr"]))
            width = 8 if typ == R64 else 4
            if r_off < lo or r_off + width > hi:
                die("перемещение вне загружаемой части образа: %#x" % r_off)
            (r64 if typ == R64 else r32).add(r_off - e.base)
    return sorted(r64), sorted(r32), hi - lo


def kreloc_section_range(e):
    for s in e.sec:
        if e.secname(s) == ".kreloc":
            return s["addr"], s["addr"] + s["size"]
    return None


def table(e):
    r64, r32, loaded = collect(e)
    # Копируется и исправляется всё, что лежит в файле, кроме самой таблицы: она стоит в конце файловой части.
    kr = kreloc_section_range(e)
    if kr:
        if kr[1] - e.base != loaded:
            die(".kreloc должна быть последней в файловой части образа")
        loaded = kr[0] - e.base
        for off in r64 + r32:
            if off >= loaded:
                die("перемещение внутри самой таблицы")
    body = struct.pack("<5I", MAGIC, VERSION, len(r64), len(r32), loaded)
    body += struct.pack("<%dI" % len(r64), *r64) + struct.pack("<%dI" % len(r32), *r32)
    return body, len(r64), len(r32), loaded


def image_bytes(e, skip):
    """Содержимое PT_LOAD по виртуальным адресам, без диапазона skip (va_lo, va_hi)."""
    out = {}
    for p in e.loads:
        data = e.d[p["off"]:p["off"] + p["fsz"]]
        if skip:
            a = max(skip[0], p["va"]) - p["va"]
            b = min(skip[1], p["va"] + p["fsz"]) - p["va"]
            if a < b:
                data = data[:a] + bytes(b - a) + data[b:]
        out[p["va"]] = (p["msz"], data)
    return out


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("build", "check", "same"):
        sys.exit(__doc__)
    cmd = sys.argv[1]
    e = Elf(sys.argv[2])
    if cmd == "same":
        f = Elf(sys.argv[3])
        if image_bytes(e, kreloc_section_range(e)) != image_bytes(f, kreloc_section_range(f)):
            die("образы различаются: итоговая линковка не совпала с проверенной")
        print("mkkreloc: образы совпадают")
        return
    body, n64, n32, loaded = table(e)
    if cmd == "build":
        open(sys.argv[3], "wb").write(body)
        print("mkkreloc: %d 64-битных и %d 32-битных перемещений, загружаемая часть %d байт, таблица %d байт" %
              (n64, n32, loaded, len(body)))
    else:
        have = open(sys.argv[3], "rb").read()
        if have != body:
            die("таблица не совпала: перемещения образа изменились между проходами линковки")
        print("mkkreloc: таблица верна (%d + %d перемещений)" % (n64, n32))


if __name__ == "__main__":
    main()
