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
# тот же wmix без оптимизации: много local.tee/local.set и пересылок через память - другие формы кода для предкомпилятора
clang $WF -O0 -c userland/wasm/wmix.c -o build-wasm/wmix0.o
wasm-ld --no-entry --export=kmain -z stack-size=65536 --initial-memory=262144 build-wasm/wmix0.o build-wasm/kwlib.o -o build-wasm/wmix0.wasm
