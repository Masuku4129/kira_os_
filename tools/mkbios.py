#!/usr/bin/env python3
"""mkbios.py stage1.bin stage2.bin kernel.elf out.img [cd] [initrd.img] - BIOS disk image."""
import os, sys, zlib, struct

KERNEL_LBA = 64
S2_SECTORS = 32
MAX_INITRD = 64 * 1024 * 1024

def main():
    if len(sys.argv) < 5 or len(sys.argv) > 7:
        sys.exit(__doc__)
    s1 = open(sys.argv[1], "rb").read()
    s2 = open(sys.argv[2], "rb").read()
    kernel = open(sys.argv[3], "rb").read()
    out = sys.argv[4]
    extras = sys.argv[5:]
    cd = "cd" in extras
    module_paths = [arg for arg in extras if arg != "cd"]
    if len(module_paths) > 1:
        sys.exit("mkbios: only one initrd module is supported")
    initrd_path = module_paths[0] if module_paths else None
    initrd = open(initrd_path, "rb").read() if initrd_path else b""

    if cd:
        if len(s1) != 1024:
            sys.exit("stage1cd: нужно ровно 1024 байта")
    elif len(s1) != 512 or s1[510:] != b"\x55\xaa":
        sys.exit("stage1: нужен ровно 1 сектор с подписью 55AA")
    if len(s2) != S2_SECTORS * 512:
        sys.exit("stage2: нужно ровно %d байт" % (S2_SECTORS * 512))
    if s2[-160:] != bytes(160):
        sys.exit("stage2: манифест должен быть пустым")
    if kernel[:4] != b"\x7fELF" or kernel[4] != 2:
        sys.exit("ядро: не ELF64")
    if initrd_path and (not initrd or len(initrd) > MAX_INITRD):
        sys.exit("initrd: размер должен быть от 1 байта до 64 МиБ")

    cmd = os.environ.get("KIRA_CMDLINE", "").encode("utf-8")
    if len(cmd) > 126 or b"\0" in cmd:
        sys.exit("KIRA_CMDLINE: не больше 126 байт, без нулевых байтов")
    s2 = s2[:-160] + cmd + bytes(128 - len(cmd)) + s2[-32:]

    kernel_crc = zlib.crc32(kernel) & 0xffffffff
    image = bytearray(s1 + s2)
    image += bytes(KERNEL_LBA * 512 - len(image))
    image += kernel
    # Module starts on a 2048-byte boundary: both EDD disks and El Torito
    # no-emulation CD reads use the same 512-byte LBA interface in stage2.
    image += bytes((-len(image)) % 2048)

    module_lba = module_size = module_crc = module_flags = 0
    if initrd_path:
        module_lba = len(image) // 512
        module_size = len(initrd)
        module_crc = zlib.crc32(initrd) & 0xffffffff
        module_flags = 1
        image += initrd
        image += bytes((-len(image)) % 512)

    manifest = (
        b"KBM1"
        + struct.pack("<III", KERNEL_LBA, len(kernel), kernel_crc)
        + struct.pack("<IIII", module_lba, module_size, module_crc, module_flags)
    )
    s2_offset = len(s1)
    image[s2_offset + S2_SECTORS * 512 - 32:
          s2_offset + S2_SECTORS * 512] = manifest
    image += bytes((-len(image)) % (1 << 20))
    with open(out, "wb") as f:
        f.write(image)
    print("mkbios: %s, ядро %d байт, crc32 %08x%s" %
          (out, len(kernel), kernel_crc,
           ", initrd %d байт crc32 %08x" % (len(initrd), module_crc)
           if initrd_path else ""))

main()
