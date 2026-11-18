#!/usr/bin/env bash
# Выпуск подписанного образа для доверенной загрузки (А2, docs/SECURE_BOOT.md): подписывает загрузчик ключом db (Authenticode, его проверяет
# прошивка), подписывает манифест KIRA.SIG ключом выпуска (его проверяет загрузчик) и собирает диск GPT + ESP.
#
#   tools/sbsign_image.sh --loader BOOTX64.EFI --kernel kira.bin --release-key rel.pem --version 3 \
#       --db-key db.key --db-cert db.crt --out kira-secure.img [--initrd initrd.img] [--size-mib 64]
#
# BOOTX64.EFI - загрузчик, собранный как `KIRA_SECURE=1 KIRA_BOOT_PUBKEY=rel.pub ./build_uefi.sh DIR` (ключ выпуска вшит в него).
# Закрытые ключи (--release-key, --db-key) остаются у выпускающего: в образ они не попадают, в git не коммитятся.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOADER=; KERNEL=; INITRD=; RKEY=; VERSION=; DBKEY=; DBCERT=; OUT=; SIZE=64
while [[ $# -gt 0 ]]; do
    case "$1" in
        --loader) LOADER="$2"; shift 2 ;;
        --kernel) KERNEL="$2"; shift 2 ;;
        --initrd) INITRD="$2"; shift 2 ;;
        --release-key) RKEY="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --db-key) DBKEY="$2"; shift 2 ;;
        --db-cert) DBCERT="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --size-mib) SIZE="$2"; shift 2 ;;
        *) echo "sbsign_image: неизвестный параметр $1" >&2; exit 2 ;;
    esac
done
for v in LOADER KERNEL RKEY VERSION DBKEY DBCERT OUT; do
    [[ -n "${!v}" ]] || { echo "sbsign_image: не задан обязательный параметр ($v), см. описание в начале скрипта" >&2; exit 2; }
done
for f in "$LOADER" "$KERNEL" "$RKEY" "$DBKEY" "$DBCERT"; do
    [[ -s "$f" ]] || { echo "sbsign_image: нет файла $f" >&2; exit 1; }
done
[[ -z "$INITRD" || -s "$INITRD" ]] || { echo "sbsign_image: нет файла $INITRD" >&2; exit 1; }
command -v sbsign >/dev/null || { echo "sbsign_image: нужен sbsign (apt install sbsigntool)" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# 1. Authenticode-подпись загрузчика ключом db: прошивка с Secure Boot отвергнет любой другой BOOTX64.EFI.
sbsign --key "$DBKEY" --cert "$DBCERT" --output "$tmp/BOOTX64.EFI" "$LOADER" >/dev/null
sbverify --cert "$DBCERT" "$tmp/BOOTX64.EFI" >/dev/null

# 2. Подписанный манифест: хэши ядра и initrd и номер выпуска, подпись Ed25519 ключом выпуска.
files=("$KERNEL")
[[ -n "$INITRD" ]] && files+=("$INITRD")
python3 "$ROOT/tools/kirasig.py" sign --key "$RKEY" --version "$VERSION" --out "$tmp/KIRA.SIG" "${files[@]}"

# 3. Диск GPT + ESP: BOOTX64.EFI (подписанный), KIRA.ELF, KIRA.SIG, при наличии INITRD.IMG.
args=(--sig "$tmp/KIRA.SIG" "$tmp/BOOTX64.EFI" "$KERNEL" "$OUT" "$SIZE")
[[ -n "$INITRD" ]] && args+=("$INITRD")
python3 "$ROOT/tools/mkuefi.py" "${args[@]}"
echo "sbsign_image: $OUT готов: загрузчик подписан ключом db, манифест выпуска $VERSION подписан ключом выпуска"
