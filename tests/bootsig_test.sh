#!/bin/bash
# Хост-тест подписанного манифеста загрузки: ключи и подпись делает tools/kirasig.py (openssl), проверяет bootsig.c.
# Ключи живут во временном каталоге и в git не попадают. Аргументы: CC FLAGS-строка не нужна, берутся из окружения run_host_tests.sh.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 tools/kirasig.py keygen "$T/rel.pem" "$T/rel.pub" >/dev/null
python3 tools/kirasig.py keygen "$T/other.pem" "$T/other.pub" >/dev/null
head -c 3000 /dev/urandom > "$T/KIRA.ELF"
head -c 700 /dev/urandom > "$T/INITRD.IMG"
python3 tools/kirasig.py sign --key "$T/rel.pem" --version 7 --out "$T/KIRA.SIG" "$T/KIRA.ELF" "$T/INITRD.IMG"
python3 tools/kirasig.py verify --pub "$T/rel.pub" "$T/KIRA.SIG" "$T/KIRA.ELF" "$T/INITRD.IMG"
# инструмент тоже обязан отклонять откат и подмену
if python3 tools/kirasig.py verify --pub "$T/rel.pub" --min-version 8 "$T/KIRA.SIG" "$T/KIRA.ELF" "$T/INITRD.IMG" 2>/dev/null; then echo "FAIL: откат не отклонён инструментом"; exit 1; fi
cp "$T/KIRA.ELF" "$T/bad.elf"; printf '\x55' | dd of="$T/bad.elf" bs=1 seek=1500 conv=notrunc 2>/dev/null
if python3 tools/kirasig.py verify --pub "$T/rel.pub" "$T/KIRA.SIG" "$T/bad.elf" "$T/INITRD.IMG" 2>/dev/null; then echo "FAIL: подмена ядра не отклонена инструментом"; exit 1; fi
$CC -O1 -g -Wall -Werror -I. -fsanitize=address,undefined -o "$T/bootsig" tests/bootsig_test.c bootsig.c pk.c crypto.c sha256.c
"$T/bootsig" "$T/rel.pub" "$T/KIRA.SIG" "$T/KIRA.ELF" "$T/INITRD.IMG" "$T/other.pub"
