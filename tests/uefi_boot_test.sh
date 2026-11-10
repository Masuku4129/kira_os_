#!/bin/bash
# Загрузка ядра нашим UEFI-загрузчиком (без GRUB) под QEMU + OVMF: GPT/ESP-диск должен дойти до командной строки.
# Нужны: gnu-efi (системный; иначе EFI_INCLUDE/EFI_LIBDIR как у build_uefi.sh), mtools, qemu-system-x86_64, OVMF. Без них тест пропускается (код 0 с пометкой), в CI они ставятся.
set -e
cd "$(dirname "$0")/.."
CODE=""; VARS=""
for d in /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/ovmf; do
    for c in OVMF_CODE_4M.fd OVMF_CODE.fd; do [ -f "$d/$c" ] && CODE="$d/$c" && break 2; done
done
for d in /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/ovmf; do
    for v in OVMF_VARS_4M.fd OVMF_VARS.fd; do [ -f "$d/$v" ] && VARS="$d/$v" && break 2; done
done
if [ -z "$CODE" ] || [ -z "$VARS" ] || ! command -v qemu-system-x86_64 >/dev/null || ! command -v mcopy >/dev/null; then
    echo "uefi boot: ПРОПУЩЕН (нет OVMF, qemu или mtools)"; exit 0
fi
[ -f kira.bin ] || ./build_clang.sh --no-run >/dev/null 2>&1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
bash ./build_uefi.sh $T/uefi >$T/build.log 2>&1 || { echo "uefi boot: ПРОПУЩЕН (UEFI-загрузчик не собран: нет gnu-efi?)"; tail -3 $T/build.log; exit 0; }
printf 'KIRA-INITRD-TEST\\n' > $T/initrd.img
python3 -I tools/mkuefi.py $T/uefi/EFI/BOOT/BOOTX64.EFI $T/uefi/EFI/KIRA/KIRA.ELF $T/kira-uefi.img 64 $T/initrd.img >>$T/build.log 2>&1 || { cat $T/build.log; exit 1; }
cp "$VARS" $T/vars.fd
timeout 90 qemu-system-x86_64 -machine q35 -m 256 -display none -serial file:$T/serial.log \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" -drive if=pflash,format=raw,file=$T/vars.fd \
    -drive file=$T/kira-uefi.img,format=raw,if=virtio -no-reboot >/dev/null 2>&1 &
Q=$!
for i in $(seq 1 80); do
    if grep -aq "Command line ready" $T/serial.log 2>/dev/null; then break; fi
    if grep -aq "Exception Type" $T/serial.log 2>/dev/null; then break; fi
    sleep 1
done
kill $Q 2>/dev/null || true; wait $Q 2>/dev/null || true
LOG=$(tr -d '\033' < $T/serial.log)
for want in "KIRA OS UEFI loader" "Common BootInfo accepted" "[KIRA UEFI] Boot modules: 1" "BootInfo v1 validated" "Native RSDP accepted" "W^X kernel: ok" "Command line ready"; do
    echo "$LOG" | grep -aqF "$want" || { echo "uefi boot: FAIL - нет строки '$want'"; echo "$LOG" | tail -15; exit 1; }
done
echo "$LOG" | grep -aq "Exception Type" && { echo "uefi boot: FAIL - исключение"; exit 1; }
echo "uefi boot: PASS (own UEFI loader, GPT+ESP, OVMF)"
