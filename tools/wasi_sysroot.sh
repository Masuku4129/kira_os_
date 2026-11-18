#!/bin/bash
# Собирает sysroot для wasm32-wasi: wasi-libc и встроенные функции компилятора (compiler-rt builtins) из исходников с закреплёнными коммитами.
# Нужны: clang 18 с целью wasm32, wasm-ld, llvm-ar, cmake, ninja, git, доступ к github.com (git clone работает, архивы codeload - нет).
# Результат: $KIRA_WASI/sysroot (по умолчанию build-wasm/wasi). Исходники в репозиторий не попадают.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${KIRA_WASI:-$PWD/build-wasm/wasi}
. tools/mpy_kira/pins.txt   # WASI_LIBC_REV, LLVM_REV
mkdir -p "$OUT"
if [ -f "$OUT/sysroot/lib/clang_rt/libclang_rt.builtins-wasm32.a" ] && [ -f "$OUT/sysroot/lib/wasm32-wasip1/libc.a" ]; then echo "wasi sysroot готов: $OUT/sysroot"; exit 0; fi
fetch() {   # каталог url коммит [sparse-пути...]
    local d=$1 url=$2 rev=$3; shift 3
    if [ ! -d "$d/.git" ]; then git init -q "$d"; git -C "$d" remote add origin "$url"; fi
    if [ "$(git -C "$d" rev-parse HEAD 2>/dev/null || true)" != "$rev" ]; then
        if [ $# -gt 0 ]; then git -C "$d" sparse-checkout init --cone; git -C "$d" sparse-checkout set "$@"; fi
        git -C "$d" fetch -q --depth 1 --filter=blob:none origin "$rev"
        git -C "$d" checkout -q FETCH_HEAD
    fi
    [ "$(git -C "$d" rev-parse HEAD)" = "$rev" ] || { echo "коммит $d не совпал с закреплённым"; exit 1; }
}
fetch "$OUT/src/wasi-libc" https://github.com/WebAssembly/wasi-libc "$WASI_LIBC_REV"
fetch "$OUT/src/llvm-project" https://github.com/llvm/llvm-project "$LLVM_REV" compiler-rt/lib/builtins compiler-rt/cmake cmake llvm/cmake
cmake -S "$OUT/src/wasi-libc" -B "$OUT/build-libc" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_AR=/usr/bin/llvm-ar -DCMAKE_NM=/usr/bin/llvm-nm \
    -DCMAKE_INSTALL_PREFIX="$OUT/sysroot" -DTARGET_TRIPLE=wasm32-wasip1 -DMALLOC=dlmalloc -Wno-dev >/dev/null
cmake --build "$OUT/build-libc" >/dev/null
cmake --install "$OUT/build-libc" >/dev/null
cmake -S "$OUT/src/llvm-project/compiler-rt/lib/builtins" -B "$OUT/build-rt" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_AR=/usr/bin/llvm-ar \
    -DCMAKE_NM=/usr/bin/llvm-nm -DCMAKE_RANLIB=/usr/bin/llvm-ranlib -DCMAKE_C_COMPILER_TARGET=wasm32-wasip1 -DCMAKE_C_FLAGS="--sysroot=$OUT/sysroot" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCOMPILER_RT_BAREMETAL_BUILD=ON -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCMAKE_SYSTEM_NAME=Generic \
    -DCMAKE_ASM_COMPILER_TARGET=wasm32-wasip1 -DLLVM_CMAKE_DIR="$OUT/src/llvm-project/llvm/cmake" -Wno-dev >/dev/null
cmake --build "$OUT/build-rt" >/dev/null
mkdir -p "$OUT/sysroot/lib/clang_rt"
cp "$OUT/build-rt/lib/generic/libclang_rt.builtins-wasm32.a" "$OUT/sysroot/lib/clang_rt/"
echo "wasi sysroot: $OUT/sysroot"
