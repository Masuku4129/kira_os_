#!/bin/bash
# Пакет драйвера Ring 3 в /drv образа диска: tools/mkdrv_pkg.sh образ имя драйвер.elf VVVV:PPPP права [компилятор-хоста]
#   права - как у kdrvpack --caps (mmio,pcidev,netdev ...); пакет подтверждён сборщиком образа (--confirm), только для CI/QEMU
set -e
cd "$(dirname "$0")/.."
IMG=$1; NAME=$2; ELF=$3; PCI=$4; CAPS=$5; HOSTCC=${6:-clang}
$HOSTCC -O1 -Wall -I. -Iarch/x86_64 -o tools/kdrvpack tools/kdrvpack.c kdrv_fmt.c sha256.c
T=$(mktemp -d)
tools/kdrvpack $NAME $ELF $T/$NAME.kdrv $T/$NAME.rec --pci $PCI --source "tools/mkdrv_pkg.sh(тестовый-пакет-$NAME)" --caps $CAPS --confirm >/dev/null
tools/kfstool $IMG put /drv/$NAME.kdrv $T/$NAME.kdrv >/dev/null
tools/kfstool $IMG put /drv/$NAME.rec $T/$NAME.rec >/dev/null
rm -rf $T
