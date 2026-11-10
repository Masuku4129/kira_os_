#!/bin/bash
# Проверка производственного кода на добавленные заглушки и пустые функции.
# Тестовые фикстуры и исследовательские эксперименты имеют собственные тесты и не входят в release lint.
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
    case "$file" in
        tests/*|research/*|docs/*|ci/lint.sh) continue ;;
    esac
    added=$(git diff -U0 "$base" HEAD -- "$file" | grep -E '^\+[^+]' || true)
    matches=$(printf '%s\n' "$added" | grep -nE "$pat" || true)
    if [ -n "$matches" ]; then
        printf '%s\n' "$matches" | sed "s|^|$file:|"
        bad=1
    fi
    # Проверяем только объявления функций, а не пустые циклы/ветви ожидания.
    matches=$(printf '%s\n' "$added" | grep -nE '^\+[[:space:]]*(static[[:space:]]+)?([[:alnum:]_]+[[:space:]*]+)+[[:alnum:]_]+[[:space:]]*\([^;]*\)[[:space:]]*\{[[:space:]]*(return[[:space:]]+(0|-1|NULL);)?[[:space:]]*\}[[:space:]]*$' || true)
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
