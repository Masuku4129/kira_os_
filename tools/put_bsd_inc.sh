#!/bin/bash
# Заголовки слоя BSD и среда исполнения - в /etc/kcc/include образа плоскими именами: sys/param.h -> sys_param.h (drvget сворачивает путь так же).
# tools/put_bsd_inc.sh образ
set -e
cd "$(dirname "$0")/.."
IMG=$1
( cd kcc/include/bsd && find . -type f -name '*.h' | sed 's|^\./||' ) | while read -r f; do
    tools/kfstool $IMG put /etc/kcc/include/$(echo $f | tr / _) kcc/include/bsd/$f >/dev/null
done
tools/kfstool $IMG put /etc/kcc/include/bsd_rt.c userland/bsd_rt.c >/dev/null
