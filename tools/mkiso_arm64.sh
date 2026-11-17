#!/bin/bash
# ISO Киры для ARM64 с СОБСТВЕННЫМ UEFI-загрузчиком, без GRUB: tools/mkiso_arm64.sh BOOTAA64.EFI kira-arm64.img выход.iso
#   Вход El Torito (платформа 0xEF, UEFI): образ ESP FAT с \EFI\BOOT\BOOTAA64.EFI (запасной путь, который прошивка ищет сама)
#   и \EFI\KIRA\KIRA-ARM64.IMG (ядро, которое читает загрузчик). Те же файлы лежат и в самом ISO.
#   Аналог tools/mkiso_own.sh для x86-64.
set -e
cd "$(dirname "$0")/.."
EFI=$1; IMG=$2; OUT=$3
[ -f "$EFI" ] && [ -f "$IMG" ] && [ -n "$OUT" ] || { echo "использование: tools/mkiso_arm64.sh BOOTAA64.EFI kira-arm64.img выход.iso" >&2; exit 2; }
command -v mformat >/dev/null && command -v xorriso >/dev/null || { echo "нужны mtools и xorriso" >&2; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export MTOOLS_SKIP_CHECK=1
mkdir -p $T/iso/boot $T/iso/EFI/BOOT $T/iso/EFI/KIRA
SZ=$(( ($(stat -c %s "$IMG") + $(stat -c %s "$EFI")) / 1048576 + 8 ))
dd if=/dev/zero of=$T/iso/boot/kira-esp.img bs=1M count=$SZ status=none
# FAT16 (как у x86-64): FAT32 с таким малым числом кластеров прошивка отвергает как недопустимую
mformat -i $T/iso/boot/kira-esp.img -T $((SZ * 2048)) -h 64 -s 32 -v KIRAESP ::
mmd -i $T/iso/boot/kira-esp.img ::/EFI ::/EFI/BOOT ::/EFI/KIRA
mcopy -i $T/iso/boot/kira-esp.img "$EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i $T/iso/boot/kira-esp.img "$IMG" ::/EFI/KIRA/KIRA-ARM64.IMG
cp "$EFI" $T/iso/EFI/BOOT/BOOTAA64.EFI
cp "$IMG" $T/iso/EFI/KIRA/KIRA-ARM64.IMG
xorriso -as mkisofs -quiet -iso-level 3 -V KIRA_ARM64 -o "$OUT" -e boot/kira-esp.img -no-emul-boot $T/iso
