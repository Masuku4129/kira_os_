#!/bin/bash
# Проверка добавленных строк на признаки заглушек и пустые функции.
# ci/lint.sh [база] (по умолчанию origin/stable, иначе origin/main).
cd "$(dirname "$0")/.."
base=${1:-origin/stable}
git rev-parse -q --verify "$base" >/dev/null || base=origin/main
git rev-parse -q --verify "$base" >/dev/null || { echo "lint: нет базы $base"; exit 2; }

pat='TODO|FIXME|XXX|HACK|stub|not implemented|unimplemented|NotImplemented|заглушк|пока не реализ|упрощённо вместо|имитаци|fake|dummy|placeholder'
bad=0

while IFS= read -r file; do
    case "$file" in
        *.c|*.h|*.S|*.py|*.sh) ;;
        *) continue ;;
    esac
    [ "$file" = "ci/lint.sh" ] && continue
    matches=$(git diff -U0 "$base" HEAD -- "$file" | grep -E '^\+[^+]' | grep -inE "$pat" || true)
    if [ -n "$matches" ]; then
        printf '%s\n' "$matches" | sed "s|^|$file:|"
        bad=1
    fi
    matches=$(git diff -U0 "$base" HEAD -- "$file" | grep -E '^\+[^+]' | grep -nE '\)\s*\{\s*(return\s+(0|-1|NULL);)?\s*\}\s*$' || true)
    if [ -n "$matches" ]; then
        printf '%s\n' "$matches" | sed "s|^|$file:|"
        bad=1
    fi
done < <(git diff --name-only "$base" HEAD)

if [ "$bad" -eq 0 ]; then
    echo "lint: чисто"
    exit 0
fi
echo "lint: найдены признаки заглушек или пустые функции"
exit 1
