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

C_SOURCES="syscall.c percpu.c mem.c sched.c console.c kernel.c pmm.c vmm.c heap.c keyboard.c timer.c mind.c mindnet.c mind_sys.c rtc.c watchdog.c immune.c sha256.c uart.c clock.c klog.c gfx2d.c font8x16.c fb.c fbcon.c display.c wm.c input.c acpi.c proc.c elf.c trust.c life.c progs.c fsrv.c ata.c store.c experience.c gfxsrv.c gfxui.c icons.c brainfx.c apps.c explorer.c dock.c usbhid.c usb.c usbhub.c usbmsc.c uhci.c xhci.c kfs.c fat.c part.c vfs.c ahci.c nvme.c uaccess.c devhealth.c kpar.c fscmd.c pci.c virtio.c virtio_blk.c virtio_gpu.c virtio_input.c e1000.c net.c irqwait.c hda.c blk.c bootmap.c mbinfo.c earlycon.c crashdump.c uheap.c userlog.c hwprofile.c ramext.c kmutex.c linebuf.c corefault.c scale.c kobj.c futex.c"
# Код, зависящий от процессора x86-64 (вход в ядро, прерывания, APIC, ядра, FPU, системные вызовы): arch/x86_64
ARCH=arch/x86_64
ARCH_SOURCES="$ARCH/gdt.c $ARCH/idt.c $ARCH/pic.c $ARCH/apic.c $ARCH/ioapic.c $ARCH/irqmode.c $ARCH/fpu.c $ARCH/syscall_arch.c $ARCH/exceptions.c $ARCH/smp.c $ARCH/percpu_arch.c $ARCH/proc_arch.c $ARCH/uaccess_arch.c $ARCH/msi_arch.c"

echo "[*] Ассемблер..."
rm -f *.o arch/x86_64/*.o kira.bin
nasm -f elf64 $ARCH/boot.s -o boot.o
nasm -f elf64 $ARCH/interrupts.s -o interrupts.o
nasm -f elf64 $ARCH/context.s -o context.o
nasm -f elf64 $ARCH/ap_tramp.s -o ap_tramp.o
nasm -f elf64 $ARCH/uaccess.s -o uaccess_s.o

echo "[*] Userland (Ring 3): программы собираются в ELF и кладутся на диск, ядро их не содержит..."
UCFLAGS="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra"
ULINK="-T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro"
UELF=""
for app in plasma paint burn mt ipc; do
    $CC $UCFLAGS -c userland/$app.c -o userland/$app.o
    ld.lld $ULINK -o userland/$app.elf userland/$app.o
    UELF="$UELF userland/$app.elf"
done
# Программы с вещественной арифметикой (3D, тест FPU): SSE2 разрешён только им, ядро собирается без SSE.
USSE_FLAGS="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -msse2 -mfpmath=sse -mno-mmx -fno-math-errno -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra"
for app in cube3d fpu; do
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

echo "[*] Инструмент kfstool, манифест доверенных программ и установка в образ диска..."
HOSTCC="${HOSTCC:-clang}"
$HOSTCC -O1 -Wall -DKFS_NO_THREADS -I. -Iarch/x86_64 -o tools/kfstool tools/kfstool.c kfs.c store.c sha256.c
tools/kfstool manifest $UELF > manifest.h
[ -f kira.disk ] || dd if=/dev/zero of=kira.disk bs=1M count=8 2>/dev/null
tools/kfstool kira.disk init
for e in $UELF; do
    tools/kfstool kira.disk put /bin/$(basename $e .elf) $e
done

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
            for e in $UELF; do tools/kfstool "$OUT/kira_disk.img" put /bin/$(basename $e .elf) $e >/dev/null; done
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
