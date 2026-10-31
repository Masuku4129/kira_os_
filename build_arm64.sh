#!/bin/bash
# Сборка Kira OS для ARM64 (первый этап переноса: платформа - дерево устройств, MMU, GIC, таймер, ядра по PSCI).
#   ./build_arm64.sh           собрать kira-arm64.img (образ ARM64: QEMU -kernel, U-Boot booti) и kira-arm64.elf (символы)
#   ./build_arm64.sh --run     собрать и запустить в QEMU virt (4 ядра, GICv3)
set -e
cd "$(dirname "$0")"
A=arch/aarch64
CC="clang --target=aarch64-unknown-none-elf"
CFLAGS="$KIRA_ARM64_EXTRA -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align -fpie -fno-stack-protector -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -I$A -I."
mkdir -p build-arm64
OBJ=""
for f in $A/boot.S $A/mem.S $A/context.S $A/fpu.S; do
    o=build-arm64/$(basename $f .S)_s.o; $CC $CFLAGS -c $f -o $o; OBJ="$OBJ $o"
done
# Общее ядро (те же файлы, что и на x86-64) и платформа ARM64
for f in fdt.c percpu.c sched.c scale.c kmutex.c heap.c pci.c virtio.c virtio_blk.c blk.c nvme.c ahci.c kfs.c store.c sha256.c \
         experience.c fscmd.c vfs.c fat.c part.c life.c mind.c mindnet.c mind_sys.c devhealth.c klog.c linebuf.c kpar.c clock.c \
         immune.c kobj.c futex.c watchdog.c \
         $A/main.c $A/pmm_arm.c $A/selftest_arm.c $A/notyet.c; do
    o=build-arm64/$(basename $f .c).o; $CC $CFLAGS -c $f -o $o; OBJ="$OBJ $o"
done
ld.lld -T $A/linker.ld -nostdlib -pie --no-dynamic-linker -z text -z norelro -z max-page-size=4096 $OBJ -o kira-arm64.elf
llvm-objcopy -O binary kira-arm64.elf kira-arm64.img
echo "[*] kira-arm64.img (образ ARM64 с заголовком загрузки): $(stat -c %s kira-arm64.img) байт"
# Диск с KiraFS (тот же формат, что и на x86-64; программ для ARM64 на нём пока нет)
HOSTCC="${HOSTCC:-clang}"
[ -x tools/kfstool ] || $HOSTCC -O1 -Wall -DKFS_NO_THREADS -I. -Iarch/x86_64 -o tools/kfstool tools/kfstool.c kfs.c store.c sha256.c
if [ ! -f kira-arm64.disk ]; then
    dd if=/dev/zero of=kira-arm64.disk bs=1M count=16 2>/dev/null
    tools/kfstool kira-arm64.disk init >/dev/null
fi
if [ "$1" = "--run" ]; then
    exec qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -smp 4 -m 1G -nographic -kernel kira-arm64.img -drive file=kira-arm64.disk,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0
fi
