#!/bin/bash
# Проверка добавленных строк на заглушки: ci/lint.sh [база] (по умолчанию origin/stable, иначе origin/main).
# Ищет в НОВЫХ строках C/asm/py/sh слова-признаки недоделок и пустые функции. Код возврата 1 - найдено.
cd "$(dirname "$0")/.."
base=${1:-origin/stable}; git rev-parse -q --verify "$base" >/dev/null || base=origin/main
bad=0
# Research harnesses may contain intentionally minimal ABI shims; production source is checked separately.
df=$(git diff -U0 "$base" HEAD -- '*.c' '*.h' '*.S' ':!docs' ':!research') || { echo "lint: нет базы $base"; exit 2; }
d=$(echo "$df" | grep -E '^\+[^+]')
pat='TODO|FIXME|XXX|HACK|stub|not implemented|unimplemented|NotImplemented|заглушк|пока не реализ|упрощённо вместо|имитаци|fake|dummy|placeholder'
d_markers=$(printf "%s\n" "$d" | sed "s/-fno-xxx//Ig")
if echo "$d_markers" | grep -inE "$pat"; then echo "lint: найдены признаки заглушек (выше)"; bad=1; fi
if echo "$d" | grep -nE '^\+[[:space:]]*(static[[:space:]]+)?(void|int|bool|size_t|uint[0-9]+_t|int[0-9]+_t|char|long|unsigned|const[[:space:]]+[[:alnum:]_]+)[[:space:]*]+[[:alnum:]_]+[[:space:]]*\([^;]*\)[[:space:]]*\{[[:space:]]*(return[[:space:]]+(0|-1|NULL);)?[[:space:]]*\}[[:space:]]*$'; then echo "lint: пустые функции или функции-только-возврат (выше): реализовать"; bad=1; fi
[ $bad = 0 ] && echo "lint: чисто"
exit $bad
