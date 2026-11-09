#!/bin/bash
# Собрать драйвер BSD из корпуса OpenBSD в ELF Ring 3: tools/bsd_build.sh НАБОР ИМЯ cfattach ВЫХОД.elf файл.c...
#   пример: tools/bsd_build.sh ne2k ne ne_pci_ca out.elf dev/ic/dp8390.c dev/ic/ne2000.c dev/ic/rtl80x9.c dev/pci/if_ne_pci.c
set -e
cd "$(dirname "$0")/.."
SET=$1; NAME=$2; CA=$3; OUT=$4; shift 4
CC=${CC:-clang}
S=build-bsd/corpus/$SET/sys; G=build-bsd/gen/$SET; mkdir -p $G/dev/pci $G/obj
python3 tools/bsd_corpus.py fetch >/dev/null
echo '#define NBPFILTER 0' > $G/bpfilter.h
SRCS=""; for f in "$@"; do SRCS="$SRCS $S/$f"; done
python3 tools/bsd_pcidevs.py gen build-bsd/corpus/pcidevs/sys/dev/pci/pcidevs $G/dev/pci/pcidevs.h $SRCS
python3 tools/bsd_glue.py $G/glue.c $NAME $CA
UCFLAGS="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -fno-builtin -w -D_KERNEL"
INC="-I$G -Ikcc/include/bsd -I$S -Ikcc/include -Iuserland -I."
OBJS=""
for f in $SRCS $G/glue.c userland/bsd_rt.c; do o=$G/obj/$(basename $f .c).o; $CC $UCFLAGS $INC -c $f -o $o; OBJS="$OBJS $o"; done
ld.lld -T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro -o $OUT $OBJS
