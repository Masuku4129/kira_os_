#!/bin/bash
# Счётчик совместимости: компилируются ли файлы корпуса OpenBSD через слой kcc/include/bsd (clang -fsyntax-only), по наборам.
# Использование: tests/bsd/meter.sh [набор...]  (по умолчанию все наборы из manifest.txt, кроме pcidevs). Нужен корпус (tools/bsd_corpus.py fetch).
cd "$(dirname "$0")/../.."
python3 tools/bsd_corpus.py fetch >/dev/null || { echo "meter: корпус недоступен"; exit 2; }
SETS=${*:-$(awk '$1=="set" && $2!="pcidevs"{print $2}' drivers/bsdcorpus/manifest.txt)}
total=0; okn=0
for s in $SETS; do
    S=build-bsd/corpus/$s/sys; G=build-bsd/gen/$s; mkdir -p $G/dev/pci
    files=$(awk -v s=$s '$1=="file" && $2==s && $3 ~ /\.c$/ {print $3}' drivers/bsdcorpus/manifest.txt)
    srcs=""; for f in $files; do srcs="$srcs $S/$f"; done
    echo '#define NBPFILTER 0' > $G/bpfilter.h
    python3 tools/bsd_pcidevs.py gen build-bsd/corpus/pcidevs/sys/dev/pci/pcidevs $G/dev/pci/pcidevs.h $srcs || { echo "FAIL $s: pcidevs"; total=$((total+1)); continue; }
    for f in $files; do
        total=$((total+1))
        if clang -fsyntax-only -D_KERNEL -ffreestanding -Wno-everything -I$G -Ikcc/include/bsd -I$S -Ikcc/include -Iuserland -I. $S/$f 2>/tmp/meter.err; then okn=$((okn+1)); echo "ok   $s/$f"
        else echo "FAIL $s/$f: $(grep -m1 error /tmp/meter.err)"; fi
    done
done
echo "meter: $okn из $total файлов компилируются"
[ $okn = $total ]
