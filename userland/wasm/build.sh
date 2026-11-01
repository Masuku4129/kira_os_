#!/bin/bash
# Модули WebAssembly (один файл для x86-64 и ARM64) -> build-wasm/*.wasm. Нужны clang и wasm-ld (пакет lld).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-wasm
WF="--target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Iuserland/wasm"
clang $WF -c userland/wasm/kwlib.c -o build-wasm/kwlib.o
for m in wtest ftest; do
    clang $WF -c userland/wasm/$m.c -o build-wasm/$m.o
    wasm-ld --no-entry --export=kmain -z stack-size=65536 --initial-memory=262144 build-wasm/$m.o build-wasm/kwlib.o -o build-wasm/$m.wasm
done
