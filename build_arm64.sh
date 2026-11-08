#!/bin/bash
# Сборка Kira OS для ARM64 (первый этап переноса: платформа - дерево устройств, MMU, GIC, таймер, ядра по PSCI).
#   ./build_arm64.sh           собрать kira-arm64.img (образ ARM64: QEMU -kernel, U-Boot booti) и kira-arm64.elf (символы)
#   ./build_arm64.sh --run     собрать и запустить в QEMU virt (4 ядра, GICv3)
set -e
cd "$(dirname "$0")"
A=arch/aarch64
CC="clang --target=aarch64-unknown-none-elf"
CFLAGS="$KIRA_ARM64_EXTRA -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align -fpie -fno-stack-protector -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -Ibuild-arm64 -I$A -I."
mkdir -p build-arm64
# Программы для ARM64 (EL0): те же исходники, что и для x86-64; ядро их не содержит - они лежат на диске в /bin
HOSTCC="${HOSTCC:-clang}"
[ -x tools/kfstool ] || $HOSTCC -O1 -Wall -DKFS_NO_THREADS -I. -Iarch/x86_64 -o tools/kfstool tools/kfstool.c kfs.c store.c sha256.c
UCFLAGS="-ffreestanding -fno-builtin -fno-math-errno -fPIE -fvisibility=hidden -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra"
UELF=""
for app in mt ipc burn plasma cube3d paint fpu demo atk-mem atk-priv atk-sys atk-jmp atk-cap serve melody wasm udpecho rec; do   # fpu проверяет x87 (long double): на ARM64 - отдельная проверка позже
    $CC $UCFLAGS -c userland/$app.c -o build-arm64/u_$app.o
    ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/$app.elf build-arm64/u_$app.o
    UELF="$UELF build-arm64/$app.elf"
done
# fetch: HTTP и HTTPS (TLS 1.3 и криптография внутри программы)
for f in userland/fetch.c userland/tls.c userland/x509.c pk.c crypto.c sha256.c userland/libc_min.c; do
    $CC $UCFLAGS -I. -Iuserland -c $f -o build-arm64/uf_$(basename $f .c).o
done
ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/fetch.elf build-arm64/uf_*.o
UELF="$UELF build-arm64/fetch.elf"
$CC $UCFLAGS -I. -Iuserland -c userland/net6.c -o build-arm64/un6_net6.o       # сокеты IPv6; адрес разбирает ip6.c
$CC $UCFLAGS -I. -c ip6.c -o build-arm64/un6_ip6.o
ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/net6.elf build-arm64/un6_*.o
UELF="$UELF build-arm64/net6.elf"
# kcc: компилятор C как программа EL0 (те же kcc/*.c без main.c и userland/kcc.c, что у x86-64; мини-libc - userland/kccrt.c). Целевая архитектура
# программ, которые он собирает, - ARM64 (userland/kcc.c). Без вещественных регистров: setjmp/longjmp их не сохраняют.
KCC_OBJ=""
KCFLAGS="$UCFLAGS -mgeneral-regs-only -nostdlibinc"
for src in kcc/*.c; do
    [ "$src" = kcc/main.c ] && continue
    $CC $KCFLAGS -Iuserland/kccrt -c $src -o build-arm64/kcc_$(basename $src .c).o
    KCC_OBJ="$KCC_OBJ build-arm64/kcc_$(basename $src .c).o"
done
$CC $KCFLAGS -Iuserland/kccrt -Iuserland -c userland/kcc.c -o build-arm64/kcc_main.o
$CC $KCFLAGS -Iuserland -c userland/kccrt.c -o build-arm64/ukccrt.o
$CC $UCFLAGS -c userland/libc_min.c -o build-arm64/ukcc_libc_min.o
ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/kcc.elf build-arm64/kcc_main.o $KCC_OBJ build-arm64/ukccrt.o build-arm64/ukcc_libc_min.o
UELF="$UELF build-arm64/kcc.elf"
# drvget (см. build_clang.sh): TLS-клиент, kcc и запись пакетов в /drv в одной программе EL0
$CC $KCFLAGS -I. -Iuserland -Iuserland/kccrt -c userland/drvget.c -o build-arm64/dg_main.o
$CC $UCFLAGS -I. -c userland/drvget_core.c -o build-arm64/dg_core.o
$CC $UCFLAGS -I. -c kdrv_fmt.c -o build-arm64/dg_kdrv_fmt.o
DG_TLS=""
for f in userland/tls.c userland/x509.c pk.c crypto.c sha256.c kdrv_sig.c; do
    $CC $UCFLAGS -I. -Iuserland -c $f -o build-arm64/dg_$(basename $f .c).o; DG_TLS="$DG_TLS build-arm64/dg_$(basename $f .c).o"
done
ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/drvget.elf build-arm64/dg_main.o build-arm64/dg_core.o build-arm64/dg_kdrv_fmt.o \
    $KCC_OBJ build-arm64/ukccrt.o $DG_TLS build-arm64/ukcc_libc_min.o
UELF="$UELF build-arm64/drvget.elf"
# drvman: окно «Драйверы» (см. build_clang.sh): userland/drvget.c с -DDRVGET_LIB + окно и шрифт ядра
$CC $KCFLAGS -I. -Iuserland -Iuserland/kccrt -DDRVGET_LIB -c userland/drvget.c -o build-arm64/dm_drvget.o
$CC $KCFLAGS -I. -Iuserland -Iuserland/kccrt -c userland/drvman.c -o build-arm64/dm_main.o
$CC $UCFLAGS -I. -c font8x16.c -o build-arm64/dm_font.o
ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/drvman.elf build-arm64/dm_main.o build-arm64/dm_drvget.o build-arm64/dg_core.o build-arm64/dg_kdrv_fmt.o \
    build-arm64/dm_font.o $KCC_OBJ build-arm64/ukccrt.o $DG_TLS build-arm64/ukcc_libc_min.o
UELF="$UELF build-arm64/drvman.elf"
for app in edit view; do                 # с текстом: шрифт ядра вместе с программой
    $CC $UCFLAGS -I. -c userland/$app.c -o build-arm64/u_$app.o
    $CC $UCFLAGS -I. -c font8x16.c -o build-arm64/u_font_$app.o
    ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/$app.elf build-arm64/u_$app.o build-arm64/u_font_$app.o
    UELF="$UELF build-arm64/$app.elf"
done
tools/kfstool manifest $UELF > build-arm64/manifest.h        # доверенные программы ARM64 (SHA-256)
OBJ=""
for f in $A/boot.S $A/mem.S $A/context.S $A/fpu.S $A/uaccess.S; do
    o=build-arm64/$(basename $f .S)_s.o; $CC $CFLAGS -c $f -o $o; OBJ="$OBJ $o"
done
# Общее ядро (те же файлы, что и на x86-64) и платформа ARM64
for f in fdt.c bootinfo.c bootinfo_fdt.c percpu.c sched.c scale.c kmutex.c heap.c pci.c virtio.c virtio_blk.c blk.c nvme.c ahci.c kfs.c store.c sha256.c \
         experience.c fscmd.c vfs.c fat.c part.c life.c mind.c mindnet.c mind_sys.c devhealth.c klog.c linebuf.c kpar.c clock.c \
         immune.c kobj.c futex.c corefault.c crypto.c pk.c kdrv_sig.c kdrv_state.c rng.c virtio_rng.c $A/rng_arch.c watchdog.c proc.c caps.c syscall.c elf.c progs.c trust.c uheap.c userlog.c fsrv.c uaccess.c \
         virtio_gpu.c virtio_input.c e1000.c virtio_net.c net.c ip6.c ipfrag.c irqwait.c kdrv.c kdrv_fmt.c hda.c audio.c console.c fb.c display.c wm.c gfx2d.c gfxui.c gfxsrv.c fbcon.c font8x16.c apps.c explorer.c fm.c dock.c \
         icons.c brainfx.c input.c rtc.c usb.c usbhid.c usbhub.c usbmsc.c xhci.c \
         $A/main.c $A/wx_arm.c $A/fb_arm.c $A/pmm_arm.c $A/selftest_arm.c $A/vmm_user.c $A/uaccess_arch.c; do
    o=build-arm64/$(basename $f .c).o; $CC $CFLAGS -c $f -o $o; OBJ="$OBJ $o"
done
ld.lld -T $A/linker.ld -nostdlib -pie --no-dynamic-linker -z text -z norelro -z max-page-size=4096 $OBJ -o kira-arm64.elf
llvm-objcopy -O binary kira-arm64.elf kira-arm64.img
echo "[*] kira-arm64.img (образ ARM64 с заголовком загрузки): $(stat -c %s kira-arm64.img) байт"
# Диск с KiraFS (тот же формат, что и на x86-64): программы ARM64 в /bin обновляются, данные и опыт сохраняются
[ -f kira-arm64.disk ] || dd if=/dev/zero of=kira-arm64.disk bs=1M count=16 2>/dev/null
tools/kfstool kira-arm64.disk init >/dev/null
for e in $UELF; do tools/kfstool kira-arm64.disk put /bin/$(basename $e .elf) $e >/dev/null; done
for h in kcc/include/*.h; do                              # заголовки libc-lite для kcc на Кире (те же файлы, что у x86-64)
    tools/kfstool kira-arm64.disk put /etc/kcc/include/$(basename $h) $h >/dev/null
done
# Драйвер Ring 3 (тестовый, для QEMU -device edu) и его пакеты в /drv
$CC $UCFLAGS -I. -Iuserland -c userland/drvedu.c -o build-arm64/drvedu.o
ld.lld -T userland/user_arm64.ld -z max-page-size=4096 -z noseparate-code -z norelro -o build-arm64/drvedu.elf build-arm64/drvedu.o
tools/mkdrv.sh kira-arm64.disk build-arm64/drvedu.elf "$HOSTCC"
userland/wasm/build.sh                                   # модули WebAssembly - те же файлы, что у x86-64
for w in build-wasm/*.wasm; do tools/kfstool kira-arm64.disk put /wasm/$(basename $w) $w >/dev/null; done
# Корневые сертификаты для HTTPS: системная связка хозяина (PEM) -> /etc/ssl/roots.der на диске Киры
$HOSTCC -O1 -Wall -o tools/pem2der tools/pem2der.c
ROOTS_SRC=""
for b in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt ${PREFIX:-/nonexistent}/etc/tls/cert.pem /etc/ssl/cert.pem; do
    [ -f "$b" ] && ROOTS_SRC=$b && break
done
ROOTS_N=$([ -n "$ROOTS_SRC" ] && tools/pem2der "$ROOTS_SRC" tools/roots.der)
[ -n "$ROOTS_N" ] && echo "[*] Корневые сертификаты: $ROOTS_N из $ROOTS_SRC" || echo "[!] Связки корневых сертификатов нет (пакет ca-certificates): fetch https откажется проверять сервер"
[ -n "$ROOTS_N" ] && tools/kfstool kira-arm64.disk put /etc/ssl/roots.der tools/roots.der >/dev/null
tools/kfstool kira-arm64.disk put /etc/ssl/drivers.index drivers/index.txt >/dev/null
# Семя опыта Разума (см. build_clang.sh)
[ -f data/mind_seed.bin ] && tools/kfstool kira-arm64.disk put /etc/mind_seed.bin data/mind_seed.bin >/dev/null
if [ "$1" = "--run" ]; then
    exec qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -smp 4 -m 1G -serial mon:stdio -kernel kira-arm64.img -drive file=kira-arm64.disk,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0 \
        -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-tablet-pci
fi
