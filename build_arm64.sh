#!/bin/bash
# Сборка Kira OS для ARM64 (первый этап переноса: платформа - дерево устройств, MMU, GIC, таймер, ядра по PSCI).
#   ./build_arm64.sh           собрать kira-arm64.img (образ ARM64: QEMU -kernel, U-Boot booti) и kira-arm64.elf (символы)
#   ./build_arm64.sh --run     собрать и запустить в QEMU virt (4 ядра, GICv3)
set -e
cd "$(dirname "$0")"
A=arch/aarch64
CC="clang --target=aarch64-unknown-none-elf"
CFLAGS="$KIRA_ARM64_EXTRA -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align -fpie -fno-stack-protector -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -I. -I$A"
mkdir -p build-arm64
$CC $CFLAGS -c $A/boot.S -o build-arm64/boot.o
$CC $CFLAGS -c $A/mem.S -o build-arm64/mem.o
$CC $CFLAGS -c $A/main.c -o build-arm64/main.o
$CC $CFLAGS -c fdt.c -o build-arm64/fdt.o
ld.lld -T $A/linker.ld -nostdlib -pie --no-dynamic-linker -z text -z max-page-size=4096 build-arm64/boot.o build-arm64/mem.o build-arm64/main.o build-arm64/fdt.o -o kira-arm64.elf
llvm-objcopy -O binary kira-arm64.elf kira-arm64.img
echo "[*] kira-arm64.img (образ ARM64 с заголовком загрузки): $(stat -c %s kira-arm64.img) байт"
if [ "$1" = "--run" ]; then
    exec qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -smp 4 -m 1G -nographic -kernel kira-arm64.img
fi
