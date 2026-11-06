#!/bin/bash
# Флешка для сценариев USB: 64 МБ, FAT32, файл hello.txt.
set -e
mkdir -p ci-out
img=ci-out/flash.img
rm -f "$img"
dd if=/dev/zero of="$img" bs=1M count=64 status=none
mkfs.fat -F 32 -n KIRACI "$img" >/dev/null
printf 'kira-ci: флешка стенда\n' > ci-out/hello.txt
mcopy -i "$img" ci-out/hello.txt ::/hello.txt
echo "flash: $img"
