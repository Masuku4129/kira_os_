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

"$CC" "${EFI_CFLAGS[@]}" -c "$ROOT/arch/x86_64/uefi/loader.c" \
    -o "$tmp/loader.o"
"$CC" "${EFI_CFLAGS[@]}" -I"$ROOT" -I"$ROOT/arch/x86_64/uefi" \
    -c "$ROOT/arch/x86_64/uefi/uefi_kbi.c" -o "$tmp/uefi-kbi.o"

"$LD" -nostdlib -znocombreloc -T "$LDS" -shared -Bsymbolic \
    "$CRT" "$tmp/loader.o" "$tmp/uefi-kbi.o" \
    -L"$EFI_LIB" -lefi -lgnuefi -o "$tmp/loader.so"

mkdir -p "$OUT/EFI/BOOT" "$OUT/EFI/KIRA"
"$OBJCOPY" -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
    -j .rel -j .rela -j .reloc --target=efi-app-x86_64 \
    "$tmp/loader.so" "$OUT/EFI/BOOT/BOOTX64.EFI"
cp "$ROOT/kira.bin" "$OUT/EFI/KIRA/KIRA.ELF"

"$OBJCOPY" --info | grep -q 'efi-app-x86_64' || {
    echo "UEFI build: objcopy lacks the efi-app-x86-64 target" >&2
    exit 1
}
objdump -f "$OUT/EFI/BOOT/BOOTX64.EFI" | grep -q 'pei-x86-64' || {
    echo "UEFI build: BOOTX64.EFI is not a PE32+ x86-64 image" >&2
    exit 1
}
readelf -h "$OUT/EFI/KIRA/KIRA.ELF" | grep -q 'ELF64' || {
    echo "UEFI build: kernel payload is not ELF64" >&2
    exit 1
}

echo "UEFI build: PE32+ loader and ELF64 kernel payload ready at $OUT"
