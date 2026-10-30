#!/bin/bash
# Запуск готового kira.iso. Консоль - в этом терминале (UART).
#   ./run_qemu.sh         без экрана
#   ./run_qemu.sh --vnc   экран по VNC: подключитесь VNC-клиентом к 127.0.0.1:5900
#                         (на телефоне подойдёт любое VNC-приложение: адрес 127.0.0.1, порт 5900)
#   ./run_qemu.sh --ata   диск как ATA PIO (старый путь; для проверки запасного драйвера)
#   ./run_qemu.sh --sata  диск на контроллере AHCI (SATA, как у большинства ПК)
#   ./run_qemu.sh --nvme  диск NVMe (как у современных ноутбуков и ПК)
#   Флаги можно сочетать: ./run_qemu.sh --vnc --ata
#   ./run_qemu.sh --usb   USB-планшет и USB-клавиатура на UHCI (как в Limbo с Mouse: usb-tablet)
#   ./run_qemu.sh --xhci  те же устройства на контроллере xHCI (как на современных ПК)
#   ./run_qemu.sh --flash образ флешки kira_flash.img (64 МБ FAT32, создаётся сам, если есть mkfs.fat) - в системе /usb0
#   ./run_qemu.sh --flash=путь.img  своя флешка (FAT12/16/32, с таблицей разделов MBR/GPT или без)
#   ./run_qemu.sh --hub   USB-хаб: флешка и клавиатура подключаются через него
#   Контроллер для флешки и хаба - xHCI, если не указан --usb (тогда UHCI). Пример: ./run_qemu.sh --vnc --xhci --hub --flash
#   ./run_qemu.sh --uefi  загрузка через UEFI (OVMF: apt install ovmf) - так же грузятся современные ПК
#   Процессор и плата: CPU=max MACHINE=q35 ./run_qemu.sh --vnc  (CPU: qemu64 по умолчанию, max - все возможности эмулятора)
#   Память и ядра: MEM=2G SMP=4 ./run_qemu.sh --vnc  (по умолчанию 512M и половина ядер устройства, не больше 4)
# Выход: Ctrl+C
EXTRA="-display none"
FW=""
USBOPT=""
HC=""; HID=0; HUB=0; FLASH=""
DISK="-drive file=kira.disk,format=raw,if=none,id=kd0 -device virtio-blk-pci,drive=kd0,disable-legacy=on"
for a in "$@"; do
    case "$a" in
        --vnc) EXTRA="-display none -vnc 127.0.0.1:0"; echo "[*] VNC: 127.0.0.1:5900" ;;
        --uefi)
            for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/qemu/OVMF.fd; do [ -f "$f" ] && FW="-bios $f" && break; done
            [ -n "$FW" ] || { echo "[!] Прошивка OVMF не найдена: apt install ovmf"; exit 1; }
            echo "[*] UEFI: $FW" ;;
        --usb) HC=uhci; HID=1 ;;
        --xhci) HC=xhci; HID=1 ;;
        --hub) HUB=1 ;;
        --flash) FLASH=kira_flash.img ;;
        --flash=*) FLASH="${a#--flash=}" ;;
        --ata) DISK="-drive file=kira.disk,format=raw,if=ide,index=0" ;;
        --sata) DISK="-drive file=kira.disk,format=raw,if=none,id=kd0 -device ahci,id=ahci0 -device ide-hd,drive=kd0,bus=ahci0.0" ;;
        --nvme) DISK="-drive file=kira.disk,format=raw,if=none,id=kd0 -device nvme,drive=kd0,serial=KIRA0001" ;;
    esac
done
if [ -n "$FLASH" ] && [ ! -f "$FLASH" ]; then
    MKFS=$(command -v mkfs.fat || command -v mkfs.vfat || true)
    [ -n "$MKFS" ] || { echo "[!] Нет $FLASH, и нечем его создать: apt install dosfstools (или укажите --flash=готовый.img)"; exit 1; }
    dd if=/dev/zero of="$FLASH" bs=1M count=64 2>/dev/null
    "$MKFS" -F 32 -n KIRAFLASH "$FLASH" >/dev/null
    if command -v mcopy >/dev/null 2>&1; then
        printf 'Привет из Termux! Этот файл лежит на флешке FAT32.\n' > /tmp/kira_hello.txt
        mcopy -i "$FLASH" /tmp/kira_hello.txt ::/hello.txt
    fi
    echo "[*] Создана флешка $FLASH (64 МБ, FAT32)"
fi
if [ "$HID" = 1 ] || [ "$HUB" = 1 ] || [ -n "$FLASH" ]; then
    [ -n "$HC" ] || HC=xhci
    if [ "$HC" = uhci ]; then USBOPT="-usb"; BUS="usb-bus.0"; else USBOPT="-device qemu-xhci,id=xhci"; BUS="xhci.0"; fi
    # без --hub порты выбирает QEMU (у UHCI их два: когда кончаются, QEMU сам подключает хаб)
    KP=""; SP=""
    if [ "$HUB" = 1 ]; then USBOPT="$USBOPT -device usb-hub,bus=$BUS,port=1"; KP=",port=1.2"; SP=",port=1.1"; fi
    [ "$HID" = 1 ] && USBOPT="$USBOPT -device usb-tablet,bus=$BUS -device usb-kbd,bus=$BUS$KP"
    [ -n "$FLASH" ] && USBOPT="$USBOPT -drive if=none,id=stick,format=raw,file=$FLASH -device usb-storage,bus=$BUS$SP,drive=stick,removable=on"
    echo "[*] USB: $HC$([ "$HUB" = 1 ] && echo ', хаб')$([ "$HID" = 1 ] && echo ', планшет и клавиатура')$([ -n "$FLASH" ] && echo ", флешка $FLASH")"
fi
[ -f kira.disk ] || dd if=/dev/zero of=kira.disk bs=1M count=8 2>/dev/null
if [ -z "$SMP" ]; then
    # Половина ядер устройства, но не больше 4: остальное нужно самому эмулятору (экран VNC, диск) и системе телефона.
    # Для проверки масштабирования задайте явно: SMP=8 ./run_qemu.sh --vnc
    SMP=$(( $(nproc 2>/dev/null || echo 2) / 2 ))
    [ "$SMP" -lt 1 ] && SMP=1
    [ "$SMP" -gt 4 ] && SMP=4
fi
echo "[*] Ядер: $SMP, память: ${MEM:-512M}"
CPUOPT=""
[ -n "$CPU" ] && CPUOPT="-cpu $CPU"
exec qemu-system-x86_64 -machine ${MACHINE:-pc} $CPUOPT $FW $DISK -boot order=d -cdrom kira.iso -m ${MEM:-512M} -smp $SMP -vga std $USBOPT $EXTRA -audiodev none,id=a0 -serial stdio -no-reboot -d cpu_reset,guest_errors -D qemu.log
