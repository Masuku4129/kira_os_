#!/bin/bash
# ISO Киры с СОБСТВЕННЫМ загрузчиком, без GRUB: tools/mkiso_own.sh kira.bin выход.iso [BOOTX64.EFI KIRA.ELF]
#   BIOS: El Torito без эмуляции, образ = stage1cd + stage2 + ядро (stage2 сам читает ядро с компакт-диска, см. arch/x86_64/bios/stage2.asm);
#   UEFI (если даны BOOTX64.EFI и ядро): второй вход El Torito - образ ESP FAT с \EFI\BOOT\BOOTX64.EFI и \EFI\KIRA\KIRA.ELF.
set -e
cd "$(dirname "$0")/.."
KERNEL=$1; OUT=$2; EFI=$3; ELF=$4
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
nasm -f bin arch/x86_64/bios/stage1cd.asm -o $T/s1.bin
nasm -f bin -w+all arch/x86_64/bios/stage2.asm -o $T/s2.bin
mkdir -p $T/iso/boot
python3 tools/mkbios.py $T/s1.bin $T/s2.bin "$KERNEL" $T/iso/boot/kira-bios.img cd >/dev/null
cp "$KERNEL" $T/iso/boot/kira.bin
ARGS="-b boot/kira-bios.img -no-emul-boot -boot-load-size 34"
if [ -n "$EFI" ] && command -v mformat >/dev/null 2>&1; then
    export MTOOLS_SKIP_CHECK=1
    dd if=/dev/zero of=$T/iso/boot/kira-esp.img bs=1M count=8 2>/dev/null
    mformat -i $T/iso/boot/kira-esp.img -T 16384 -h 64 -s 32 ::
    mmd -i $T/iso/boot/kira-esp.img ::/EFI ::/EFI/BOOT ::/EFI/KIRA
    mcopy -i $T/iso/boot/kira-esp.img "$EFI" ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i $T/iso/boot/kira-esp.img "$ELF" ::/EFI/KIRA/KIRA.ELF
    ARGS="$ARGS -eltorito-alt-boot -e boot/kira-esp.img -no-emul-boot"
fi
xorriso -as mkisofs -quiet -o "$OUT" $ARGS -V KIRA $T/iso 2>/dev/null
