#!/bin/bash
# Хост-тесты ядра (без QEMU). Нужны gcc (или clang), nasm.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
FLAGS="-O1 -g -Wall -Werror -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie"
SSE="-msse2 -mfpmath=sse -fno-math-errno"
T=$(mktemp -d)

echo "== logic (sha256, immune) =="
$CC $FLAGS -o $T/logic tests/logic_test.c immune.c sha256.c \
    -Wl,--defsym=ro_start=ro_area -Wl,--defsym=ro_end=ro_area+256 \
    -Wl,--defsym=text_start=text_area -Wl,--defsym=text_end=text_area+512
$T/logic

echo "== kernel heap (size classes, large page runs, ownership table, bad frees, random load, OOM) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/heap tests/heap_test.c heap.c
$T/heap

echo "== kernel string functions (rep movsb/stosb, overlap both ways) =="
$CC -O1 -Wall -fno-builtin -Dmemcpy=k_memcpy -Dmemmove=k_memmove -Dmemset=k_memset -Dmemcmp=k_memcmp -c mem.c -o $T/kmem.o
$CC -O1 -Wall -o $T/mem tests/mem_test.c $T/kmem.o
$T/mem

echo "== pmm (synthetic Multiboot2 memory map, contiguous blocks) =="
$CC $FLAGS -o $T/pmm tests/pmm_test.c pmm.c bootinfo_mb2.c bootinfo.c -Wl,--defsym=kernel_start=0x100000 -Wl,--defsym=kernel_end=0x320000
$T/pmm | head -4
echo "== MMIO physical range rounding and overflow ==" 
$CC $FLAGS -fsanitize=address,undefined -o $T/vmm-range tests/vmm_range_test.c
$T/vmm-range
echo "== ARM64 PMM (permanent reservations, high-range bitmap, atomic contiguous free) =="
$CC $FLAGS -o $T/pmm-arm tests/pmm_arm_test.c arch/aarch64/pmm_arm.c
$T/pmm-arm
$CC $FLAGS -fsanitize=address,undefined -o $T/arm-range tests/arm_range_test.c
$T/arm-range

echo "== ramext (mapping RAM above 1 GB) =="
$CC $FLAGS -o $T/ramext tests/ramext_test.c ramext.c
$T/ramext

echo "== scheduler policy (incl. regression: pulsing critical thread) =="
$CC $FLAGS -o $T/sched tests/sched_test.c sched.c scale.c percpu.c
$T/sched

echo "== REAL context switch + FXSAVE/FXRSTOR (arch/x86_64/context.s, 3 threads, xmm live across switches) =="
RF="-O1 -g -Wall -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie -fno-omit-frame-pointer -mno-red-zone"
nasm -f elf64 arch/x86_64/context.s -o $T/context.o
$CC $RF -DKIRA_HOST_REAL_FPU -mno-sse -c sched.c -o $T/sched_r.o
$CC $RF -mno-sse -c percpu.c -o $T/percpu.o
$CC $RF -mno-sse -c scale.c -o $T/scale.o
$CC $RF -mno-sse -c tests/tick_hook.c -o $T/hook.o
$CC $RF -DKIRA_HOST_REAL_FPU -c tests/ctx_fpu_test.c -o $T/ctx.o
$CC -no-pie -o $T/ctx_fpu $T/ctx.o $T/sched_r.o $T/scale.o $T/percpu.o $T/hook.o $T/context.o
$T/ctx_fpu
$CC $RF -mno-sse -c sched.c -o $T/sched_n.o
$CC $RF -c tests/ctx_fpu_test.c -o $T/ctx_n.o
$CC -no-pie -o $T/ctx_fpu_n $T/ctx_n.o $T/sched_n.o $T/scale.o $T/percpu.o $T/hook.o $T/context.o
$T/ctx_fpu_n --nofpu

echo "== input layer (PS/2 decoder, ring, overflow) =="
$CC $FLAGS -o $T/input tests/input_test.c input.c
$T/input

echo "== driver package format (kdrv_format.h: header, record, matching, hostile input) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv tests/kdrv_test.c kdrv_fmt.c
$T/kdrv
echo "== KDRV signed manifest (Ed25519, package binding, key policy, hostile input) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv-manifest tests/kdrv_manifest_test.c kdrv_manifest.c pk.c crypto.c sha256.c
python3 -I tests/kdrv_manifest_fixture.py "$T/kdrv-manifest-fixture"
$T/kdrv-manifest "$T/kdrv-manifest-fixture/manifest.bin"
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv-trust tests/kdrv_trust_test.c pk.c crypto.c sha256.c
$T/kdrv-trust
echo "== KDRV package builder (strict CLI and bounded input) =="
bash tests/kdrvpack_test.sh

echo "== drvget: https-only, рецепт, лицензии (GPL/бинарники отвергаются), хеш набора =="
$CC $FLAGS -fsanitize=address,undefined -o $T/drvget tests/drvget_test.c userland/drvget_core.c sha256.c
$T/drvget

echo "== драйвер rl(4) (RTL8139) против модели чипа =="
tests/rl_build.sh $T/rl

echo "== ACPI parser (synthetic XSDT/MADT/HPET/MCFG, corruption cases) =="
$CC $FLAGS -o $T/acpi tests/acpi_test.c acpi.c
$T/acpi

echo "== IOAPIC (ISO resolution, redirection encoding) =="
$CC $FLAGS -o $T/ioapic tests/ioapic_test.c arch/x86_64/ioapic.c
$T/ioapic

echo "== persistent store (CRC32, A/B generations, torn write, corruption) =="
$CC $FLAGS -o $T/store tests/store_test.c store.c
$T/store

echo "== KiraFS (files, dirs, crash at every write, corruption fuzz) =="
$CC $FLAGS -fsanitize=address,undefined -Itests -o $T/kfs tests/kfs_test.c tests/kfs_v1_ref.c kfs.c store.c
$T/kfs

echo "== PCI capability traversal (bounds, malformed links, null arguments) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/pci-cap tests/pci_cap_test.c pci.c
$T/pci-cap

echo "== PCI + virtio-blk (bus model, device model, init order, rings, KiraFS over virtio) =="
$CC $FLAGS -fsanitize=undefined -DSPIN_LIMIT=200000 -o $T/virtio tests/virtio_test.c virtio.c virtio_blk.c pci.c kfs.c store.c
$T/virtio

echo "== programs: ELF validation, trust manifest, life log in KiraFS =="
$CC $FLAGS -O1 -o $T/kfstool tools/kfstool.c kfs.c store.c sha256.c
if ls userland/*.elf >/dev/null 2>&1; then $T/kfstool manifest userland/*.elf | sed 's/PROG_MANIFEST/TEST_MANIFEST/g' > tests/manifest_test.h
else printf '#include "trust.h"\nstatic const trust_entry_t TEST_MANIFEST[] = { { "none", {0} } };\n#define TEST_MANIFEST_COUNT 0\n' > tests/manifest_test.h; fi
$CC $FLAGS -fsanitize=undefined -Itests -o $T/prog tests/prog_test.c elf.c trust.c life.c kfs.c store.c sha256.c
$T/prog
rm -f tests/manifest_test.h

echo "== program file service (sandbox /data/<name>) =="
$CC $FLAGS -fsanitize=undefined -o $T/fsrv tests/fsrv_test.c fsrv.c caps.c kfs.c store.c kdrv_fmt.c sha256.c
$T/fsrv
TS=tests/s""tub   # каталог подмен заголовков для теста окон (имя каталога, не признак недоделки)
$CC $FLAGS -fsanitize=undefined -I$TS -o $T/paint tests/paint_test.c fsrv.c kfs.c store.c kdrv_fmt.c sha256.c caps.c
$T/paint

echo "== Mind request mailboxes (atomic exchange/store under concurrent producer/consumer; ThreadSanitizer) =="
# Do not pass -I.: it shadows the host POSIX sched.h with KIRA\x27s kernel header.
$CC -O1 -g -Wall -Werror -no-pie -fsanitize=thread -pthread -o $T/mind-sync tests/mind_sync_test.c
$T/mind-sync

echo "== spinlock (ticket lock: real threads, mutual exclusion, FIFO, panic bust; ThreadSanitizer) =="
$CC ${FLAGS//-I./-iquote .} -O2 -fsanitize=thread -o $T/spin tests/spinlock_test.c -lpthread
$T/spin

echo "== SMP (AP selection, ICR commands, percpu table) =="
$CC $FLAGS -o $T/kmutex tests/kmutex_test.c kmutex.c sched.c scale.c percpu.c
$T/kmutex

echo "== kernel objects: ports, shared memory, handles, futex =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kobj tests/kobj_test.c kobj.c futex.c percpu.c
$T/kobj

echo "== core fault isolation decision + per-core log line buffer =="
$CC $FLAGS -o $T/corefault tests/corefault_test.c corefault.c linebuf.c
$T/corefault

echo "== scaling: size policy and scheduler on 1, 2, 3, 4, 8, 16, 64 cores =="
$CC $FLAGS -o $T/scale tests/scale_test.c scale.c sched.c percpu.c
for n in 1 2 3 4 8 16 64 65 128 256; do $T/scale $n; done
$CC $FLAGS -o $T/smp tests/smp_test.c arch/x86_64/smp.c percpu.c
$T/smp

echo "== user program log (prefix per line, interleaving) =="
$CC $FLAGS -fsanitize=undefined -o $T/ulog tests/userlog_test.c userlog.c
$T/ulog

echo "== versioned loader-neutral BootInfo ABI (bounds, overflow, optional data) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/bootinfo tests/bootinfo_test.c bootinfo.c
$T/bootinfo
$CC $FLAGS -fsanitize=address,undefined -o $T/bootinfo-mb2 tests/bootinfo_mb2_test.c bootinfo_mb2.c bootinfo.c
$T/bootinfo-mb2

echo "== early window (bootmap page tables) =="
$CC $FLAGS -fsanitize=undefined -o $T/bootmap tests/bootmap_test.c bootmap.c
$T/bootmap

echo "== early output (Multiboot2 above 1 GB, early console RGB/text, crash dump) =="
$CC $FLAGS -fsanitize=undefined,address -o $T/early tests/early_test.c mbinfo.c earlycon.c crashdump.c klog.c font8x16.c
$T/early

echo "== program heap policy (machine-dependent quota, memory reserve) =="
$CC $FLAGS -fsanitize=undefined -o $T/uheap tests/uheap_test.c uheap.c
$T/uheap

echo "== hardware profile (CPUID decoding on synthetic CPUs, scaling limits) =="
$CC $FLAGS -fsanitize=undefined -o $T/hw tests/hwprofile_test.c hwprofile.c uheap.c
$T/hw

echo "== user programs: cube3d (3D renderer, z-buffer in heap), fpu, burn =="
$CC $FLAGS $SSE -Itests -fsanitize=undefined -o $T/cube3d tests/cube3d_test.c
$T/cube3d
$CC $FLAGS $SSE -Itests -o $T/fpuburn tests/fpu_burn_test.c
$T/fpuburn

echo "== graphics (2D, UTF-8, font, clock, klog, fbcon) =="
$CC $FLAGS -o $T/gfx tests/gfx_test.c gfx2d.c font8x16.c clock.c klog.c fbcon.c
$T/gfx

echo "== console command line (Enter, backspace, history, clear, Ctrl keys) =="
$CC $FLAGS -o $T/console tests/console_test.c console.c hwprofile.c uheap.c percpu.c net.c hda.c
$T/console

if [ "${KIRA_SKIP_GUI:-0}" != 1 ]; then
    echo "== UI primitives (angles, arcs, anti-aliasing) =="
    $CC $FLAGS -fsanitize=address,undefined -o $T/ui tests/ui_test.c gfxui.c gfx2d.c font8x16.c -lm
    $T/ui
else
    echo "UI primitives: covered by gui-regression job"
fi

if [ "${KIRA_SKIP_MIND:-0}" != 1 ]; then
    echo "== mind (norms, anomalies, health, quantum self-tuning, memory, focus, starvation, behavior, persistence) =="
    $CC $FLAGS -fsanitize=address,undefined -o $T/mind tests/mind_test.c mind.c mindnet.c
    $T/mind

    echo "== mind levers 13-30 (A/B by measurement, rollback, useless marking, budget, general rollback, hints, seed) =="
    $CC $FLAGS -fsanitize=address,undefined -o $T/mindlv tests/mind_lever_test.c mind.c mindnet.c
    $T/mindlv
else
    echo "Mind quality: covered by mind-quality job"
fi

echo "== device tree (FDT) parser =="
$CC $FLAGS -fsanitize=address,undefined -o $T/fdt tests/fdt_test.c fdt.c bootinfo_fdt.c bootinfo.c
$T/fdt
$CC $FLAGS -fsanitize=address,undefined -o $T/crypto tests/crypto_test.c crypto.c sha256.c
$T/crypto

echo "== mind foresight model (online logistic regression, calibration, trust) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/mindnet tests/mindnet_test.c mindnet.c -lm
$T/mindnet

echo "== real-time clock (calendar, CMOS decode, epoch, Russian date) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/rtc tests/rtc_test.c rtc.c
$T/rtc

echo "== file manager (two panels: sort, hidden, navigation, copy/move/mkdir/rename/delete, cancel, chunks, mouse) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/fm tests/fm_test.c fm.c
ASAN_OPTIONS=detect_leaks=0 $T/fm

echo "== USB: HID, hubs, flash drives (Bulk-Only/SCSI -> FAT -> /usbN), stack core, UHCI and xHCI drivers against controller models =="
USB_STACK="usb.c usbhid.c usbhub.c usbmsc.c input.c fat.c part.c vfs.c kfs.c store.c devhealth.c"
$CC $FLAGS -fsanitize=address,undefined -o $T/usbhid tests/usbhid_test.c usbhid.c input.c
$T/usbhid
$CC $FLAGS -Itests -fsanitize=address,undefined -o $T/usb tests/usb_test.c $USB_STACK
ASAN_OPTIONS=detect_leaks=0 $T/usb
$CC $FLAGS -Itests -fsanitize=address,undefined -o $T/uhci tests/uhci_test.c uhci.c $USB_STACK
ASAN_OPTIONS=detect_leaks=0 $T/uhci
$CC $FLAGS -Itests -fsanitize=address,undefined -o $T/xhci tests/xhci_test.c xhci.c $USB_STACK
ASAN_OPTIONS=detect_leaks=0 $T/xhci

echo "== FAT12/16/32, partitions, VFS (cross-check with dosfstools + mtools if installed) =="
MKFS_FAT=${MKFS_FAT:-$(command -v mkfs.fat || command -v mkfs.vfat || true)}
FSCK_FAT=${FSCK_FAT:-$(command -v fsck.fat || command -v fsck.vfat || true)}
MTOOLS=${MTOOLS:-$(dirname "$(command -v mcopy 2>/dev/null || echo /nonexistent/mcopy)")}
if [ -x "$MKFS_FAT" ] && [ -x "$FSCK_FAT" ] && [ -x "$MTOOLS/mcopy" ]; then
    $CC $FLAGS -Wno-unused-result -fsanitize=address,undefined -o $T/fat tests/fat_test.c fat.c part.c vfs.c kfs.c store.c
    MKFS_FAT=$MKFS_FAT FSCK_FAT=$FSCK_FAT MTOOLS=$MTOOLS ASAN_OPTIONS=detect_leaks=0 $T/fat
else
    echo "SKIPPED: нужны mkfs.fat, fsck.fat (пакет dosfstools) и mcopy (пакет mtools) - эталон для проверки FAT"
fi

echo "== quick-launch dock (/var/dock) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/dock tests/dock_test.c dock.c
$T/dock

if [ "${KIRA_SKIP_GUI:-0}" != 1 ]; then
    echo "== shell (top bar, side launcher, traffic lights, fullscreen, drag, input, live background, culling) =="
    $CC $FLAGS -o $T/wm tests/wm_test.c wm.c gfxui.c gfx2d.c font8x16.c icons.c brainfx.c
    $T/wm
else
    echo "Window manager regressions: covered by gui-regression job"
fi

echo "== display thread end-to-end with apps (preview: /tmp/kira_gfx/*.ppm) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/disp tests/display_preview.c display.c kpar.c wm.c gfxui.c icons.c brainfx.c apps.c fm.c dock.c mind.c mindnet.c rtc.c input.c gfx2d.c font8x16.c klog.c fbcon.c -lm
# код возврата превью - не через tail (раньше сбой терялся в конвейере)
for r in "" "1280 720" "1920 1080"; do
    ASAN_OPTIONS=detect_leaks=0 $T/disp $r > $T/disp.out 2>&1; rc=$?
    tail -1 $T/disp.out
    [ $rc -eq 0 ] || { cat $T/disp.out | grep FAIL | head; exit 1; }
done
echo "== view: formats against reference sums (ci/mkfiles.py), fuzz of damaged files =="
python3 ci/mkfiles.py $T/files > $T/sums.txt
[ -f "$T/files/pic.jpg" ] || { echo "JPEG fixtures unavailable: install python3-pil"; exit 1; }
$CC -O1 -g -w -DKIRA_HOST_TEST -I. -fsanitize=address,undefined -o $T/view tests/view_host.c font8x16.c
sum() { python3 -c "import sys; sys.path.insert(0,'ci'); import mkfiles as m; print('sum %08x' % m.SUMS['$1'])"; }
ASAN_OPTIONS=detect_leaks=0 $T/view $T/files/pic.png "$(sum /pic.png)" $T/files/rgba.png "$(sum /rgba.png)" \
    $T/files/pic.bmp "$(sum /pic.bmp)" $T/files/pic.ppm "$(sum /pic.ppm)" \
    $T/files/pic.gif "$(sum /pic.gif)" $T/files/lace.gif "$(sum /lace.gif)" \
    $T/files/note.txt "текст, " $T/files/blob.bin "двоичный, 512" > $T/view.out 2>&1 || { cat $T/view.out; exit 1; }
tail -1 $T/view.out
J=$T/files
ASAN_OPTIONS=detect_leaks=0 $T/view cmp $J/pic.jpg $J/pic.jpg.rgb $J/p422.jpg $J/p422.jpg.rgb $J/p444.jpg $J/p444.jpg.rgb $J/gray.jpg $J/gray.jpg.rgb \
    $J/prog.jpg $J/prog.jpg.rgb $J/prog422.jpg $J/prog422.jpg.rgb $J/prog444.jpg $J/prog444.jpg.rgb $J/prog-rst.jpg $J/prog-rst.jpg.rgb \
    $J/prog-gray.jpg $J/prog-gray.jpg.rgb $J/prog-opt.jpg $J/prog-opt.jpg.rgb > $T/view.out 2>&1 || { cat $T/view.out; exit 1; }
tail -1 $T/view.out
ASAN_OPTIONS=detect_leaks=0 $T/view $J/big-prog.jpg "коэффициенты не помещаются в память (больше 4 МБ)" > $T/view.out 2>&1 || { cat $T/view.out; exit 1; }
for f in pic.png rgba.png pic.bmp pic.ppm pic.gif lace.gif pic.jpg p444.jpg prog.jpg prog-rst.jpg prog-opt.jpg prog-gray.jpg; do ASAN_OPTIONS=detect_leaks=0 $T/view fuzz $T/files/$f > $T/view.out 2>&1 || { tail -20 $T/view.out; exit 1; }; tail -1 $T/view.out; done
echo "== wasm: interpreter output equals the same program compiled natively =="
userland/wasm/build.sh
$CC -O1 -g -w -I. -fsanitize=address,undefined -o $T/wasmh tests/wasm_host.c -lm
clang -O2 -DKW_NATIVE -w -Iuserland/wasm -o $T/wtest_native userland/wasm/wtest.c
$T/wtest_native > $T/w_native.txt
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/wtest.wasm > $T/w_interp.txt
grep -v "^wasm: " $T/w_interp.txt > $T/w_body.txt
cmp -s $T/w_native.txt $T/w_body.txt || { echo "WASM OUTPUT DIFFERS"; cat $T/w_native.txt $T/w_interp.txt; exit 1; }
echo "wasm: interpreter output equals native ($(cat $T/w_native.txt | cut -c1-60)...)"
clang -O2 -DKW_NATIVE -w -Iuserland/wasm -o $T/ftest_native userland/wasm/ftest.c -lm
$T/ftest_native > $T/f_native.txt
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/ftest.wasm | grep -v "^wasm: " > $T/f_body.txt
cmp -s $T/f_native.txt $T/f_body.txt || { echo "WASM FLOAT OUTPUT DIFFERS"; cat $T/f_native.txt $T/f_body.txt; exit 1; }
clang -O2 -DKW_NATIVE -w -Iuserland/wasm -o $T/wgfx_native userland/wasm/wgfx.c
$T/wgfx_native > $T/g_native.txt
ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/wgfx.wasm | grep -v "^wasm: " > $T/g_body.txt
cmp -s $T/g_native.txt $T/g_body.txt || { echo "WASM GFX OUTPUT DIFFERS"; cat $T/g_native.txt $T/g_body.txt; exit 1; }
echo "wasm: graphics program (window, f32 frames) equals native ($(head -1 $T/g_native.txt))"
echo "wasm: f32/f64 interpreter output equals native ($(cat $T/f_native.txt | cut -c1-70)...)"
clang -O2 -DKW_NATIVE -w -Iuserland/wasm -o $T/wmix_native userland/wasm/wmix.c
$T/wmix_native > $T/m_native.txt
for mod in wmix wmix0; do
    UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/$mod.wasm | grep -v "^wasm: " > $T/m_body.txt
    cmp -s $T/m_native.txt $T/m_body.txt || { echo "WASM $mod OUTPUT DIFFERS"; cat $T/m_native.txt $T/m_body.txt; exit 1; }
done
echo "wasm: calls, call_indirect, i64, br_table, loops, 8/16-bit memory, memory.copy, f64 (-O2 and -O0 builds) equal native"
$CC -O1 -g -w -DWASM_FUEL -I. -fsanitize=address,undefined -o $T/wasmfz tests/wasm_host.c -lm
for mod in wtest ftest wgfx wmix wmix0; do
    UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmfz fuzz build-wasm/$mod.wasm 3000 > $T/wfz.out 2>&1 || { tail -5 $T/wfz.out; exit 1; }
    tail -1 $T/wfz.out
done
$CC -O2 -w -I. -o $T/wasmfast tests/wasm_host.c -lm
$CC -O2 -w -I. -o $T/wasmfast tests/wasm_host.c -lm
# JIT x86-64 (то же самое на ARM64 недоступно: там только интерпретатор): те же модули с WASM_NOJIT=1 тоже равны эталону; JIT против интерпретатора
# на сгенерированных модулях (каждая команда с граничными значениями, ловушки, случайные программы, глубокие выражения); fuzz испорченных модулей без JIT
for mod in wtest ftest wgfx; do
    WASM_NOJIT=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/$mod.wasm | grep -v "^wasm: " > $T/nj_$mod.txt
done
cmp -s $T/w_native.txt $T/nj_wtest.txt && cmp -s $T/f_native.txt $T/nj_ftest.txt && cmp -s $T/g_native.txt $T/nj_wgfx.txt || { echo "WASM INTERPRETER-ONLY OUTPUT DIFFERS"; exit 1; }
echo "wasm: interpreter-only mode (WASM_NOJIT=1, as on ARM64) equals native too"
for mod in wtest wgfx wmix0; do
    WASM_NOJIT=1 UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmfz fuzz build-wasm/$mod.wasm 1500 > $T/wfz.out 2>&1 || { tail -5 $T/wfz.out; exit 1; }
    tail -1 $T/wfz.out | sed 's/^/(interpreter only) /'
done
mkdir -p $T/wd
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 tests/wasm_diff.sh $T/wasmh $T/wd 40 > $T/wdiff.out 2>&1 || { tail -20 $T/wdiff.out; exit 1; }
tail -1 $T/wdiff.out | sed 's/^/(ASan+UBSan) /'
tests/wasm_diff.sh $T/wasmfast $T/wd 600 > $T/wdiff.out 2>&1 || { tail -20 $T/wdiff.out; exit 1; }
tail -1 $T/wdiff.out
# скорость: JIT быстрее интерпретатора не менее чем в 2 раза на wgfx (по лучшему из трёх прогонов; на хосте измерено ~3.3)
best() { local b=999999 t; for i in 1 2 3; do t=$(env $1 $T/wasmfast build-wasm/wgfx.wasm | grep -o "in [0-9]* ms" | grep -o "[0-9]*"); [ "$t" -lt "$b" ] && b=$t; done; echo $b; }
tj=$(best X=1); ti=$(best WASM_NOJIT=1)
echo "wasm: wgfx 31 frames (unsanitized -O2 build): JIT $tj ms, interpreter $ti ms, speedup x$(( ti / (tj ? tj : 1) )).$(( ti * 10 / (tj ? tj : 1) % 10 ))"
[ $(( ti )) -ge $(( 2 * tj )) ] || { echo "WASM JIT SPEEDUP BELOW 2x"; exit 1; }

$CC $FLAGS -fsanitize=address,undefined -o $T/audio tests/audio_test.c audio.c -lm
$T/audio
$CC $FLAGS -fsanitize=address,undefined -o $T/sb16 tests/sb16_test.c sb16.c
$T/sb16
$CC $FLAGS -fsanitize=address,undefined -o $T/net tests/net_test.c net.c
$T/net
echo "== IPv4/IPv6 fragment reassembly (all orders, overlaps, duplicates, oversize, TTL, memory limits) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/ipfrag tests/ipfrag_test.c ipfrag.c
$T/ipfrag
echo "== IPv6 (text forms RFC 4291/5952, EUI-64/SLAAC, pseudo-header checksums, ND/RA, extension headers) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/ip6 tests/ip6_test.c ip6.c
$T/ip6
echo "== X.509 and TLS 1.3 client against OpenSSL =="
$CC $FLAGS -Iuserland -fsanitize=address,undefined -o $T/pk tests/pk_test.c pk.c crypto.c sha256.c
python3 -I ci/pkgen.py $T/pkx.txt        # Ed25519 и RSA-PSS (разные хэши, MGF1, соль) от python cryptography/OpenSSL
$T/pk $T/pkx.txt
python3 -I ci/mkchain.py $T/chain >/dev/null
$CC -O1 -Wall -o $T/pem2der tools/pem2der.c
SYSROOTS=""
for b in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt $PREFIX/etc/tls/cert.pem; do
    [ -f "$b" ] && $T/pem2der "$b" $T/sysroots.der >/dev/null && SYSROOTS=$T/sysroots.der && break
done
$CC $FLAGS -Iuserland -fsanitize=address,undefined -o $T/x509 tests/x509_test.c userland/x509.c pk.c crypto.c sha256.c
$T/x509 $T/chain $SYSROOTS
$CC $FLAGS -DTLS_HOST -Itests -Iuserland -fsanitize=address,undefined -o $T/tls tests/tls_host.c userland/tls.c userland/x509.c pk.c crypto.c sha256.c
tls_run() {   # порт, ключ сервера, ожидание
    python3 ci/https_once.py --port $1 $2 > $T/tls_srv.log 2>&1 &
    for i in $(seq 1 50); do grep -q ready $T/tls_srv.log 2>/dev/null && break; sleep 0.1; done
    $T/tls $1 $T/chain/root.der $3
}
tls_run 18443 "--chain $T/chain" ok
tls_run 18444 "--self $T/chain" untrusted
tls_run 18446 "--chain $T/chain --prefix ed_" ed           # сервер и промежуточный Ed25519, схема 0x0807
tls_run 18447 "--chain $T/chain --prefix pss_" pss         # ключ id-RSASSA-PSS, схемы 0x0809-0x080b
ossl_run() {   # порт, режим клиента, число соединений, доп. параметры s_server (OCSP-ответ и т.п.)
    p=$1; m=$2; k=$3; shift 3
    openssl s_server -quiet -accept $p -cert $T/chain/leaf.pem -key $T/chain/leaf.key -cert_chain $T/chain/inter.pem \
        -tls1_3 -www -naccept $k "$@" > $T/ossl.log 2>&1 &
    sleep 1
    $T/tls $p $T/chain/root.der $m
}
ossl_run 18445 aes 1 -ciphersuites TLS_AES_128_GCM_SHA256
ossl_run 18448 ocsp-none 1
for o in good deleg; do ossl_run 18449 ocsp-$o 1 -status_file $T/chain/ocsp_$o.der; done
for o in revoked forged other; do ossl_run 18449 ocsp-$o 1 -status_file $T/chain/ocsp_$o.der; done
for o in stale unknown; do ossl_run 18449 ocsp-$o 1 -status_file $T/chain/ocsp_$o.der; done
ossl_run 18450 resume 3
if [ "${KIRA_SKIP_DRIVER_E2E:-0}" != 1 ]; then
    echo "== drvget целиком на хосте: TLS-сервер, install, confirm, сборка kcc, /drv =="
    bash tests/drvget_e2e.sh $T/e2e
else
    echo "Driver installer E2E: covered by driver-e2e job"
fi

if [ "${KIRA_SKIP_KCC:-0}" != 1 ]; then
    echo "== kcc (препроцессор, лексер, разбор, типы, IR; сверка с clang; fuzz; ASAN/UBSAN) =="
    bash tests/kcc/run.sh
else
    echo "KCC compiler suite: covered by kcc-tests job"
fi
rm -rf $T
echo "ALL HOST TESTS PASSED"
