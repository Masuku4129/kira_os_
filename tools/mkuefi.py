#!/usr/bin/env python3
"""Собирает загрузочный UEFI-диск Киры: GPT с одним разделом ESP (FAT32), в нём \\EFI\\BOOT\\BOOTX64.EFI (наш загрузчик) и
\\EFI\\KIRA\\KIRA.ELF (ядро). GRUB не нужен. Использует mformat/mcopy (mtools) для FAT внутри образа по смещению.
Использование: mkuefi.py BOOTX64.EFI kira.bin kira-uefi.img [размер_МиБ] [initrd.img]"""
import os, struct, subprocess, sys, uuid, zlib

SECTOR = 512
ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")

def gpt_image(total_sectors, part_first, part_last, disk_guid, part_guid):
    entries = bytearray(128 * 128)
    name = "KIRA ESP".encode("utf-16le")
    e = ESP_TYPE.bytes_le + part_guid.bytes_le + struct.pack("<QQQ", part_first, part_last, 0) + name.ljust(72, b"\0")
    entries[:128] = e
    ecrc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
    def header(cur, other, first_usable, last_usable, entries_lba):
        h = bytearray(92)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x00010000, 92, 0, 0, cur, other, first_usable, last_usable,
                         disk_guid.bytes_le, entries_lba, 128, 128, ecrc)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b"\0")
    first_usable, last_usable = 34, total_sectors - 34
    primary = header(1, total_sectors - 1, first_usable, last_usable, 2)
    backup = header(total_sectors - 1, 1, first_usable, last_usable, total_sectors - 33)
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(total_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr), primary, bytes(entries), backup

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    efi, kernel, out = sys.argv[1:4]
    mib = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    initrd = sys.argv[5] if len(sys.argv) > 5 else None
    if initrd and (not os.path.isfile(initrd) or os.path.getsize(initrd) == 0):
        sys.exit(f"mkuefi: invalid initrd file: {initrd}")
    total = mib * 1024 * 1024 // SECTOR
    part_first = 2048
    part_last = total - 34 - 1
    disk_guid = uuid.UUID(int=zlib.crc32(open(kernel, "rb").read()) << 96 | 0x4B495241)   # детерминированно от ядра
    part_guid = uuid.UUID(int=(zlib.crc32(open(efi, "rb").read()) << 96) | 0x45535021)
    mbr, primary, entries, backup = gpt_image(total, part_first, part_last, disk_guid, part_guid)
    with open(out, "wb") as f:
        f.truncate(total * SECTOR)
        f.seek(0); f.write(mbr)
        f.seek(SECTOR); f.write(primary)
        f.seek(2 * SECTOR); f.write(entries)
        f.seek((total - 33) * SECTOR); f.write(entries)
        f.seek((total - 1) * SECTOR); f.write(backup)
    off = part_first * SECTOR
    img = f"{out}@@{off}"
    size_kib = (part_last - part_first + 1) * SECTOR // 1024
    def run(*a): subprocess.run(a, check=True, env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
    run("mformat", "-i", img, "-F", "-T", str((part_last - part_first + 1)), "-h", "64", "-s", "32", "::")
    run("mmd", "-i", img, "::/EFI", "::/EFI/BOOT", "::/EFI/KIRA")
    run("mcopy", "-i", img, efi, "::/EFI/BOOT/BOOTX64.EFI")
    run("mcopy", "-i", img, kernel, "::/EFI/KIRA/KIRA.ELF")
    if initrd:
        run("mcopy", "-i", img, initrd, "::/EFI/KIRA/INITRD.IMG")
    extra = f", initrd {os.path.getsize(initrd)} байт" if initrd else ""
    print(f"mkuefi: {out}, {mib} МиБ, GPT + ESP FAT32 ({size_kib} КиБ), ядро {os.path.getsize(kernel)} байт{extra}")

main()
