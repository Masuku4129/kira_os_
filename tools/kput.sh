#!/bin/sh
# kput.sh образ /путь файл: положить файл в образ KiraFS сжатым кадром Zstd (прозрачно для чтения), без python-zstandard или если сжатие не выгодно - как есть.
img="$1"; dst="$2"; src="$3"
t=$(mktemp) || exit 1
trap 'rm -f "$t"' EXIT
if python3 "$(dirname "$0")/zstd_pack.py" "$src" "$t" 2>/dev/null && [ "$(wc -c < "$t")" -lt "$(( $(wc -c < "$src") * 9 / 10 ))" ]; then
    "$(dirname "$0")/kfstool" "$img" putz "$dst" "$t"
else
    "$(dirname "$0")/kfstool" "$img" put "$dst" "$src"
fi
