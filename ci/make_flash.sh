#!/bin/bash
# Флешка для сценариев USB: 64 МБ, FAT32, файл hello.txt.
set -e
mkdir -p ci-out
img=ci-out/flash.img
rm -f "$img"
dd if=/dev/zero of="$img" bs=1M count=64 status=none
# mkfs.fat (dosfstools) есть не на каждой машине; без него образ оставался пустым и сценарии USB падали с «no FAT volume found».
# Запасной путь - mformat из mtools (он же ставит mcopy, нужный ниже).
if command -v mkfs.fat >/dev/null 2>&1; then
    mkfs.fat -F 32 -n KIRACI "$img" >/dev/null
elif command -v mformat >/dev/null 2>&1; then
    mformat -i "$img" -F -v KIRACI ::
else
    echo "make_flash: нужен mkfs.fat (dosfstools) или mformat (mtools)" >&2; exit 1
fi
printf 'kira-ci: флешка стенда\n' > ci-out/hello.txt
python3 - <<'PY'
with open("ci-out/big.bin", "wb") as f:
    f.write(bytes(((i * 13 + 5) & 0xff) for i in range(100000)))
PY
mcopy -i "$img" ci-out/hello.txt ::/hello.txt
mcopy -i "$img" ci-out/big.bin ::/big.bin
echo "flash: $img (hello.txt + 100 KB big.bin; USB tests never write to the device)"
# Проверка: на образе действительно FAT32 (метка файловой системы в загрузочном секторе), иначе стенд не имеет права идти дальше
[ "$(dd if="$img" bs=1 skip=82 count=5 status=none)" = "FAT32" ] || { echo "make_flash: на $img нет FAT32" >&2; exit 1; }
