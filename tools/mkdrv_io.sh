#!/bin/bash
# Тестовый пакет драйвера портов (QEMU -device ne2k_pci, 10ec:8029): tools/mkdrv_io.sh образ драйвер.elf [компилятор-хоста]
set -e
cd "$(dirname "$0")/.."
IMG=$1; ELF=$2; HOSTCC=${3:-clang}
$HOSTCC -O1 -Wall -I. -Iarch/x86_64 -o tools/kdrvpack tools/kdrvpack.c kdrv_fmt.c sha256.c
T=$(mktemp -d)
tools/kdrvpack ne2kio $ELF $T/ne2kio.kdrv $T/ne2kio.rec --pci 10ec:8029 --source tools/mkdrv_io.sh\(тестовый-драйвер-портов\) --caps mmio,pcidev --confirm >/dev/null
tools/kfstool $IMG put /drv/ne2kio.kdrv $T/ne2kio.kdrv >/dev/null
tools/kfstool $IMG put /drv/ne2kio.rec $T/ne2kio.rec >/dev/null
rm -rf $T
