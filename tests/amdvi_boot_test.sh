#!/bin/bash
# AMD-Vi под QEMU q35 + amd-iommu: ядро включает трансляцию DMA, virtio-blk работает через домен с отображением,
# а обращение устройства к невыданной памяти останавливается и записывается как нарушение.
# Ядро собирается clang (как build_clang.sh, без запуска QEMU); образ - собственная BIOS-цепочка.
# Нет qemu / amd-iommu / clang / nasm - тест пропускается с пометкой SKIP (код 0), НЕ считается пройденным.
#   tests/amdvi_boot_test.sh [готовый kira.bin]
set -u
cd "$(dirname "$0")/.."
skip() { echo "SKIP: $1 (amdvi_boot_test НЕ выполнен)"; exit 0; }
for t in qemu-system-x86_64 nasm python3 timeout; do command -v $t >/dev/null || skip "нет $t"; done
qemu-system-x86_64 -device help 2>&1 | grep -q '"amd-iommu"' || skip "этот QEMU без устройства amd-iommu"
T=${KEEP_T:-$(mktemp -d)}; mkdir -p $T; [ -n "${KEEP_T:-}" ] || trap 'rm -rf "$T"' EXIT
HOSTCC=${HOSTCC:-gcc}

KERNEL=${1:-}
if [ -z "$KERNEL" ]; then
    command -v clang >/dev/null && command -v ld.lld >/dev/null || skip "нет clang/ld.lld для сборки ядра"
    CC="clang --target=x86_64-unknown-none-elf"
    CFLAGS="-ffreestanding -fno-omit-frame-pointer -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -O2 -Wall -Wextra -Werror -mno-mmx -mno-sse -mno-sse2"
    mkdir -p $T/inc
    $HOSTCC -O1 -DKFS_NO_THREADS -I. -Iarch/x86_64 -o $T/kfstool tools/kfstool.c kfs.c zstd.c store.c sha256.c sysseal.c cbor.c cas.c || exit 1
    echo amdvi-probe > $T/amdvi-probe.elf
    $T/kfstool manifest $T/amdvi-probe.elf > $T/inc/manifest.h || exit 1     # progs.c требует манифест; программы в этом образе не нужны
    S=$(sed -n '/^C_SOURCES=/p;/^ARCH_SOURCES=/p' build_clang.sh | sed 's/^[A-Z_]*="//;s/"$//' | tr ' ' '\n' | grep -v '^$' | sed 's|\$ARCH|arch/x86_64|')
    for a in boot interrupts context ap_tramp uaccess; do nasm -f elf64 arch/x86_64/$a.s -o $T/$a.o || exit 1; done
    OBJ=""
    for s in $S; do
        o=$T/$(echo $s | tr / _).o
        $CC $CFLAGS -I$T/inc -I. -Iarch/x86_64 -c $s -o $o 2>$T/err || { echo "FAIL: сборка $s: $(head -c 400 $T/err)"; exit 1; }
        OBJ="$OBJ $o"
    done
    ld.lld -T arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000 $T/boot.o $OBJ $T/interrupts.o $T/context.o $T/ap_tramp.o $T/uaccess.o -o $T/kira.bin || exit 1
    KERNEL=$T/kira.bin
fi
$HOSTCC -O1 -DKFS_NO_THREADS -I. -Iarch/x86_64 -o $T/kfstool tools/kfstool.c kfs.c zstd.c store.c sha256.c sysseal.c cbor.c cas.c || exit 1
nasm -f bin arch/x86_64/bios/stage1.asm -o $T/s1.bin && nasm -f bin arch/x86_64/bios/stage2.asm -o $T/s2.bin \
    && python3 tools/mkbios.py $T/s1.bin $T/s2.bin "$KERNEL" $T/k.img >/dev/null || exit 1
dd if=/dev/zero of=$T/kira.disk bs=1M count=8 2>/dev/null
$T/kfstool $T/kira.disk init >/dev/null || exit 1
cp $T/kira.disk $T/kira.disk.orig

# run <метка> <опции QEMU...>: UART - в $T/<метка>.log, трассировка QEMU - в $T/<метка>.trace.
# Ядро после загрузки не выходит само: ждём «Command line ready» (до AMDVI_WAIT секунд), даём ещё 3 с на хвост журнала и останавливаем QEMU.
run() {
    local label=$1; shift
    cp $T/kira.disk.orig $T/kira.disk
    timeout 90 qemu-system-x86_64 -machine q35 -cpu qemu64 -m 256 -display none -serial file:$T/$label.raw -no-reboot \
        -drive file=$T/k.img,format=raw -drive file=$T/kira.disk,format=raw,if=none,id=kd0 \
        -trace 'amdvi_translation_result' -trace 'amdvi_pages_inval' -trace 'amdvi_devtab_inval' -D $T/$label.trace "$@" >/dev/null 2>&1 &
    local pid=$! i
    for i in $(seq 1 ${AMDVI_WAIT:-40}); do
        sleep 1
        grep -q "Command line ready" $T/$label.raw 2>/dev/null && break
        kill -0 $pid 2>/dev/null || break
    done
    sleep 3
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    tr -d '\r' < $T/$label.raw | head -c 400000 > $T/$label.log
}
fail=0
ok()   { echo "ok:   $1"; }
bad()  { echo "FAIL: $1"; fail=1; }
has()  { grep -q -- "$2" $T/$1.log && ok "$3" || bad "$3 (нет '$2' в журнале $1)"; }
hasnot() { grep -q -- "$2" $T/$1.log && bad "$3 ('$2' найдено в журнале $1)" || ok "$3"; }

# --- 1. q35 + amd-iommu + virtio-blk, которому QEMU отдаёт DMA через IOMMU ---
run iommu -device amd-iommu -device virtio-blk-pci,drive=kd0,disable-legacy=on,iommu_platform=on
has iommu "Kernel core starting" "ядро загрузилось"
has iommu "IVRS: 1 AMD-Vi unit" "IVRS разобран"
has iommu "AMD-Vi translation enabled on 1 unit" "трансляция AMD-Vi включена"
has iommu "passthrough: unrestricted DMA, NOT isolated" "остальные устройства честно помечены как passthrough"
has iommu "isolated: own translation domain" "virtio-blk получил свой домен"
has iommu "enforcement check passed - no fault record on this platform" "проверка принуждения: запись в невыданную память остановлена, метка цела"
has iommu "Disk virtio-blk.*DMA isolated by IOMMU" "диск поднят через отображение"
has iommu "Command line ready" "система дошла до консоли с диском за IOMMU"
hasnot iommu "KERNEL PANIC" "без паники"
hasnot iommu "blk request timeout" "запросы к диску не зависли"
grep -q "amdvi_translation_result.*hpa 0x0\b" $T/iommu.trace && ok "QEMU отказал в трансляции невыданной страницы (hpa 0x0)" || bad "в трассе QEMU нет отказа в трансляции"
cmp -s $T/kira.disk.orig $T/kira.disk && bad "образ диска не изменился: записи через IOMMU не было" || ok "образ диска изменён ядром (запись через отображение)"

# --- 2. устройство не согласует ACCESS_PLATFORM: QEMU обходит IOMMU, изоляцию не объявляем ---
run noplat -device amd-iommu -device virtio-blk-pci,drive=kd0,disable-legacy=on
has noplat "AMD-Vi translation enabled on 1 unit" "AMD-Vi включён и без iommu_platform"
has noplat "does not negotiate ACCESS_PLATFORM" "честная пометка: DMA устройства мимо IOMMU"
hasnot noplat "DMA isolated by IOMMU" "изоляция не заявлена"
has noplat "Command line ready" "диск работает"

# --- 3. без IOMMU поведение прежнее ---
run plain -device virtio-blk-pci,drive=kd0,disable-legacy=on
has plain "No DMAR table" "без IOMMU: сообщение и прежнее поведение"
hasnot plain "translation enabled" "трансляция не включалась"
has plain "Command line ready" "система дошла до консоли"

[ $fail = 0 ] && echo "amdvi boot: PASS" || { echo "amdvi boot: FAIL"; for l in iommu noplat plain; do echo "--- $l (хвост) ---"; grep -i "iommu\|virtio\|panic" $T/$l.log | tail -15; done; exit 1; }
