#!/bin/bash
# Флешка для сценариев USB: 64 МБ, FAT32, файл hello.txt.
set -e
mkdir -p ci-out
img=ci-out/flash.img
rm -f "$img"
dd if=/dev/zero of="$img" bs=1M count=64 status=none
mkfs.fat -F 32 -n KIRACI "$img" >/dev/null
printf 'kira-ci: флешка стенда\n' > ci-out/hello.txt
python3 - <<'PY'
with open("ci-out/big.bin", "wb") as f:
    block = bytes(((i * 13 + 5) & 0xff) for i in range(100000))
    f.write(block)
PY
mcopy -i "$img" ci-out/hello.txt ::/hello.txt
mcopy -i "$img" ci-out/big.bin ::/big.bin
echo "flash: $img (hello.txt + 100 KB big.bin; USB tests never write to the device)"
