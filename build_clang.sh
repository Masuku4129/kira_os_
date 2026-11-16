#!/bin/bash
# Сборка Kira OS через clang + ld.lld.
# Ничего, кроме clang, ld.lld, nasm и grub-mkrescue, не требуется.
#   ./build_clang.sh            собрать и запустить (только UART)
#   ./build_clang.sh --vnc      собрать и запустить с экраном по VNC (127.0.0.1:5900)
#   ./build_clang.sh --vnc --ata  то же, но диск как ATA (по умолчанию virtio-blk)
#   ./build_clang.sh --no-run   только собрать
#   ./build_clang.sh --vnc --xhci --hub --flash   собрать и запустить с USB-хабом и флешкой (см. run_qemu.sh)
#   ./build_clang.sh --android  собрать и положить kira.iso и kira_disk.img в Download телефона (для Limbo x86):
#                               диск в Download обновляется на месте - /bin новый, опыт (/var) и файлы программ (/data) сохраняются.
#                               Папку можно задать: KIRA_OUT=/путь ./build_clang.sh --android
set -e

CC="clang --target=x86_64-unknown-none-elf"
COMMON="-ffreestanding -fno-omit-frame-pointer -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -O2 -Wall -Wextra -Werror"
# Ядро собирается БЕЗ SSE/MMX: обработчики прерываний не трогают регистры xmm,
# поэтому прерванному потоку не нужно сохранять их при входе в прерывание.
CFLAGS="$COMMON -mno-mmx -mno-sse -mno-sse2 -I. -Iarch/x86_64"

# arch/x86_64/kernel_main.c owns the steady-state loop; arch/x86_64/kernel_boot.c owns x86 boot composition.
C_SOURCES="kernel/syscall.c services/syscall_router.c services/graphics_syscall.c services/filesystem_syscall.c services/network_syscall.c services/audio_syscall.c services/clock_syscall.c services/driver_syscall.c percpu.c mem.c sched.c console.c pmm.c vmm.c heap.c keyboard.c timer.c mind.c mindnet.c mind_sys.c rtc.c watchdog.c immune.c sha256.c uart.c clock.c klog.c gfx2d.c font8x16.c fb.c fbcon.c display.c wm.c input.c acpi.c proc.c caps.c elf.c trust.c life.c progs.c fsrv.c ata.c store.c experience.c gfxsrv.c gfxui.c icons.c brainfx.c apps.c explorer.c fm.c dock.c usbhid.c usb.c usbhub.c usbmsc.c uhci.c xhci.c kfs.c fat.c part.c vfs.c ahci.c nvme.c uaccess.c devhealth.c kpar.c fscmd.c device_model.c pci.c virtio.c virtio_blk.c virtio_gpu.c virtio_input.c e1000.c virtio_net.c net.c ip6.c ipfrag.c irqwait.c kdrv.c kdrv_fmt.c hda.c ac97.c sb16.c audio.c blk.c bootmap.c mbinfo.c earlycon.c crashdump.c uheap.c userlog.c hwprofile.c ramext.c kmutex.c linebuf.c corefault.c crypto.c rng.c virtio_rng.c scale.c kobj.c futex.c"
# Код, зависящий от процессора x86-64 (вход в ядро, прерывания, APIC, ядра, FPU, системные вызовы): arch/x86_64
ARCH=arch/x86_64
ARCH_SOURCES="$ARCH/kernel_main.c $ARCH/kernel_boot.c $ARCH/gdt.c $ARCH/idt.c $ARCH/pic.c $ARCH/apic.c $ARCH/ioapic.c $ARCH/irqmode.c $ARCH/fpu.c $ARCH/syscall_arch.c $ARCH/exceptions.c $ARCH/smp.c $ARCH/percpu_arch.c $ARCH/proc_arch.c $ARCH/uaccess_arch.c $ARCH/msi_arch.c $ARCH/power.c $ARCH/rng_arch.c"

echo "[*] Ассемблер..."
rm -f *.o arch/x86_64/*.o kernel/*.o services/*.o kira.bin
nasm -f elf64 $ARCH/boot.s -o boot.o
nasm -f elf64 $ARCH/interrupts.s -o interrupts.o
nasm -f elf64 $ARCH/context.s -o context.o
nasm -f elf64 $ARCH/ap_tramp.s -o ap_tramp.o
nasm -f elf64 $ARCH/uaccess.s -o uaccess_s.o

echo "[*] Userland (Ring 3): программы собираются в ELF и кладутся на диск, ядро их не содержит..."
UCFLAGS="-Ikcc/include -ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra"
ULINK="-T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro"
UELF=""
for app in plasma paint burn mt ipc serve melody udpecho rec atk-cap; do
    $CC $UCFLAGS -c userland/$app.c -o userland/$app.o
    ld.lld $ULINK -o userland/$app.elf userland/$app.o
    UELF="$UELF userland/$app.elf"
done
# fetch: HTTP и HTTPS - TLS 1.3 и криптография (crypto.c, sha256.c - те же файлы, что у ядра) внутри программы
$CC $UCFLAGS -I. -Iuserland -c userland/fetch.c -o userland/fetch.o
$CC $UCFLAGS -I. -Iuserland -c userland/tls.c -o userland/u_tls.o
$CC $UCFLAGS -I. -Iuserland -c userland/x509.c -o userland/u_x509.o
$CC $UCFLAGS -I. -c pk.c -o userland/u_pk.o
$CC $UCFLAGS -I. -c crypto.c -o userland/u_crypto.o
$CC $UCFLAGS -I. -c sha256.c -o userland/u_sha256.o
$CC $UCFLAGS -c userland/libc_min.c -o userland/u_libc_min.o
ld.lld $ULINK -o userland/fetch.elf userland/fetch.o userland/u_tls.o userland/u_x509.o userland/u_pk.o userland/u_crypto.o userland/u_sha256.o userland/u_libc_min.o
UELF="$UELF userland/fetch.elf"
# net6: сокеты IPv6; разбор адреса - ip6.c (тот же файл, что у ядра)
$CC $UCFLAGS -I. -Iuserland -c userland/net6.c -o userland/net6.o
$CC $UCFLAGS -I. -c ip6.c -o userland/u_ip6.o
ld.lld $ULINK -o userland/net6.elf userland/net6.o userland/u_ip6.o
UELF="$UELF userland/net6.elf"
# kcc: компилятор C как программа Ring 3 (kcc/*.c без main.c + userland/kcc.c). Мини-libc для него - userland/kccrt (заголовки) и
# userland/kccrt.c (строки, vsnprintf, qsort, setjmp); mem* - libc_min.c. Заголовки для программ пользователя (kcc/include) кладутся
# на диск Киры как /etc/kcc/include/*.h (см. ниже) - kcc ищет их там.
KCC_OBJ=""
for src in kcc/*.c; do
    [ "$src" = kcc/main.c ] && continue
    $CC $UCFLAGS -nostdlibinc -Iuserland/kccrt -c $src -o userland/kcc_$(basename $src .c).o
    KCC_OBJ="$KCC_OBJ userland/kcc_$(basename $src .c).o"
done
$CC $UCFLAGS -nostdlibinc -Iuserland/kccrt -Iuserland -c userland/kcc.c -o userland/kcc.o
$CC $UCFLAGS -nostdlibinc -Iuserland -c userland/kccrt.c -o userland/u_kccrt.o
ld.lld $ULINK -o userland/kcc.elf userland/kcc.o $KCC_OBJ userland/u_kccrt.o userland/u_libc_min.o
UELF="$UELF userland/kcc.elf"
# drvget: установка драйверов из исходников на устройстве (TLS-клиент + kcc встроены в программу; запись в /drv - FS_OP_DRV_*, только drvget)
$CC $UCFLAGS -nostdlibinc -I. -Iuserland -Iuserland/kccrt -c userland/drvget.c -o userland/drvget.o
$CC $UCFLAGS -I. -c userland/drvget_core.c -o userland/u_drvget_core.o
$CC $UCFLAGS -I. -c kdrv_fmt.c -o userland/u_kdrv_fmt.o
ld.lld $ULINK -o userland/drvget.elf userland/drvget.o userland/u_drvget_core.o userland/u_kdrv_fmt.o $KCC_OBJ userland/u_kccrt.o \
    userland/u_tls.o userland/u_x509.o userland/u_pk.o userland/u_crypto.o userland/u_sha256.o userland/u_libc_min.o
UELF="$UELF userland/drvget.elf"
# drvman: окно «Драйверы» (/bin/drvman). Та же логика, что у drvget (userland/drvget.c с -DDRVGET_LIB), плюс окно и шрифт ядра
$CC $UCFLAGS -nostdlibinc -I. -Iuserland -Iuserland/kccrt -DDRVGET_LIB -c userland/drvget.c -o userland/drvget_lib.o
$CC $UCFLAGS -nostdlibinc -I. -Iuserland -Iuserland/kccrt -c userland/drvman.c -o userland/drvman.o
$CC $UCFLAGS -I. -c font8x16.c -o userland/font_drvman.o
ld.lld $ULINK -o userland/drvman.elf userland/drvman.o userland/drvget_lib.o userland/u_drvget_core.o userland/u_kdrv_fmt.o userland/font_drvman.o $KCC_OBJ userland/u_kccrt.o \
    userland/u_tls.o userland/u_x509.o userland/u_pk.o userland/u_crypto.o userland/u_sha256.o userland/u_libc_min.o
UELF="$UELF userland/drvman.elf"
# Программы с вещественной арифметикой (3D, тест FPU): SSE2 разрешён только им, ядро собирается без SSE.
USSE_FLAGS="-Ikcc/include -ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -msse2 -mfpmath=sse -mno-mmx -fno-math-errno -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra"
for app in cube3d fpu wasm; do
    $CC $USSE_FLAGS -c userland/$app.c -o userland/$app.o
    ld.lld $ULINK -o userland/$app.elf userland/$app.o
    UELF="$UELF userland/$app.elf"
done
# Программы с текстом: шрифт ядра (font8x16.c) собирается вместе с программой
for app in edit view; do
    $CC $UCFLAGS -I. -c userland/$app.c -o userland/$app.o
    $CC $UCFLAGS -I. -c font8x16.c -o userland/font_$app.o
    ld.lld $ULINK -o userland/$app.elf userland/$app.o userland/font_$app.o
    UELF="$UELF userland/$app.elf"
done
for app in demo atk-mem atk-priv atk-sys atk-gs; do
    nasm -f elf64 userland/$app.s -o userland/$app.o
    ld.lld $ULINK -o userland/$app.elf userland/$app.o
    UELF="$UELF userland/$app.elf"
done

# Пользовательская сборка (--android или KIRA_RELEASE=1): тестовые и учебные программы на диск не кладутся (CI собирает полный набор).
TESTPROGS="burn mt ipc udpecho melody demo fpu atk-cap atk-mem atk-priv atk-sys atk-gs atk-jmp"
RELEASE=0; case " $* " in *" --android "*) RELEASE=1 ;; esac; [ -n "$KIRA_RELEASE" ] && RELEASE=1
if [ "$RELEASE" = 1 ]; then
    KEEP=""; for e in $UELF; do n=$(basename $e .elf); case " $TESTPROGS " in *" $n "*) ;; *) KEEP="$KEEP $e" ;; esac; done
    UELF="$KEEP"
fi

echo "[*] Инструмент kfstool, манифест доверенных программ и установка в образ диска..."
HOSTCC="${HOSTCC:-clang}"
$HOSTCC -O1 -Wall -DKFS_NO_THREADS -I. -Iarch/x86_64 -o tools/kfstool tools/kfstool.c kfs.c store.c sha256.c
tools/kfstool manifest $UELF > manifest.h
rm -f kira.disk; dd if=/dev/zero of=kira.disk bs=1M count=8 2>/dev/null     # образ каждый раз новый: старые файлы не остаются
tools/kfstool kira.disk init
for e in $UELF; do
    tools/kfstool kira.disk put /bin/$(basename $e .elf) $e
done
# Драйвер Ring 3 (тестовый, для QEMU -device edu) и его пакеты в /drv (kdrv_format.h, docs/DRIVERS.md)
$CC $UCFLAGS -I. -Iuserland -c userland/drvedu.c -o userland/drvedu.o
ld.lld $ULINK -o userland/drvedu.elf userland/drvedu.o
tools/mkdrv.sh kira.disk userland/drvedu.elf "$HOSTCC"
for h in kcc/include/*.h; do
    tools/kfstool kira.disk put /etc/kcc/include/$(basename $h) $h     # заголовки libc-lite для kcc на Кире
done
userland/wasm/build.sh                                   # модули WebAssembly - в /wasm (те же файлы, что у ARM64)
for w in build-wasm/*.wasm; do tools/kfstool kira.disk put /wasm/$(basename $w) $w; done
# Корневые сертификаты для HTTPS: системная связка хозяина (PEM) -> /etc/ssl/roots.der на диске Киры
$HOSTCC -O1 -Wall -o tools/pem2der tools/pem2der.c
ROOTS_SRC=""
for b in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt ${PREFIX:-/nonexistent}/etc/tls/cert.pem /etc/ssl/cert.pem; do
    [ -f "$b" ] && ROOTS_SRC=$b && break
done
ROOTS_N=$([ -n "$ROOTS_SRC" ] && tools/pem2der "$ROOTS_SRC" tools/roots.der)
[ -n "$ROOTS_N" ] && echo "[*] Корневые сертификаты: $ROOTS_N из $ROOTS_SRC" || echo "[!] Связки корневых сертификатов нет (пакет ca-certificates): fetch https откажется проверять сервер"
[ -n "$ROOTS_N" ] && tools/kfstool kira.disk put /etc/ssl/roots.der tools/roots.der
# Семя опыта Разума (получено в QEMU workflow mind-train): стартовые веса предвидения и подсказки по рычагам, если своего опыта на диске нет
[ -f data/mind_seed.bin ] && tools/kfstool kira.disk put /etc/mind_seed.bin data/mind_seed.bin >/dev/null

echo "[*] Компиляция C..."
OBJ="boot.o"
for src in $C_SOURCES $ARCH_SOURCES; do
    $CC $CFLAGS -c $src -o ${src%.c}.o
    OBJ="$OBJ ${src%.c}.o"
done
OBJ="$OBJ interrupts.o context.o ap_tramp.o uaccess_s.o"

echo "[*] Линковка..."
ld.lld -T $ARCH/linker.ld -nostdlib -z max-page-size=0x1000 $OBJ -o kira.bin

echo "[*] ISO..."
rm -rf isodir kira.iso
mkdir -p isodir/boot/grub
cp kira.bin isodir/boot/kira.bin
cp grub.cfg isodir/boot/grub/grub.cfg
grub-mkrescue -o kira.iso isodir 2>/dev/null
if command -v xorriso >/dev/null 2>&1 && xorriso -indev kira.iso -report_el_torito plain 2>/dev/null | grep -qi "uefi"; then
    echo "[*] ISO: загрузка BIOS и UEFI."
else
    echo "[!] В ISO нет UEFI-загрузки: современные ПК её не увидят. Установите: apt install grub-efi-amd64-bin mtools xorriso, затем соберите снова."
fi

case "$1" in
    --no-run) exit 0 ;;
    --android)
        OUT="${KIRA_OUT:-/storage/emulated/0/Download}"
        [ -d "$OUT" ] || { echo "[!] Нет папки $OUT (в Termux: termux-setup-storage; в proot - проверьте, что /storage примонтирован)"; exit 1; }
        cp kira.iso "$OUT/kira.iso"
        if [ -f "$OUT/kira_disk.img" ]; then
            tools/kfstool "$OUT/kira_disk.img" init >/dev/null
            for t in $TESTPROGS; do tools/kfstool "$OUT/kira_disk.img" rm /bin/$t >/dev/null 2>&1 || true; done   # убрать тестовые программы прежних версий
            for e in $UELF; do tools/kfstool "$OUT/kira_disk.img" put /bin/$(basename $e .elf) $e >/dev/null; done
            for w in build-wasm/*.wasm; do tools/kfstool "$OUT/kira_disk.img" put /wasm/$(basename $w) $w >/dev/null; done
            for h in kcc/include/*.h; do tools/kfstool "$OUT/kira_disk.img" put /etc/kcc/include/$(basename $h) $h >/dev/null; done
            [ -n "$ROOTS_N" ] && tools/kfstool "$OUT/kira_disk.img" put /etc/ssl/roots.der tools/roots.der >/dev/null
            echo "[*] Диск $OUT/kira_disk.img обновлён: новые программы в /bin, опыт и данные сохранены."
        else
            cp kira.disk "$OUT/kira_disk.img"
            echo "[*] Диск: $OUT/kira_disk.img (новый)."
        fi
        echo "[*] Образ: $OUT/kira.iso ($(( $(stat -c %s kira.iso) / 1024 )) КБ)"
        echo "[*] Limbo x86: CD-ROM = kira.iso, Hard Disk A = kira_disk.img, Boot from = CD-ROM, VGA = std. Подробно - LIMBO.md"
        exit 0 ;;
    --vnc|--ata|--sata|--nvme|--usb|--xhci|--hub|--flash*|--uefi) exec ./run_qemu.sh "$@" ;;
    *)        exec ./run_qemu.sh ;;
esac
