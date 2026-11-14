#!/bin/bash
# Сборка Kira OS через clang + ld.lld.
# Ничего, кроме clang, ld.lld, nasm и grub-mkrescue, не требуется.
#   ./build_clang.sh            собрать и запустить (только UART)
#   ./build_clang.sh --vnc      собрать и запустить с экраном по VNC (127.0.0.1:5900)
#   ./build_clang.sh --vnc --ata  то же, но диск как ATA (по умолчанию virtio-blk)
#   ./build_clang.sh --no-run   только собрать
#   ./build_clang.sh --no-run --spatial-preview  собрать Spatial Minimal для CI без Android-пути и без исключения тестовых программ
#   ./build_clang.sh --vnc --xhci --hub --flash   собрать и запустить с USB-хабом и флешкой (см. run_qemu.sh)
#   ./build_clang.sh --android       собрать полный Spatial пакет со всеми системными, тестовыми и учебными программами.
#   ./build_clang.sh --android-full  то же самое: полный Spatial пакет со всеми программами и ISO + диск в Download телефона (Limbo x86):
#                               существующий диск обновляется через временную копию с проверкой и атомарной заменой; /var и /data сохраняются.
#                               Папку можно задать: KIRA_OUT=/путь ./build_clang.sh --android
set -e

CC="clang --target=x86_64-unknown-none-elf"
COMMON="-ffreestanding -fno-omit-frame-pointer -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -O2 -Wall -Wextra -Werror"
# Ядро собирается БЕЗ SSE/MMX: обработчики прерываний не трогают регистры xmm,
# поэтому прерванному потоку не нужно сохранять их при входе в прерывание.
CFLAGS="$COMMON -mno-mmx -mno-sse -mno-sse2 -I. -Iarch/x86_64"
# Spatial Minimal is the production desktop; preview/full Android packages use the same visual system.
    *" --android-full "*|"*" --android "*)
        OUT="${KIRA_OUT:-/storage/emulated/0/Download}"
        [ -d "$OUT" ] || { echo "[!] Нет папки $OUT (в Termux: termux-setup-storage; в proot - проверьте, что /storage примонтирован)"; exit 1; }
        DISK_TMP="$OUT/.kira_disk.img.tmp.$$"
        ISO_TMP="$OUT/.kira.iso.tmp.$$"
        SUM_TMP="$OUT/.KIRA-FULL-SHA256SUMS.tmp.$$"
        cleanup_full() { rm -f "$DISK_TMP" "$ISO_TMP" "$SUM_TMP"; }
        trap cleanup_full EXIT INT TERM

        # Stage outputs on the destination filesystem; never mutate the user's disk
        # until all package writes and filesystem validation have succeeded.
        cp -p kira.iso "$ISO_TMP"
        if [ -f "$OUT/kira_disk.img" ]; then
            cp -p "$OUT/kira_disk.img" "$DISK_TMP"
            # ls mounts without formatting: reject an unknown/corrupt disk rather than overwrite it.
            tools/kfstool "$DISK_TMP" ls / >/dev/null
            for e in $UELF; do tools/kfstool "$DISK_TMP" put "/bin/$(basename "$e" .elf)" "$e" >/dev/null; done
            for w in build-wasm/*.wasm; do [ -f "$w" ] && tools/kfstool "$DISK_TMP" put "/wasm/$(basename "$w")" "$w" >/dev/null; done
            for h in kcc/include/*.h; do [ -f "$h" ] && tools/kfstool "$DISK_TMP" put "/etc/kcc/include/$(basename "$h")" "$h" >/dev/null; done
            [ -n "$ROOTS_N" ] && tools/kfstool "$DISK_TMP" put /etc/ssl/roots.der tools/roots.der >/dev/null
            [ -f data/mind_seed.bin ] && tools/kfstool "$DISK_TMP" put /etc/mind_seed.bin data/mind_seed.bin >/dev/null
            tools/mkdrv.sh "$DISK_TMP" userland/drvedu.elf "$HOSTCC"
        else
            cp -p kira.disk "$DISK_TMP"
        fi

        # Re-open the staged filesystem after all writes; a failed check leaves the old disk untouched.
        tools/kfstool "$DISK_TMP" ls / >/dev/null
        ISO_HASH=$(sha256sum "$ISO_TMP" | awk '{print $1}')
        DISK_HASH=$(sha256sum "$DISK_TMP" | awk '{print $1}')
        printf '%s  kira.iso\n%s  kira_disk.img\n' "$ISO_HASH" "$DISK_HASH" > "$SUM_TMP"

        # Each rename is atomic because all temporary files live in OUT.
        mv -f "$DISK_TMP" "$OUT/kira_disk.img"
        mv -f "$ISO_TMP" "$OUT/kira.iso"
        mv -f "$SUM_TMP" "$OUT/KIRA-FULL-SHA256SUMS"
        trap - EXIT INT TERM
        echo "[*] Полный пакет готов:"
        ls -lh "$OUT/kira.iso" "$OUT/kira_disk.img" "$OUT/KIRA-FULL-SHA256SUMS"
        echo "[*] Limbo x86: CD-ROM = kira.iso (self-contained boot + embedded KiraFS); Hard Disk A = kira_disk.img (persistent state, recommended), Boot from = CD-ROM, VGA = std. Подробно - LIMBO.md"
        exit 0 ;;
    --vnc|--ata|--sata|--nvme|--usb|--xhci|--hub|--flash*|--uefi) exec ./run_qemu.sh "$@" ;;
    *)        exec ./run_qemu.sh ;;
esac
