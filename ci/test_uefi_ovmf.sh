#!/usr/bin/env bash
# End-to-end x86-64 UEFI boot under OVMF. The OS intentionally keeps running,
# so timeout is the expected stop condition after serial milestones are reached.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UEFI_ROOT="${1:-$ROOT/ci-out/uefi}"
OUT="${2:-$ROOT/ci-out/uefi-ovmf}"
mkdir -p "$OUT"

LOADER="$UEFI_ROOT/EFI/BOOT/BOOTX64.EFI"
KERNEL="$UEFI_ROOT/EFI/KIRA/KIRA.ELF"
for file in "$LOADER" "$KERNEL"; do
    if [[ ! -s "$file" ]]; then
        echo "OVMF test: required boot file is missing: $file" >&2
        exit 2
    fi
done

if [[ -s /usr/share/OVMF/OVMF_CODE_4M.fd && -s /usr/share/OVMF/OVMF_VARS_4M.fd ]]; then
    CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
    VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.fd
elif [[ -s /usr/share/OVMF/OVMF_CODE.fd && -s /usr/share/OVMF/OVMF_VARS.fd ]]; then
    CODE=/usr/share/OVMF/OVMF_CODE.fd
    VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS.fd
else
    echo "OVMF test: firmware CODE/VARS pair not found under /usr/share/OVMF" >&2
    exit 2
fi

ESP="$OUT/esp.img"
VARS="$OUT/OVMF_VARS.fd"
SERIAL="$OUT/serial.log"
QEMU_LOG="$OUT/qemu.log"
truncate -s 64M "$ESP"
mkfs.vfat "$ESP" >/dev/null
mmd -i "$ESP" ::/EFI ::/EFI/BOOT ::/EFI/KIRA
mcopy -i "$ESP" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$KERNEL" ::/EFI/KIRA/KIRA.ELF
cp "$VARS_TEMPLATE" "$VARS"
: > "$SERIAL"

set +e
timeout --signal=TERM --kill-after=5s 45s qemu-system-x86_64 \
    -machine pc,accel=tcg -cpu qemu64 -m 1024 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive if=ide,format=raw,index=0,file="$ESP" \
    -boot order=c,menu=off -display none -monitor none -serial "file:$SERIAL" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1
qemu_rc=$?
set -e

required=(
    "[KIRA UEFI] Common BootInfo accepted"
    "[KIRA BOOT] BootInfo v1 validated:"
    "[KIRA VMM] NULL guard installed"
)
for marker in "${required[@]}"; do
    if ! grep -Fq "$marker" "$SERIAL"; then
        echo "OVMF test: missing kernel milestone: $marker" >&2
        echo "--- serial.log ---" >&2
        cat "$SERIAL" >&2 || true
        echo "--- qemu.log ---" >&2
        cat "$QEMU_LOG" >&2 || true
        exit 1
    fi
done

if grep -Eiq '\\[KIRA (UEFI|BOOT|VMM)\\].*(Invalid|Cannot|failed|rejected|panic)' "$SERIAL"; then
    echo "OVMF test: boot log contains a fatal boot-path diagnostic" >&2
    cat "$SERIAL" >&2
    exit 1
fi

if [[ "$qemu_rc" -ne 124 && "$qemu_rc" -ne 0 ]]; then
    echo "OVMF test: QEMU exited unexpectedly with status $qemu_rc" >&2
    cat "$QEMU_LOG" >&2
    exit "$qemu_rc"
fi

echo "OVMF test: PASS (UEFI loader -> BootInfo v1 -> kernel VMM)"
cat "$SERIAL"
