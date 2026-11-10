#!/bin/bash
# Собственная BIOS-цепочка (stage1+stage2) под SeaBIOS в QEMU: успешная загрузка и отказы на повреждённых входных данных.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
KERNEL=${1:-kira.bin}
nasm -f bin arch/x86_64/bios/stage1.asm -o $T/s1.bin && nasm -f bin -w+all arch/x86_64/bios/stage2.asm -o $T/s2.bin || exit 1
python3 tools/mkbios.py $T/s1.bin $T/s2.bin "$KERNEL" $T/ok.img >/dev/null || exit 1
run() { timeout 10 qemu-system-x86_64 -drive file=$1,format=raw -m ${2:-256} -display none -serial stdio -no-reboot 2>&1 | head -c 4000; }
fail=0
expect() { if echo "$OUT" | grep -q "$2"; then echo "ok:   $1"; else echo "FAIL: $1 (нет '$2')"; fail=1; fi; }
OUT=$(run $T/ok.img);                      expect "загрузка ядра" "Kernel core starting"
expect "W^X ядра (нет страниц W+X)" "W^X kernel: ok"
echo "$OUT" | grep -q "GRUB" && { echo "FAIL: GRUB в выводе"; fail=1; }
cp $T/ok.img $T/crc.img; printf '\xff' | dd of=$T/crc.img bs=1 seek=$((64*512+5000)) conv=notrunc 2>/dev/null
OUT=$(run $T/crc.img);                     expect "повреждённый образ (CRC)" "CRC mismatch"
python3 - "$T" <<'PY'
import sys, struct, zlib
t = sys.argv[1]
img = bytearray(open(t + '/ok.img', 'rb').read())
k = 64 * 512
# ELF с нереальным p_paddr первого PT_LOAD (ниже 1 МиБ) и пересчитанным CRC: проверка должна отвергнуть именно ELF
bad = bytearray(img)
phoff = struct.unpack_from('<Q', bad, k + 32)[0]
struct.pack_into('<Q', bad, k + phoff + 24, 0x1000)
size = struct.unpack_from('<I', bad, 512 + 16384 - 32 + 8)[0]
crc = zlib.crc32(bytes(bad[k:k + size])) & 0xFFFFFFFF
struct.pack_into('<I', bad, 512 + 16384 - 32 + 12, crc)
open(t + '/elf.img', 'wb').write(bad)
PY
OUT=$(run $T/elf.img);                     expect "p_paddr ниже 1 МиБ" "outside usable RAM"
OUT=$(run $T/ok.img 16);                   expect "мало памяти (16 МиБ)" "no RAM at 16MiB"
OUT=$(run $T/ok.img 64);                   expect "загрузка при 64 МиБ" "Kernel core starting"
printf 'KIRA-INITRD-TEST\\n' > $T/initrd.img
python3 tools/mkbios.py $T/s1.bin $T/s2.bin "$KERNEL" $T/module.img $T/initrd.img >/dev/null || exit 1
OUT=$(run $T/module.img);                  expect "загрузка BIOS initrd" "modules=1"
python3 - "$T/module.img" "$T/module-corrupt.img" <<'PY'
import sys, struct
source, target = sys.argv[1:3]
image = bytearray(open(source, "rb").read())
manifest = 512 + 16384 - 32
module_lba = struct.unpack_from("<I", image, manifest + 16)[0]
if not module_lba:
    raise SystemExit("module manifest missing")
image[module_lba * 512] ^= 0xff
open(target, "wb").write(image)
PY
OUT=$(run $T/module-corrupt.img);          expect "повреждённый initrd (CRC)" "initrd CRC mismatch"
[ $fail = 0 ] && echo "bios boot: PASS" || { echo "bios boot: FAIL"; exit 1; }
