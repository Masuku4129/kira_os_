#!/bin/bash
# Запасной путь: если python.wasm или ksh.wasm не собрались (на телефоне нет github.com, cmake или sysroot), берётся готовый файл из prebuilt/,
# но только если он собран из ТЕКУЩИХ исходников (хеш в prebuilt/WASM.sha256) и сам не изменён. Иначе возврат 1 и прежнее предупреждение.
# WebAssembly не зависит от архитектуры, поэтому один файл годится для x86-64 и ARM64.
cd "$(dirname "$0")/.."
name="$1"; f="prebuilt/$name.wasm"
[ -s "$f" ] && [ -s prebuilt/WASM.sha256 ] || exit 1
[ "$(awk '$1=="src"{print $2}' prebuilt/WASM.sha256)" = "$(tools/wasm_srchash.sh)" ] || { echo "[!] prebuilt/$name.wasm устарел (исходники изменились)"; exit 1; }
[ "$(awk -v n="$name" '$1==n{print $2}' prebuilt/WASM.sha256)" = "$(sha256sum "$f" | cut -d' ' -f1)" ] || { echo "[!] prebuilt/$name.wasm изменён"; exit 1; }
mkdir -p build-wasm && cp "$f" "build-wasm/$name.wasm" && echo "[*] $name: готовый prebuilt/$name.wasm (исходники не менялись)"
