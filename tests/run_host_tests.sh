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
$CC $FLAGS -o $T/pmm tests/pmm_test.c pmm.c -Wl,--defsym=kernel_start=0x100000 -Wl,--defsym=kernel_end=0x320000
$T/pmm | head -4

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
$CC $FLAGS -fsanitize=undefined -o $T/fsrv tests/fsrv_test.c fsrv.c kfs.c store.c
$T/fsrv
$CC $FLAGS -fsanitize=undefined -Itests/stub -o $T/paint tests/paint_test.c fsrv.c kfs.c store.c
$T/paint

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

echo "== UI primitives (angles, arcs, anti-aliasing) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/ui tests/ui_test.c gfxui.c gfx2d.c font8x16.c -lm
$T/ui

echo "== mind (norms, anomalies, health, quantum self-tuning, memory, focus, starvation, behavior, persistence) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/mind tests/mind_test.c mind.c mindnet.c
$T/mind

echo "== device tree (FDT) parser =="
$CC $FLAGS -fsanitize=address,undefined -o $T/fdt tests/fdt_test.c fdt.c
$T/fdt
$CC $FLAGS -fsanitize=address,undefined -o $T/crypto tests/crypto_test.c crypto.c sha256.c
$T/crypto

echo "== mind foresight model (online logistic regression, calibration, trust) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/mindnet tests/mindnet_test.c mindnet.c -lm
$T/mindnet

echo "== real-time clock (calendar, CMOS decode, epoch, Russian date) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/rtc tests/rtc_test.c rtc.c
$T/rtc

echo "== explorer tree (KiraFS listing: order, nesting, limits) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/explorer tests/explorer_test.c explorer.c
ASAN_OPTIONS=detect_leaks=0 $T/explorer

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

echo "== shell (top bar, side launcher, traffic lights, fullscreen, drag, input, live background, culling) =="
$CC $FLAGS -o $T/wm tests/wm_test.c wm.c gfxui.c gfx2d.c font8x16.c icons.c brainfx.c
$T/wm

echo "== display thread end-to-end with apps (preview: /tmp/kira_gfx/*.ppm) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/disp tests/display_preview.c display.c kpar.c wm.c gfxui.c icons.c brainfx.c apps.c explorer.c dock.c mind.c mindnet.c rtc.c input.c gfx2d.c font8x16.c klog.c fbcon.c -lm
# код возврата превью - не через tail (раньше сбой терялся в конвейере)
for r in "" "1280 720" "1920 1080"; do
    ASAN_OPTIONS=detect_leaks=0 $T/disp $r > $T/disp.out 2>&1; rc=$?
    tail -1 $T/disp.out
    [ $rc -eq 0 ] || { cat $T/disp.out | grep FAIL | head; exit 1; }
done
echo "== view: formats against reference sums (ci/mkfiles.py), fuzz of damaged files =="
python3 ci/mkfiles.py $T/files > $T/sums.txt
$CC -O1 -g -w -DKIRA_HOST_TEST -I. -fsanitize=address,undefined -o $T/view tests/view_host.c font8x16.c
sum() { python3 -c "import sys; sys.path.insert(0,'ci'); import mkfiles as m; print('sum %08x' % m.SUMS['$1'])"; }
ASAN_OPTIONS=detect_leaks=0 $T/view $T/files/pic.png "$(sum /pic.png)" $T/files/rgba.png "$(sum /rgba.png)" \
    $T/files/pic.bmp "$(sum /pic.bmp)" $T/files/pic.ppm "$(sum /pic.ppm)" \
    $T/files/pic.gif "$(sum /pic.gif)" $T/files/lace.gif "$(sum /lace.gif)" \
    $T/files/note.txt "текст, " $T/files/blob.bin "двоичный, 512" > $T/view.out 2>&1 || { cat $T/view.out; exit 1; }
tail -1 $T/view.out
J=$T/files
ASAN_OPTIONS=detect_leaks=0 $T/view cmp $J/pic.jpg $J/pic.jpg.rgb $J/p422.jpg $J/p422.jpg.rgb $J/p444.jpg $J/p444.jpg.rgb $J/gray.jpg $J/gray.jpg.rgb > $T/view.out 2>&1 || { cat $T/view.out; exit 1; }
tail -1 $T/view.out
ASAN_OPTIONS=detect_leaks=0 $T/view $J/prog.jpg "JPEG: прогрессивный - не поддерживается" > $T/view.out 2>&1 || { cat $T/view.out; exit 1; }
for f in pic.png rgba.png pic.bmp pic.ppm pic.gif lace.gif pic.jpg p444.jpg; do ASAN_OPTIONS=detect_leaks=0 $T/view fuzz $T/files/$f > $T/view.out 2>&1 || { tail -20 $T/view.out; exit 1; }; tail -1 $T/view.out; done
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
$CC $FLAGS -fsanitize=address,undefined -o $T/audio tests/audio_test.c audio.c -lm
$T/audio
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
rm -rf $T
echo "ALL HOST TESTS PASSED"
