#!/bin/bash
# Хеш исходников UEFI-загрузчика: по нему видно, что prebuilt/BOOTX64.EFI собран из ТЕКУЩИХ исходников.
cd "$(dirname "$0")/.."
cat arch/x86_64/uefi/*.c arch/x86_64/uefi/*.h bootinfo.c bootinfo.h build_uefi.sh bootsig.c bootsig.h tpm2.c tpm2.h rollback.c rollback.h 2>/dev/null | sha256sum | cut -d' ' -f1
