#!/bin/bash
# Хеш исходников Python (MicroPython) и ksh под WebAssembly: по нему видно, что prebuilt/python.wasm и prebuilt/ksh.wasm собраны из ТЕКУЩИХ исходников.
cd "$(dirname "$0")/.."
cat tools/mpy_kira/* tools/mpy_kira/include/* tools/mp_wasm.sh tools/wasi_sysroot.sh tools/ksh_wasm.sh userland/wasm/ksh.c 2>/dev/null | sha256sum | cut -d' ' -f1
