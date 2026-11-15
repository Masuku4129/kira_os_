#!/bin/bash
# Модули WebAssembly (один файл для x86-64 и ARM64) -> build-wasm/*.wasm. Нужны clang и wasm-ld (пакет lld).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-wasm
WF="--target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Iuserland/wasm"
clang $WF -c userland/wasm/kwlib.c -o build-wasm/kwlib.o
for m in wtest ftest wgfx wmix; do
    mem=262144; [ $m = wgfx ] && mem=524288                      # кадр 320x200 - 250 КБ
    clang $WF -c userland/wasm/$m.c -o build-wasm/$m.o
    wasm-ld --no-entry --export=kmain -z stack-size=65536 --initial-memory=$mem build-wasm/$m.o build-wasm/kwlib.o -o build-wasm/$m.wasm
done
# проверка WASI 0.1 (точка входа _start, как у модулей wasm32-wasi)
clang $WF -c userland/wasm/wasitest.c -o build-wasm/wasitest.o
wasm-ld --no-entry --export=_start -z stack-size=65536 --initial-memory=262144 build-wasm/wasitest.o -o build-wasm/wasitest.wasm
clang $WF -c userland/wasm/wasifs.c -o build-wasm/wasifs.o
wasm-ld --no-entry --export=_start -z stack-size=65536 --initial-memory=262144 build-wasm/wasifs.o -o build-wasm/wasifs.wasm
# тот же wmix без оптимизации: много local.tee/local.set и пересылок через память - другие формы кода для предкомпилятора
clang $WF -O0 -c userland/wasm/wmix.c -o build-wasm/wmix0.o
wasm-ld --no-entry --export=kmain -z stack-size=65536 --initial-memory=262144 build-wasm/wmix0.o build-wasm/kwlib.o -o build-wasm/wmix0.wasm
# Python (MicroPython) под WASI: build-wasm/python.wasm. Нужны cmake, ninja, git и доступ к github.com (исходники по закреплённым коммитам, tools/mpy_kira/pins.txt).
# KIRA_NO_PYTHON=1 - пропустить. Без инструментов образ собирается без python (команда python скажет об этом).
if [ "${KIRA_NO_PYTHON:-0}" != 1 ]; then
    tools/mp_wasm.sh || echo "[ПРЕДУПРЕЖДЕНИЕ] Python не собран (нужны cmake, ninja, git, clang с wasm32, lld и github.com); образ будет без /wasm/python.wasm"
fi
