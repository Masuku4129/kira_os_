#!/bin/sh
# Пересобрать тестовые таблицы настоящим iasl (apt install acpica-tools). Готовые .aml лежат в репозитории,
# поэтому хост-тесты iasl не требуют; этот скрипт нужен, когда меняется .asl.
# Использование: tests/data/acpi/build.sh [check]   (check - собрать во временную папку и сравнить с лежащими .aml)
set -e
cd "$(dirname "$0")"
command -v iasl >/dev/null || { echo "iasl не найден: apt install acpica-tools"; exit 2; }
OUT=.
[ "$1" = check ] && OUT=$(mktemp -d)
for f in *.asl; do
    iasl -p "$OUT/${f%.asl}" "$f" >/dev/null
    if [ "$1" = check ]; then cmp "$OUT/${f%.asl}.aml" "${f%.asl}.aml" || { echo "расходится: $f"; exit 1; }; fi
done
[ "$1" = check ] && echo "iasl: .aml совпадают с .asl"
exit 0
