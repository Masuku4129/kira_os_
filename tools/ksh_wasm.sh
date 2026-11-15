#!/bin/bash
# Собирает оболочку ksh как модуль WebAssembly для Киры: build-wasm/ksh.wasm (userland/wasm/ksh.c, wasm32-wasip1, libc - wasi-libc).
# Sysroot (wasi-libc и compiler-rt builtins) - tools/wasi_sysroot.sh: $KIRA_WASI/sysroot (по умолчанию build-wasm/wasi), общий с Python.
set -euo pipefail
cd "$(dirname "$0")/.."
export KIRA_WASI=${KIRA_WASI:-$PWD/build-wasm/wasi}
tools/wasi_sysroot.sh
SYSROOT=$KIRA_WASI/sysroot
mkdir -p build-wasm
clang --target=wasm32-wasip1 --sysroot="$SYSROOT" -std=gnu99 -Os -Wall -Wno-unused-parameter -fno-strict-aliasing \
    -c userland/wasm/ksh.c -o build-wasm/ksh.o
wasm-ld -z stack-size=65536 --initial-memory=2097152 --gc-sections --no-entry --export=_start -L"$SYSROOT/lib/wasm32-wasip1" \
    -o build-wasm/ksh.wasm "$SYSROOT/lib/wasm32-wasip1/crt1-command.o" build-wasm/ksh.o -lc "$SYSROOT/lib/clang_rt/libclang_rt.builtins-wasm32.a"
ls -l build-wasm/ksh.wasm
