#!/usr/bin/env bash
# Build the native KIRA x86-64 UEFI application and its ELF64 kernel payload.
# Requires GCC, GNU-EFI headers/libraries, ld, and objcopy.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$ROOT/ci-out/uefi}"
CC="${EFI_CC:-gcc}"
LD="${EFI_LD:-ld}"
OBJCOPY="${EFI_OBJCOPY:-objcopy}"
EFI_INC="${EFI_INCLUDE:-/usr/include/efi}"
EFI_LIB="${EFI_LIBDIR:-/usr/lib}"
CRT="${EFI_CRT:-$EFI_LIB/crt0-efi-x86_64.o}"
LDS="${EFI_LDS:-$EFI_LIB/elf_x86_64_efi.lds}"

if [[ ! -s "$ROOT/kira.bin" ]]; then
    echo "UEFI build: kira.bin is missing; build the kernel first" >&2
    exit 1
fi
for path in "$EFI_INC/efi.h" "$EFI_INC/efilib.h" "$CRT" "$LDS" \
            "$EFI_LIB/libefi.a" "$EFI_LIB/libgnuefi.a"; do
    if [[ ! -e "$path" ]]; then
        echo "UEFI build: GNU-EFI dependency not found: $path" >&2
        exit 1
    fi
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

EFI_CFLAGS=(
    -O2 -Wall -Wextra -Werror -fpic -ffreestanding
    -fshort-wchar -mno-red-zone -maccumulate-outgoing-args
    -fno-stack-protector -fno-stack-check -fno-strict-aliasing
    -DEFI_FUNCTION_WRAPPER
    -isystem "$EFI_INC" -isystem "$EFI_INC/x86_64"
)

# Доверенный режим (KIRA_SECURE=1, docs/SECURE_BOOT.md): в загрузчик вшивается открытый ключ выпуска, и он не передаёт управление ядру
# без валидной подписи манифеста KIRA.SIG. По умолчанию (режим разработки) загрузчик устроен как раньше и ничего не проверяет.
#   KIRA_BOOT_PUBKEY=файл      открытый ключ Ed25519, 32 байта (python3 tools/kirasig.py keygen): обязателен
#   KIRA_MIN_IMAGE_VERSION=N   нижний порог номера выпуска (защита от отката), по умолчанию 0
#   KIRA_REQUIRE_SB=0          не требовать включённый Secure Boot прошивки (по умолчанию 1)
#   KIRA_REQUIRE_TPM=1         не загружать без измерения в TPM (по умолчанию 0)
SECURE_CFLAGS=()
SECURE_OBJS=()
if [[ "${KIRA_SECURE:-0}" == 1 ]]; then
    if [[ -z "${KIRA_BOOT_PUBKEY:-}" || ! -s "$KIRA_BOOT_PUBKEY" ]]; then
        echo "UEFI build: KIRA_SECURE=1 requires KIRA_BOOT_PUBKEY=<32-byte Ed25519 public key> (python3 tools/kirasig.py keygen)" >&2
        exit 1
    fi
    if [[ "$(wc -c < "$KIRA_BOOT_PUBKEY")" -ne 32 ]]; then
        echo "UEFI build: KIRA_BOOT_PUBKEY must be exactly 32 bytes" >&2
        exit 1
    fi
    python3 "$ROOT/tools/kirasig.py" cheader "$KIRA_BOOT_PUBKEY" "$tmp/kira_bootkey.h"
    SECURE_CFLAGS=(-DKIRA_SECURE_BOOT=1 -I"$tmp"
        -DKIRA_MIN_IMAGE_VERSION="${KIRA_MIN_IMAGE_VERSION:-0}u"
        -DKIRA_REQUIRE_SB="${KIRA_REQUIRE_SB:-1}" -DKIRA_REQUIRE_TPM="${KIRA_REQUIRE_TPM:-0}")
    # Криптография - те же исходники, что в ядре (pk.c, crypto.c, sha256.c): общий проверенный код, без libc. Предупреждения этих файлов
    # ловят их собственные хост-тесты, поэтому здесь они собираются без -Werror.
    SHARED_CFLAGS=()
    for flag in "${EFI_CFLAGS[@]}"; do
        [[ "$flag" == -Werror ]] || SHARED_CFLAGS+=("$flag")
    done
    for src in bootsig pk crypto sha256; do
        "$CC" "${SHARED_CFLAGS[@]}" -I"$ROOT" -c "$ROOT/$src.c" -o "$tmp/$src.o"
        SECURE_OBJS+=("$tmp/$src.o")
    done
    echo "UEFI build: TRUSTED mode (release key embedded, min image version ${KIRA_MIN_IMAGE_VERSION:-0})"
fi

"$CC" "${EFI_CFLAGS[@]}" -I"$ROOT" -I"$ROOT/arch/x86_64/uefi" "${SECURE_CFLAGS[@]}" \
    -c "$ROOT/arch/x86_64/uefi/loader.c" -o "$tmp/loader.o"
"$CC" "${EFI_CFLAGS[@]}" -I"$ROOT" -I"$ROOT/arch/x86_64/uefi" \
    -c "$ROOT/arch/x86_64/uefi/uefi_kbi.c" -o "$tmp/uefi-kbi.o"
# The UEFI adapter validates the handoff with the same canonical validator
# used by the BIOS/Multiboot paths. Link it into the EFI image explicitly;
# otherwise a shared-object link can silently leave kbi_validate unresolved.
"$CC" "${EFI_CFLAGS[@]}" -I"$ROOT" -c "$ROOT/bootinfo.c" \
    -o "$tmp/bootinfo.o"

"$LD" -nostdlib -z defs -znocombreloc -T "$LDS" -shared -Bsymbolic \
    "$CRT" "$tmp/loader.o" "$tmp/uefi-kbi.o" "$tmp/bootinfo.o" "${SECURE_OBJS[@]}" \
    -L"$EFI_LIB" -lefi -lgnuefi -o "$tmp/loader.so"

# GNU-EFI's startup relocator handles R_X86_64_RELATIVE entries, not lazy
# PLT resolution. A remaining JUMP_SLOT would call through an unresolved GOT
# entry at runtime, so reject such images at build time.
if readelf -r "$tmp/loader.so" | grep -q 'JUMP_SLOT'; then
    echo "UEFI build: unresolved PLT relocation remains in loader.so" >&2
    readelf -r "$tmp/loader.so" >&2
    exit 1
fi

mkdir -p "$OUT/EFI/BOOT" "$OUT/EFI/KIRA"
# Use --output-target explicitly: newer GNU binutils interpret --target
# as both input and output format, which breaks auto-detection of loader.so.
# .rodata нужен новым линкер-скриптам gnu-efi (старые сливают его в .data): без него строки L"..." теряются, а Print печатает мусор.
"$OBJCOPY" -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
    -j .rel -j .rela -j '.rel.*' -j '.rela.*' -j .reloc --output-target=efi-app-x86_64 \
    "$tmp/loader.so" "$OUT/EFI/BOOT/BOOTX64.EFI"
cp "$ROOT/kira.bin" "$OUT/EFI/KIRA/KIRA.ELF"
rm -f "$OUT/EFI/KIRA/INITRD.IMG"
INITRD="${KIRA_INITRD:-$ROOT/initrd.img}"
if [[ -n "$INITRD" && -s "$INITRD" ]]; then
    INITRD_SIZE=$(wc -c < "$INITRD")
    if (( INITRD_SIZE > 67108864 )); then
        echo "UEFI build: initrd exceeds the 64 MiB loader limit" >&2
        exit 1
    fi
    cp "$INITRD" "$OUT/EFI/KIRA/INITRD.IMG"
    echo "UEFI build: optional initrd included ($INITRD)"
fi

objdump -f "$OUT/EFI/BOOT/BOOTX64.EFI" | grep 'pei-x86-64' >/dev/null || {
    echo "UEFI build: BOOTX64.EFI is not a PE32+ x86-64 image" >&2
    exit 1
}
objdump -p "$OUT/EFI/BOOT/BOOTX64.EFI" | grep -E 'Subsystem[[:space:]]+0000000a' >/dev/null || {
    echo "UEFI build: BOOTX64.EFI is not marked as an EFI application (subsystem 10)" >&2
    exit 1
}
readelf -h "$OUT/EFI/KIRA/KIRA.ELF" | grep 'ELF64' >/dev/null || {
    echo "UEFI build: kernel payload is not ELF64" >&2
    exit 1
}

echo "UEFI build: PE32+ loader and ELF64 kernel payload ready at $OUT"
