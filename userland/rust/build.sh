#!/bin/bash
# Rust (no_std) -> ELF64 для Kira OS (x86-64, Ring 3): userland/rust/NAME.rs -> userland/NAME.elf.
# Нужен rustc (rustup); стандартная библиотека не используется, только core. Ссылаем тем же скриптом user.ld, что и C-программы.
set -e
cd "$(dirname "$0")/../.."
for src in userland/rust/*.rs; do
    n=$(basename "$src" .rs)
    rustc --edition 2021 --crate-type bin -C panic=abort -C opt-level=2 -C relocation-model=pic -C code-model=small -C strip=symbols \
        -C linker=ld.lld -C linker-flavor=ld.lld \
        -C link-arg=-T -C link-arg=userland/user.ld -C link-arg=-z -C link-arg=max-page-size=4096 \
        -C link-arg=-z -C link-arg=noseparate-code -C link-arg=-z -C link-arg=norelro -C link-arg=-nostdlib \
        -C link-arg=--no-pie -C link-arg=--gc-sections -C link-arg=-e -C link-arg=_start \
        -o userland/$n.elf "$src"
done
