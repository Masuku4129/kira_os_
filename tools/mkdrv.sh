#!/bin/bash
# Пакеты тестового драйвера edu (QEMU -device edu) в /drv образа диска: tools/mkdrv.sh образ драйвер.elf [компилятор-хоста]
#   edu    - нормальный пакет, все права, подтверждён сборщиком образа (kdrvpack --confirm)
#   Порядок записи важен: менеджер пробует пакеты в порядке каталога, плохие должны идти раньше edu.
#   abad   - тот же ELF, но хеш в .rec испорчен: ядро обязано отказаться (имя раньше edu - его пробуют первым)
#   edunc  - права только mmio и irq: попытка DMA получает отказ ядра
#   eduun  - не подтверждён пользователем (confirmed=0): не запускается
set -e
cd "$(dirname "$0")/.."
IMG=$1; ELF=$2; HOSTCC=${3:-clang}
$HOSTCC -O1 -Wall -I. -Iarch/x86_64 -o tools/kdrvpack tools/kdrvpack.c kdrv_fmt.c sha256.c
T=$(mktemp -d)
P="--pci 1234:11e8 --source tools/mkdrv.sh(тестовый-драйвер-edu)"
tools/kdrvpack abad  $ELF $T/abad.kdrv  $T/abad.rec  $P --caps mmio,irq,dma,pcidev --confirm --corrupt >/dev/null
tools/kdrvpack eduun $ELF $T/eduun.kdrv $T/eduun.rec $P --caps mmio,irq,dma,pcidev >/dev/null
tools/kdrvpack edu   $ELF $T/edu.kdrv   $T/edu.rec   $P --caps mmio,irq,dma,pcidev --confirm >/dev/null
tools/kdrvpack edunc $ELF $T/edunc.kdrv $T/edunc.rec $P --caps mmio,pcidev --confirm >/dev/null
for n in abad eduun edu edunc; do
    tools/kfstool $IMG put /drv/$n.kdrv $T/$n.kdrv >/dev/null
    tools/kfstool $IMG put /drv/$n.rec $T/$n.rec >/dev/null
done
rm -rf $T
