#!/bin/bash
# Rust (no_std) -> ELF64 для Kira OS (x86-64, Ring 3): userland/rust/NAME.rs -> userland/NAME.elf.
# Стандартная библиотека не используется, только core. Компонуем тем же user.ld, что и C-программы.
# Цель выбирается по тому, для чего у rustc есть core:
#   x86_64-unknown-none        - правильная цель (rustup target add x86_64-unknown-none);
#   x86_64-unknown-linux-gnu   - хост x86-64 без добавленных целей (ядро Киры не Linux, но core от этого не зависит);
#   иначе (например, Debian на телефоне ARM64 без целей x86-64) - код выхода 3, программы не собираются.
set -e
cd "$(dirname "$0")/../.."
SYSROOT=$(rustc --print sysroot)
TARGET=""
for t in x86_64-unknown-none x86_64-unknown-linux-gnu; do
    if ls "$SYSROOT/lib/rustlib/$t/lib/"libcore-*.rlib >/dev/null 2>&1; then TARGET=$t; break; fi
done
if [ -z "$TARGET" ]; then
    echo "[!] у rustc нет core для x86-64 (хост $(rustc -vV | sed -n 's/^host: //p')): Rust-программы не собраны." >&2
    echo "    Нужно: rustup target add x86_64-unknown-none (в Debian без rustup - пакет libstd-rust-dev с целью x86-64 отсутствует, шаг пропускается)." >&2
    exit 3
fi
for src in userland/rust/*.rs; do
    n=$(basename "$src" .rs)
    rustc --edition 2021 --target "$TARGET" --crate-type bin -C panic=abort -C opt-level=2 -C relocation-model=pic -C code-model=small -C strip=symbols \
        -C linker=ld.lld -C linker-flavor=ld.lld \
        -C link-arg=-T -C link-arg=userland/user.ld -C link-arg=-z -C link-arg=max-page-size=4096 \
        -C link-arg=-z -C link-arg=noseparate-code -C link-arg=-z -C link-arg=norelro -C link-arg=-nostdlib \
        -C link-arg=--no-pie -C link-arg=--gc-sections -C link-arg=-e -C link-arg=_start \
        -o userland/$n.elf "$src"
done
echo "[*] Rust ($TARGET): $(ls userland/rust/*.rs | wc -l) программ(ы)"
