#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
$CC -O1 -Wall -Werror -DKIRA_HOST_TEST -I. -Iarch/x86_64 -o "$T/kdrvpack" tools/kdrvpack.c kdrv_fmt.c sha256.c
printf 'ELFtest' > "$T/driver.elf"
"$T/kdrvpack" test "$T/driver.elf" "$T/good.kdrv" "$T/good.rec" --pci 1234:11e8 --caps mmio,pcidev --version 7 --confirm >/dev/null
[ "$(wc -c < "$T/good.kdrv")" -eq 143 ]
[ "$(wc -c < "$T/good.rec")" -eq 216 ]
expect_reject() {
    rm -f "$T/rejected.kdrv" "$T/rejected.rec"
    if "$T/kdrvpack" invalid "$T/driver.elf" "$T/rejected.kdrv" "$T/rejected.rec" "$@" >/dev/null 2>&1; then
        echo "kdrvpack accepted invalid arguments: $*"; exit 1
    fi
    [ ! -e "$T/rejected.kdrv" ] && [ ! -e "$T/rejected.rec" ]
}
expect_reject --pci 10000:11e8 --caps mmio
expect_reject --pci 1234:11e8tail --caps mmio
expect_reject --class 1000000:ffffff --caps mmio
expect_reject --pci 1234:11e8 --caps mmio,,pcidev
expect_reject --pci 1234:11e8 --caps mmio,mmio
expect_reject --pci 1234:11e8 --caps mmio,unknown
expect_reject --pci 1234:11e8 --caps ""
expect_reject --pci 1234:11e8
expect_reject --pci 1234:11e8 --caps mmio --version -1
expect_reject --pci 1234:11e8 --caps mmio --version 0
expect_reject --pci 1234:11e8 --caps mmio --version 4294967296
expect_reject --pci 1234:11e8 --caps mmio --source "$(printf '%096d' 0)"
: > "$T/empty.elf"
if "$T/kdrvpack" invalid "$T/empty.elf" "$T/empty.kdrv" "$T/empty.rec" --pci 1234:11e8 --caps mmio >/dev/null 2>&1; then
    echo "kdrvpack accepted an empty ELF"; exit 1
fi
truncate -s 8388609 "$T/oversize.elf"
if "$T/kdrvpack" invalid "$T/oversize.elf" "$T/large.kdrv" "$T/large.rec" --pci 1234:11e8 --caps mmio >/dev/null 2>&1; then
    echo "kdrvpack accepted an oversized ELF"; exit 1
fi
echo "kdrvpack: strict argument parsing, bounded input, output integrity and failure paths passed"
