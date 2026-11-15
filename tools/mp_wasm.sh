#!/bin/bash
# Собирает Python (MicroPython) как модуль WebAssembly для Киры: build-wasm/python.wasm.
# Исходники MicroPython берутся с github.com по закреплённому коммиту (tools/mpy_kira/pins.txt), libc - tools/wasi_sysroot.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/mpy_kira/pins.txt
export KIRA_WASI=${KIRA_WASI:-$PWD/build-wasm/wasi}
MPY=${KIRA_MPY:-$PWD/build-wasm/mpy}
tools/wasi_sysroot.sh
mkdir -p "$MPY"
if [ "$(git -C "$MPY/src" rev-parse HEAD 2>/dev/null || true)" != "$MPY_REV" ]; then
    [ -d "$MPY/src/.git" ] || { git init -q "$MPY/src"; git -C "$MPY/src" remote add origin https://github.com/micropython/micropython; }
    git -C "$MPY/src" fetch -q --depth 1 origin "$MPY_REV"
    git -C "$MPY/src" checkout -q FETCH_HEAD
fi
[ "$(git -C "$MPY/src" rev-parse HEAD)" = "$MPY_REV" ] || { echo "коммит MicroPython не совпал с закреплённым"; exit 1; }
# внешние модули MicroPython: нужны сабмодули не все; для выбранных модулей хватает самого репозитория (без lib/)
make -C tools/mpy_kira MPY_TOP="$MPY/src" BUILD="$MPY/build" -j"$(nproc)" 2>&1 | tail -${MP_LOG:-5}
cp "$MPY/build/python.wasm" build-wasm/python.wasm
ls -l build-wasm/python.wasm
