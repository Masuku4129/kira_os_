#!/bin/bash
# Проверка добавленных строк на заглушки: ci/lint.sh [база] (по умолчанию origin/stable, иначе origin/main).
# Ищет в НОВЫХ строках C/asm/py/sh слова-признаки недоделок и пустые функции. Код возврата 1 - найдено.
cd "$(dirname "$0")/.."
base=${1:-origin/stable}; git rev-parse -q --verify "$base" >/dev/null || base=origin/main
bad=0
df=$(git diff -U0 "$base" HEAD -- '*.c' '*.h' '*.S' '*.py' '*.sh' ':!ci/lint.sh' ':!docs') || { echo "lint: нет базы $base"; exit 2; }
d=$(echo "$df" | grep -E '^\+[^+]')
pat='TODO|FIXME|XXX|HACK|stub|not implemented|unimplemented|NotImplemented|заглушк|пока не реализ|упрощённо вместо|имитаци|fake|dummy|placeholder'
if echo "$d" | grep -inE "$pat"; then echo "lint: найдены признаки заглушек (выше)"; bad=1; fi
if echo "$d" | grep -nE '\)\s*\{\s*(return\s+(0|-1|NULL);)?\s*\}\s*$'; then echo "lint: пустые функции или функции-только-возврат (выше): оправдать в отчёте или реализовать"; bad=1; fi
[ $bad = 0 ] && echo "lint: чисто"
exit $bad
