#!/bin/bash
# Пакеты тестового драйвера edu (QEMU -device edu) в /drv образа диска: tools/mkdrv.sh образ драйвер.elf [компилятор-хоста]
#   edu    - MMIO-only пакет: DMA/MSI не выдаются без IOMMU; подтверждён сборщиком образа
#   Порядок записи важен: менеджер пробует пакеты в порядке каталога, плохие должны идти раньше edu.
#   abad   - тот же ELF, но хеш в .rec испорчен: ядро обязано отказаться (имя раньше edu - его пробуют первым)
#   edunc  - второй MMIO-only пакет для проверки отказа при занятом устройстве
#   eduun  - не подтверждён пользователем (confirmed=0): не запускается
set -e
cd "$(dirname "$0")/.."
IMG=$1; ELF=$2; HOSTCC=${3:-clang}
$HOSTCC -O1 -Wall -I. -Iarch/x86_64 -o tools/kdrvpack tools/kdrvpack.c kdrv_fmt.c sha256.c
T=$(mktemp -d)
P="--pci 1234:11e8 --source tools/mkdrv.sh(тестовый-драйвер-edu)"
tools/kdrvpack abad  $ELF $T/abad.kdrv  $T/abad.rec  $P --caps mmio,pcidev --confirm --corrupt >/dev/null
tools/kdrvpack eduun $ELF $T/eduun.kdrv $T/eduun.rec $P --caps mmio,pcidev >/dev/null
tools/kdrvpack edu   $ELF $T/edu.kdrv   $T/edu.rec   $P --caps mmio,pcidev --confirm >/dev/null
tools/kdrvpack edunc $ELF $T/edunc.kdrv $T/edunc.rec $P --caps mmio,pcidev --confirm >/dev/null
# Hostile CLI inputs must fail before producing a package.
expect_reject() {
    if tools/kdrvpack invalid $ELF $T/invalid.kdrv $T/invalid.rec "$@" >/dev/null 2>&1; then
        echo "kdrvpack accepted invalid arguments: $*"; exit 1
    fi
}
expect_reject --pci 10000:11e8 --caps mmio
expect_reject --pci 1234:11e8junk --caps mmio
expect_reject --class 1000000:ffffff --caps mmio
expect_reject --pci 1234:11e8 --caps mmio --version -1
expect_reject --pci 1234:11e8 --caps mmio --version 4294967296
for n in abad eduun edu edunc; do
    tools/kfstool $IMG put /drv/$n.kdrv $T/$n.kdrv >/dev/null
    tools/kfstool $IMG put /drv/$n.rec $T/$n.rec >/dev/null
done
rm -rf $T
