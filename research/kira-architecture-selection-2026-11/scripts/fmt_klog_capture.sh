#!/bin/bash
# [E] Полный serial-журнал загрузки KIRA (kira.iso + диск kira.disk, как run_qemu.sh, но -snapshot: ничего не пишется) - набор «журнал klog» для fmt_datalab.py. 30 с, ядро 0.
. "$(dirname "$0")/fmt_env.sh"; L=$FMT_LOGS/fmt_klog_full.log; : > $L
$C0 timeout 30 qemu-system-x86_64 -machine pc -m 512M -smp 2 -drive file=$FMT_ART/kira.disk,format=raw,if=none,id=kd0,snapshot=on -device virtio-blk-pci,drive=kd0,disable-legacy=on \
  -boot order=d -cdrom $FMT_ART/kira.iso -display none -audiodev none,id=a0 -serial file:$L -no-reboot >/dev/null 2>&1
echo "serial: $(wc -c < $L) Б, строк $(wc -l < $L); маркер: $(grep -c 'Command line ready' $L)"
