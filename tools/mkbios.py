#!/usr/bin/env python3
"""mkbios.py stage1.bin stage2.bin kernel.elf out.img [cd] - диск для собственного BIOS-загрузчика KIRA.
LBA0 stage1, LBA1..32 stage2 (манифест в последних 32 байтах), LBA64.. ядро (ELF64)."""
import os, sys, zlib, struct
KERNEL_LBA = 64
S2_SECTORS = 32
def main():
    cd = len(sys.argv) == 6 and sys.argv[5] == "cd"      # образ для компакт-диска: stage1cd (1024 байта) + stage2, ядро с той же позиции
    if len(sys.argv) != 5 and not cd:
        sys.exit(__doc__)
    s1, s2, k, out = (open(sys.argv[i], 'rb').read() if i < 4 else None for i in range(1, 5))
    if cd:
        if len(s1) != 1024:
            sys.exit('stage1cd: нужно ровно 1024 байта')
    elif len(s1) != 512 or s1[510:] != b'\x55\xaa':
        sys.exit('stage1: нужен ровно 1 сектор с подписью 55AA')
    if len(s2) != S2_SECTORS * 512:
        sys.exit('stage2: нужно ровно %d байт' % (S2_SECTORS * 512))
    if s2[-160:] != bytes(160):
        sys.exit('stage2: манифест должен быть пустым')
    if k[:4] != b'\x7fELF' or k[4] != 2:
        sys.exit('ядро: не ELF64')
    cmd = os.environ.get('KIRA_CMDLINE', '').encode('utf-8')
    if len(cmd) > 126 or b'\0' in cmd:
        sys.exit('KIRA_CMDLINE: не больше 126 байт, без нулевых байтов')
    s2 = s2[:-160] + cmd + bytes(128 - len(cmd)) + s2[-32:]
    man = b'KBM1' + struct.pack('<III', KERNEL_LBA, len(k), zlib.crc32(k) & 0xFFFFFFFF) + bytes(16)
    s2 = s2[:-32] + man
    img = bytearray(s1 + s2)
    img += bytes(KERNEL_LBA * 512 - len(img))
    img += k
    img += bytes((-len(img)) % (1 << 20))
    open(sys.argv[4], 'wb').write(img)
    print('mkbios: %s, ядро %d байт, crc32 %08x' % (sys.argv[4], len(k), zlib.crc32(k) & 0xFFFFFFFF))
main()
