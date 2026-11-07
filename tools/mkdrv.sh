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
$HOSTCC -O1 -Wall -I. -Iarch/x86_64 -o tools/kdrvpack tools/kdrvpack.c kdrv_fmt.c pk.c crypto.c sha256.c
$HOSTCC -O1 -Wall -I. -Iarch/x86_64 -o tools/kdrvsig tools/kdrvsig.c kdrv_fmt.c pk.c crypto.c sha256.c
T=$(mktemp -d)
P="--pci 1234:11e8 --source tools/mkdrv.sh(тестовый-драйвер-edu)"
tools/kdrvpack abad  $ELF $T/abad.kdrv  $T/abad.rec  $P --caps mmio,pcidev --confirm --corrupt >/dev/null
tools/kdrvpack eduun $ELF $T/eduun.kdrv $T/eduun.rec $P --caps mmio,pcidev >/dev/null
tools/kdrvpack edu   $ELF $T/edu.kdrv   $T/edu.rec   $P --caps mmio,pcidev --confirm >/dev/null
tools/kdrvpack edunc $ELF $T/edunc.kdrv $T/edunc.rec $P --caps mmio,pcidev --confirm >/dev/null

# Optional publisher authentication. The private key stays outside the repository and ISO.
# KDRV_SIGNING_KEY must match the public key in kdrv_trust.h; otherwise kdrvsig rejects it.
if [ -n "${KDRV_SIGNING_KEY:-}" ]; then
    KEY_ID=${KDRV_SIGNING_KEY_ID:-$(sed -n 's/^#define KDRV_TRUST_KEY_ID "\(.*\)"/\1/p' kdrv_trust.h)}
    for n in abad eduun edu edunc; do
        openssl pkeyutl -sign -inkey "$KDRV_SIGNING_KEY" -rawin -in "$T/$n.kdrv" -out "$T/$n.sig.raw"
        tools/kdrvsig "$T/$n.kdrv" "$T/$n.sig.raw" "$T/$n.sig" "$KEY_ID"
    done
fi

for n in abad eduun edu edunc; do
    tools/kfstool $IMG put /drv/$n.kdrv $T/$n.kdrv >/dev/null
    tools/kfstool $IMG put /drv/$n.rec $T/$n.rec >/dev/null
    if [ -f "$T/$n.sig" ]; then tools/kfstool $IMG put /drv/$n.sig $T/$n.sig >/dev/null; fi
done
rm -rf $T
