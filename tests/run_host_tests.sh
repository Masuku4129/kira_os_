#!/bin/bash
# Хост-тесты ядра (без QEMU). Нужны gcc (или clang), nasm.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
FLAGS="-O1 -g -Wall -Werror -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie"
SSE="-msse2 -mfpmath=sse -fno-math-errno"
T=$(mktemp -d)

echo "== уровни архитектуры (docs/architecture/levels.map) =="
python3 ci/check_layers.py

echo "== process PID allocator (no wrap/reuse for private IPC principals) =="
$CC $FLAGS -o $T/proc-pid tests/proc_pid_test.c
$T/proc-pid

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

echo "== UEFI BootInfo to kernel-compatible Multiboot2 bridge =="
$CC $FLAGS -fsanitize=address,undefined -o $T/bootinfo-uefi tests/bootinfo_uefi_test.c bootinfo_uefi.c bootinfo.c
$T/bootinfo-uefi

echo "== UEFI loader and KBI adapter compile (GNU-EFI ABI) =="
EFI_INC=${EFI_INCLUDE:-/usr/include/efi}
if [ -s "$EFI_INC/efi.h" ]; then
EFI_CFLAGS="-O2 -Wall -Wextra -Werror -fpic -fshort-wchar -mno-red-zone -fno-stack-protector -fno-stack-check -fno-strict-aliasing -DEFI_FUNCTION_WRAPPER -isystem $EFI_INC -isystem $EFI_INC/x86_64"
$CC $EFI_CFLAGS -I. -Iarch/x86_64/uefi -c arch/x86_64/uefi/loader.c -o $T/uefi-loader.o
# доверенный режим (KIRA_SECURE=1) тоже обязан собираться без предупреждений: ключ для проверки сборки одноразовый
python3 tools/kirasig.py keygen $T/efikey.pem $T/efikey.pub >/dev/null
python3 tools/kirasig.py cheader $T/efikey.pub $T/kira_bootkey.h
$CC $EFI_CFLAGS -I. -Iarch/x86_64/uefi -I$T -DKIRA_SECURE_BOOT=1 -DKIRA_MIN_IMAGE_VERSION=0u -DKIRA_REQUIRE_SB=1 -DKIRA_REQUIRE_TPM=1 -c arch/x86_64/uefi/loader.c -o $T/uefi-loader-secure.o
$CC $EFI_CFLAGS -I. -Iarch/x86_64/uefi -c arch/x86_64/uefi/uefi_kbi.c -o $T/uefi-kbi.o
else
echo "gnu-efi не найден ($EFI_INC/efi.h): сборка загрузчика пропущена (в CI он есть)"
fi
echo "UEFI loader sources: compile passed"
echo "== UEFI: наш загрузчик + GPT/ESP под OVMF (без GRUB) =="
if [ "${KIRA_SKIP_UEFI_QEMU:-0}" != 1 ]; then
    tests/uefi_boot_test.sh
else
    echo "UEFI boot under OVMF: запускается отдельно (общий стенд QEMU под блокировкой)"
fi

echo "== pmm (synthetic Multiboot2 memory map, contiguous blocks) =="
$CC $FLAGS -o $T/pmm tests/pmm_test.c pmm.c bootinfo_mb2.c bootinfo.c -Wl,--defsym=kernel_start=0x100000 -Wl,--defsym=kernel_end=0x320000
$T/pmm | head -4
echo "== ARM64 PMM (permanent reservations, high-range bitmap, atomic contiguous free) =="
$CC $FLAGS -o $T/pmm-arm tests/pmm_arm_test.c arch/aarch64/pmm_arm.c
$T/pmm-arm

echo "== ramext (mapping RAM above 1 GB) =="
$CC $FLAGS -o $T/ramext tests/ramext_test.c ramext.c
$T/ramext

echo "== W^X ядра (дробление страниц, права секций, обход таблиц, поиск W+X) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/wx tests/wx_test.c wx.c
ASAN_OPTIONS=detect_leaks=0 $T/wx

echo "== ранний W^X образа ядра (карта страниц с первой инструкции: права по секциям, мост KASLR, нет W+X) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/earlymap tests/earlymap_test.c earlymap.c wx.c
ASAN_OPTIONS=detect_leaks=0 $T/earlymap

echo "== KASLR (выбор базы: выравнивание, диапазон, занятое, энтропия; таблица перемещений; слова командной строки; политика kptr) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kaslr tests/kaslr_test.c kaslr.c kptr.c bootinfo.c -Wl,--defsym=kernel_start=0x30000000 -Wl,--defsym=kernel_end=0x30600000
$T/kaslr

echo "== перемещение образа по таблице tools/mkkreloc.py (эталон - образ, собранный компоновщиком по другому адресу) =="
tests/kreloc_test.sh

echo "== W^X ядра ARM64 (дробление блоков, права секций, обход таблиц, поиск W+X) =="
$CC $FLAGS -fsanitize=address,undefined -Iarch/aarch64 -o $T/wx_arm tests/wx_arm_test.c arch/aarch64/wx_arm.c
ASAN_OPTIONS=detect_leaks=0 $T/wx_arm

echo "== scheduler policy (incl. regression: pulsing critical thread) =="
$CC $FLAGS -o $T/sched tests/sched_test.c sched.c scale.c percpu.c bbring.c
$T/sched

echo "== REAL context switch + FXSAVE/FXRSTOR (arch/x86_64/context.s, 3 threads, xmm live across switches) =="
RF="-O1 -g -Wall -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie -fno-omit-frame-pointer -mno-red-zone"
nasm -f elf64 arch/x86_64/context.s -o $T/context.o
$CC $RF -DKIRA_HOST_REAL_FPU -mno-sse -c sched.c -o $T/sched_r.o
$CC $RF -mno-sse -c percpu.c -o $T/percpu.o
$CC $RF -mno-sse -c bbring.c -o $T/bbring.o
$CC $RF -mno-sse -c scale.c -o $T/scale.o
$CC $RF -mno-sse -c tests/tick_hook.c -o $T/hook.o
$CC $RF -DKIRA_HOST_REAL_FPU -c tests/ctx_fpu_test.c -o $T/ctx.o
$CC -no-pie -o $T/ctx_fpu $T/ctx.o $T/sched_r.o $T/bbring.o $T/scale.o $T/percpu.o $T/hook.o $T/context.o
$T/ctx_fpu
$CC $RF -mno-sse -c sched.c -o $T/sched_n.o
$CC $RF -c tests/ctx_fpu_test.c -o $T/ctx_n.o
$CC -no-pie -o $T/ctx_fpu_n $T/ctx_n.o $T/sched_n.o $T/bbring.o $T/scale.o $T/percpu.o $T/hook.o $T/context.o
$T/ctx_fpu_n --nofpu

echo "== input layer (PS/2 decoder, ring, overflow) =="
$CC $FLAGS -o $T/input tests/input_test.c input.c
$T/input

echo "== authenticated KDRV catalogue (signature policy, canonical parser, tamper, revocation, expiry, rollback) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv-catalogue tests/kdrv_catalogue_test.c drivers/kdrv/kdrv_catalogue.c drivers/kdrv/kdrv_trust.c pk.c crypto.c sha256.c
$T/kdrv-catalogue

echo "== driver package format (kdrv_format.h: header, record, matching, hostile input) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv tests/kdrv_test.c kdrv_fmt.c
$T/kdrv

echo "== Ring 3 block device channel (kdrv_blk.h: registration, request bounds, rings, in-flight tracking, hostile responses) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv_blk tests/kdrv_blk_test.c kdrv_blk.c
$T/kdrv_blk

echo "== kdrvpack: mandatory publisher state requires explicit confirmation =="
$CC $FLAGS -o $T/kdrvpack tools/kdrvpack.c kdrv_fmt.c sha256.c
printf '\177ELF\002\001\001\000' > $T/sample.elf
if $T/kdrvpack rl $T/sample.elf $T/pkg.kdrv $T/pkg.rec --pci 10ec:8139 --caps mmio --publisher-required >/dev/null 2>&1; then
    echo "FAIL: kdrvpack accepted --publisher-required without --confirm"; exit 1
fi
$T/kdrvpack rl $T/sample.elf $T/pkg.kdrv $T/pkg.rec --pci 10ec:8139 --caps mmio --confirm --publisher-required >/dev/null
PUB_STATE=$(od -An -tx4 -j108 -N4 $T/pkg.rec | tr -d ' ')
[ "$PUB_STATE" = "4b4f4b50" ] || { echo "FAIL: kdrvpack did not persist KREC_CONFIRMED_PUBLISHER ($PUB_STATE)"; exit 1; }
echo "kdrvpack: publisher-required record and explicit confirmation passed"

echo "== drvget: https-only, рецепт, лицензии (GPL/бинарники отвергаются), хеш набора =="
$CC $FLAGS -fsanitize=address,undefined -o $T/drvget tests/drvget_test.c userland/drvget_core.c sha256.c
$T/drvget

echo "== driver compatibility pipeline (HTTPS, bounds, path safety, fail-closed stages) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/drvcompat tests/drvcompat_test.c userland/drvcompat.c
$T/drvcompat

echo "== драйвер RTL8139 (чистая реализация) против модели чипа =="
$CC $FLAGS -fsanitize=address,undefined -o $T/rtl8139 tests/rtl8139_test.c drivers/net/rtl8139.c
$T/rtl8139

echo "== ACPI parser (synthetic XSDT/MADT/HPET/MCFG, corruption cases) =="
$CC $FLAGS -o $T/acpi tests/acpi_test.c acpi.c
$T/acpi

echo "== сон S3: реестр хуков устройств (порядок, откат при отказе, журнал) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/devpm tests/devpm_test.c devpm.c
$T/devpm
echo "== AML: интерпретатор на настоящих таблицах (iasl) и на DSDT из QEMU, ASAN/UBSan =="
$CC $FLAGS -fsanitize=address,undefined -o $T/aml tests/aml_test.c aml.c aml_exec.c
$T/aml
echo "== ACPI в работе: события SCI/GPE/EC, кнопка питания, батарея, крышка, _PRT (модель железа) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/acpidev tests/acpidev_test.c acpidev.c aml.c aml_exec.c
$T/acpidev
echo "== AML на настоящих DSDT из QEMU (pc/PIIX и q35/ICH9): _PRT, связки, \\_S5, порча и обрезка =="
$CC $FLAGS -fsanitize=address,undefined -o $T/acpi_qemu tests/acpi_qemu_test.c acpidev.c aml.c aml_exec.c acpi.c
ASAN_OPTIONS=detect_leaks=0 $T/acpi_qemu
echo "== AML: фаззинг (случайные, обрезанные и испорченные таблицы) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/aml_fuzz tests/aml_fuzz.c aml.c aml_exec.c
ASAN_OPTIONS=detect_leaks=0 $T/aml_fuzz 3000

echo "== Intel VT-d DMAR topology parser (DRHD, RMRR, hostile input) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/iommu-vtd tests/iommu_vtd_test.c iommu_vtd.c
$T/iommu-vtd

echo "== VT-d end to end (iommu.c against a hardware model: DMAR -> contexts -> TE -> domains -> maps -> faults; scenarios A-E) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/iommu tests/iommu_test.c iommu.c iommu_vtd.c drivers/iommu/intel_vtd.c drivers/iommu/vtd_domain.c
for s in A B C D E; do $T/iommu $s; done

echo "== ARM SMMUv3 prepare (platform validation, stream table and allocation unwind) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/iommu-smmu tests/arm_smmu_prepare_test.c drivers/iommu/arm_smmu.c
$T/iommu-smmu

echo "== VT-d domain (hardware model: IOTLB invalidation after map/unmap, stale translations) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/vtd-domain tests/vtd_domain_test.c drivers/iommu/vtd_domain.c drivers/iommu/intel_vtd.c
$T/vtd-domain

echo "== ARM SMMUv3: STE, CD, stage-1 page tables, commands, events =="
$CC $FLAGS -fsanitize=address,undefined -o $T/smmudom tests/smmu_dom_test.c drivers/iommu/smmu_dom.c
$T/smmudom

echo "== AMD-Vi: IVRS (fuzz), DTE, v1 page tables, commands =="
$CC $FLAGS -fsanitize=address,undefined -o $T/amdvi tests/amdvi_test.c drivers/iommu/amd_vi.c
$T/amdvi

echo "== IOAPIC (ISO resolution, redirection encoding) =="
$CC $FLAGS -o $T/ioapic tests/ioapic_test.c arch/x86_64/ioapic.c
$T/ioapic

echo "== persistent store (CRC32, A/B generations, torn write, corruption) =="
$CC $FLAGS -o $T/store tests/store_test.c store.c
$T/store

echo "== KiraFS (files, dirs, crash at every write, corruption fuzz) =="
$CC $FLAGS -fsanitize=address,undefined -Itests -o $T/kfs tests/kfs_test.c tests/kfs_v1_ref.c kfs.c sha256.c zstd.c store.c
$T/kfs

echo "== KiraFS COW (kfs_clone: общие блоки, ссылки, обрыв питания) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kfscow tests/kfs_cow_test.c kfs.c sha256.c zstd.c store.c
$T/kfscow

echo "== PCI + virtio-blk (bus model, device model, init order, rings, KiraFS over virtio) =="
$CC $FLAGS -fsanitize=undefined -DSPIN_LIMIT=200000 -o $T/virtio tests/virtio_test.c virtio.c virtio_blk.c pci.c kfs.c sha256.c zstd.c store.c
$T/virtio

echo "== programs: ELF validation, trust manifest, life log in KiraFS =="
$CC $FLAGS -O1 -o $T/kfstool tools/kfstool.c kfs.c zstd.c store.c sha256.c sysseal.c cbor.c cas.c
if ls userland/*.elf >/dev/null 2>&1; then $T/kfstool manifest userland/*.elf | sed 's/PROG_MANIFEST/TEST_MANIFEST/g' > tests/manifest_test.h
else printf '#include "trust.h"\nstatic const trust_entry_t TEST_MANIFEST[] = { { "none", {0} } };\n#define TEST_MANIFEST_COUNT 0\n' > tests/manifest_test.h; fi
$CC $FLAGS -fsanitize=undefined -Itests -o $T/prog tests/prog_test.c elf.c trust.c life.c kfs.c zstd.c store.c sha256.c
$T/prog
rm -f tests/manifest_test.h

echo "== program file service (sandbox /data/<name>) =="
$CC $FLAGS -fsanitize=undefined -o $T/fsrv tests/fsrv_test.c fsrv.c caps.c kfs.c zstd.c store.c kdrv_fmt.c sha256.c
$T/fsrv
TS=tests/s""tub   # каталог подмен заголовков для теста окон (имя каталога, не признак недоделки)
$CC $FLAGS -fsanitize=undefined -I$TS -o $T/paint tests/paint_test.c fsrv.c kfs.c zstd.c store.c kdrv_fmt.c sha256.c caps.c
$T/paint

echo "== Mind request mailboxes (atomic exchange/store under concurrent producer/consumer; ThreadSanitizer) =="
# Do not pass -I.: it shadows the host POSIX sched.h with KIRA\x27s kernel header.
$CC -O1 -g -Wall -Werror -no-pie -fsanitize=thread -pthread -o $T/mind-sync tests/mind_sync_test.c
$T/mind-sync

echo "== spinlock (ticket lock: real threads, mutual exclusion, FIFO, panic bust; ThreadSanitizer) =="
$CC ${FLAGS//-I./-iquote .} -O2 -fsanitize=thread -o $T/spin tests/spinlock_test.c -lpthread
$T/spin

echo "== SMP (AP selection, ICR commands, percpu table) =="
$CC $FLAGS -o $T/kmutex tests/kmutex_test.c kmutex.c sched.c scale.c percpu.c bbring.c
$T/kmutex

echo "== kernel objects: ports, shared memory, handles, futex =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kobj tests/kobj_test.c kobj.c caps.c futex.c percpu.c
$T/kobj

echo "== application domains: ceilings, reach, quotas =="
$CC $FLAGS -fsanitize=address,undefined -o $T/domain tests/domain_test.c caps.c
$T/domain

echo "== core fault isolation decision + per-core log line buffer =="
$CC $FLAGS -o $T/corefault tests/corefault_test.c corefault.c linebuf.c
$T/corefault

echo "== scaling: size policy and scheduler on 1, 2, 3, 4, 8, 16, 64 cores =="
$CC $FLAGS -o $T/scale tests/scale_test.c scale.c sched.c percpu.c bbring.c
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

echo "== UEFI memory map to loader-neutral BootInfo (descriptor bounds and overflow) =="
$CC $FLAGS -Itests/uefi -fsanitize=address,undefined -o $T/uefi-kbi tests/uefi_kbi_test.c arch/x86_64/uefi/uefi_kbi.c bootinfo.c
$T/uefi-kbi

echo "== kernel entry decodes safely in both 32-bit (GRUB/BIOS) and 64-bit (UEFI) mode =="
bash tests/boot_entry_test.sh

echo "== UEFI loader: no boot-services calls between GetMemoryMap and ExitBootServices =="
bash tests/uefi_exit_order_test.sh

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

echo "== 3D engine k3d (tiles, z-buffer, perspective, clipping, light, textures, alpha, threads) =="
$CC $FLAGS $SSE -fsanitize=address,undefined -o $T/k3d tests/k3d_test.c userland/k3d.c -lm -lpthread
$T/k3d

echo "== user programs: cube3d (scene on k3d), fpu, burn =="
$CC $FLAGS $SSE -Itests -fsanitize=address,undefined -o $T/cube3d tests/cube3d_test.c userland/k3d.c -lpthread -lm
ASAN_OPTIONS=detect_leaks=0 $T/cube3d
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

echo "== cbor (RFC 8949: векторы, детерминированность, строгий режим, фаззинг) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -o $T/cbor tests/cbor_test.c cbor.c
$T/cbor

echo "== json (RFC 8259: строгий разбор, путь, глубина, фаззинг) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -o $T/json tests/json_test.c json.c
$T/json

echo "== cas (RFC 6962 дерево Меркла, пути включения, CAS, файлы кусками) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -o $T/cas tests/cas_test.c cas.c sha256.c
$T/cas
echo "== zstd (RFC 8878: эталонные кадры уровней 1-19, контрольные суммы, порча, усечение) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -o $T/zstdt tests/zstd_test.c zstd.c sha256.c
$T/zstdt
echo "== kmsg (сообщения портов: CBOR, строгий разбор, мусор) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -Iuserland -o $T/kmsg tests/kmsg_test.c cbor.c
$T/kmsg
echo "== kframe (каркас сообщений служб v1: запрос/ответ, строгие ключи, мусор) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -Iuserland -o $T/kframe tests/kframe_test.c cbor.c
$T/kframe

echo "== sysseal (печать системы: CBOR-манифест, корень Меркла, подмена и пропажа файлов) =="
$CC $FLAGS -Werror -Wextra -fsanitize=address,undefined -o $T/sysseal tests/sysseal_test.c sysseal.c cbor.c cas.c sha256.c
$T/sysseal

echo "== wallpaper (picture wallpaper: KWP1 parse, fill-crop scaling) =="
$CC $FLAGS -Werror -fsanitize=address,undefined -o $T/wallpaper tests/wallpaper_test.c wallpaper.c
$T/wallpaper

echo "== bbring (чёрный ящик: кольцо, переполнение, ограничитель частоты, файл, разбор, текст; потоки + ThreadSanitizer) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/bbring tests/bbring_test.c bbring.c
ASAN_OPTIONS=detect_leaks=0 $T/bbring
$CC $FLAGS -O2 -fsanitize=thread -o $T/bbring_tsan tests/bbring_test.c bbring.c -lpthread
$T/bbring_tsan --threads

echo "== latency (время реакции: корзины, границы, квантили, пороги, сброс по эпохе, слияние по ядрам, места, перевод тактов; потоки + ThreadSanitizer) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/latency tests/latency_test.c latency.c
$T/latency
$CC $FLAGS -O2 -fsanitize=thread -o $T/latency_tsan tests/latency_test.c latency.c -lpthread
$T/latency_tsan --threads

echo "== framegov (frame pacing: decided by the compositor, not by Mind) =="
$CC $FLAGS -Werror -o $T/framegov tests/framegov_test.c framegov.c
$T/framegov

echo "== Mind archive policy (preserve current and last-good snapshots, score eviction, fail closed) =="
$CC $FLAGS -Werror -o $T/mind-archive-policy tests/mind_archive_policy_test.c
$T/mind-archive-policy

if [ "${KIRA_SKIP_MIND:-0}" != 1 ]; then
    echo "== mind (norms, anomalies, health, quantum self-tuning, memory, focus, starvation, behavior, persistence) =="
    $CC $FLAGS -fsanitize=address,undefined -o $T/mind tests/mind_test.c mind.c mindnet.c
    $T/mind

    echo "== mind levers 13-23 (A/B by measurement, rollback, useless marking, budget, general rollback, hints, seed) =="
    $CC $FLAGS -fsanitize=address,undefined -o $T/mindlv tests/mind_lever_test.c mind.c mindnet.c
    $T/mindlv
else
    echo "Mind quality: covered by mind-quality job"
fi

echo "== public-key signatures (RSA, ECDSA, Ed25519; valid and forged vectors) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/pk tests/pk_test.c pk.c crypto.c sha256.c
$T/pk

echo "== KDRV signed-package verification =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv_sig tests/kdrv_sig_test.c kdrv_sig.c pk.c crypto.c sha256.c
$T/kdrv_sig
python3 -m unittest ci.test_kdrvsign
python3 -m unittest ci.test_latency_check

echo "== доверенная загрузка (А2): подписанный манифест KIRA.SIG - проверка, подмена любого байта, откат версии, формат совместим с tools/kirasig.py =="
CC=$CC bash tests/bootsig_test.sh

echo "== доверенная загрузка (А2): состояние Secure Boot и подписи в ядре (boottrust) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/boottrust tests/boottrust_test.c boottrust.c
$T/boottrust

echo "== защита от отката и подмены (А3): команды NV TPM 2.0 (tpm2.c) на модели TPM =="
$CC $FLAGS -fsanitize=address,undefined -Itests -o $T/tpm2 tests/tpm2_test.c tpm2.c
$T/tpm2
echo "== А3: политика отката образа (счётчик в TPM, вшитый порог) и диска (якорь эпохи), «загрузки» подряд на модели TPM =="
$CC $FLAGS -fsanitize=address,undefined -Itests -o $T/rollback tests/rollback_test.c rollback.c tpm2.c
$T/rollback
echo "== А3: команды tpm2.c на настоящем эмуляторе swtpm (если установлен) =="
CC=$CC bash tests/tpm2_swtpm_test.sh
echo "== А3: эпоха тома KiraFS, подмена диска старой копией, безопасный режим только чтения =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kfsepoch tests/kfs_epoch_test.c kfs.c rollback.c tpm2.c sha256.c zstd.c store.c
$T/kfsepoch
echo "== А3: подписанный манифест приложений: подпись, подмена, отзыв и ротация ключей, домены, режим допуска =="
CC=$CC bash tests/appman_test.sh
echo "== А3: мутации (убрана проверка - соответствующий тест обязан упасть) =="
CC=$CC bash tests/a3_mutation_test.sh

echo "== KDRV anti-rollback (version floor, corrupt state, revoked key, clock rollback, key rotation) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv_state tests/kdrv_state_test.c kdrv_state.c kdrv_sig.c kdrv_fmt.c pk.c crypto.c sha256.c
$T/kdrv_state
python3 ci/gen_kdrv_state_vectors.py | cmp - tests/kdrv_state_vectors.h

echo "== Mind build job (templates, quotas, kcc, ELF validation, human/signature gate, run only after grant under kloader) =="
bash tests/mind_build_test.sh

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

echo "== theme (contrast >= 4.5:1, three states, target sizes) =="
$CC $FLAGS -o $T/theme tests/theme_contrast_test.c -lm
$T/theme

echo "== file manager (two panels: sort, hidden, navigation, copy/move/mkdir/rename/delete, cancel, chunks, mouse) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/fm tests/fm_test.c fm.c
ASAN_OPTIONS=detect_leaks=0 $T/fm

echo "== USB: HID, hubs, flash drives (Bulk-Only/SCSI -> FAT -> /usbN), stack core, UHCI and xHCI drivers against controller models =="
USB_STACK="usb.c usbhid.c usbhub.c usbmsc.c usbmsc_fs.c input.c fat.c part.c vfs.c kfs.c sha256.c zstd.c store.c devhealth.c"
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
    $CC $FLAGS -Wno-unused-result -fsanitize=address,undefined -o $T/fat tests/fat_test.c fat.c part.c vfs.c kfs.c sha256.c zstd.c store.c
    MKFS_FAT=$MKFS_FAT FSCK_FAT=$FSCK_FAT MTOOLS=$MTOOLS ASAN_OPTIONS=detect_leaks=0 $T/fat
else
    echo "SKIPPED: нужны mkfs.fat, fsck.fat (пакет dosfstools) и mcopy (пакет mtools) - эталон для проверки FAT"
fi

echo "== quick-launch dock (/var/dock) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/dock tests/dock_test.c dock.c
$T/dock

if [ "${KIRA_SKIP_GUI:-0}" != 1 ]; then
    echo "== shell (top bar, side launcher, traffic lights, fullscreen, drag, input, live background, culling) =="
    $CC $FLAGS -o $T/wm tests/wm_test.c wm.c gfxui.c gfx2d.c font8x16.c icons.c brainfx.c wallpaper.c
    $T/wm
    echo "== уменьшение окна: быстрая версия = прямое усреднение по площади =="
    $CC $FLAGS -o $T/blit_scaled tests/blit_scaled_test.c gfxui.c gfx2d.c font8x16.c icons.c brainfx.c wallpaper.c
    $T/blit_scaled
    echo "== линии со сглаживанием: обход полосы = полный обход =="
    $CC $FLAGS -o $T/seg tests/seg_test.c gfxui.c gfx2d.c font8x16.c
    $T/seg
else
    echo "Window manager regressions: covered by gui-regression job"
fi

echo "== окно Разума: кэш карточек = полная перерисовка для каждого байта снимка =="
$CC $FLAGS -fsanitize=address,undefined -o $T/mcards tests/mind_cards_test.c apps.c gfxui.c gfx2d.c font8x16.c icons.c brainfx.c wallpaper.c zstd.c dock.c mind.c mindnet.c -lm
ASAN_OPTIONS=detect_leaks=0 $T/mcards

echo "== display thread end-to-end with apps (preview: /tmp/kira_gfx/*.ppm) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/disp tests/display_preview.c display.c framegov.c kpar.c wm.c gfxui.c icons.c brainfx.c wallpaper.c zstd.c apps.c fm.c dock.c mind.c mindnet.c rtc.c input.c gfx2d.c font8x16.c klog.c fbcon.c bbring.c -lm
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
echo "== wasm: WASI 0.1 subset (fd_write, clock, random_get, args, errors) in interpreter and JIT, exit code from proc_exit =="
for mode in jit interp; do
    if [ $mode = interp ]; then export WASM_NOJIT=1; else unset WASM_NOJIT; fi
    UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/wasitest.wasm > $T/wasi_$mode.txt 2>&1 || true
    grep -q "finished with code 3" $T/wasi_$mode.txt || { echo "WASI ($mode): proc_exit code"; cat $T/wasi_$mode.txt; exit 1; }
    grep -v "^wasm: " $T/wasi_$mode.txt | cmp -s - tests/wasi_expected.txt || { echo "WASI ($mode): output differs"; cat $T/wasi_$mode.txt; exit 1; }
done
echo "== wasm: WASI files (flat sandbox: create, read, append, seek, readdir, rename, unlink, limits), stdin, args, poll =="
for mode in jit interp; do
    if [ $mode = interp ]; then export WASM_NOJIT=1; else unset WASM_NOJIT; fi
    WASM_STDIN=$'hello\nworld\n' WASM_ARGS="one two" UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 $T/wasmh build-wasm/wasifs.wasm > $T/wasifs_$mode.txt 2>&1 || true
    grep -q "finished with code 0" $T/wasifs_$mode.txt || { echo "WASI files ($mode): exit code"; cat $T/wasifs_$mode.txt; exit 1; }
    grep -v "^wasm: " $T/wasifs_$mode.txt | cmp -s - tests/wasifs_expected.txt || { echo "WASI files ($mode): output differs"; diff <(grep -v "^wasm: " $T/wasifs_$mode.txt) tests/wasifs_expected.txt; exit 1; }
done
unset WASM_NOJIT
echo "wasm: WASI subset and files passed (JIT and interpreter)"
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
echo "== ksh: оболочка (userland/wasm/ksh.c): сценарии tests/ksh, хост-сборка и build-wasm/ksh.wasm (интерпретатор и JIT) дают один и тот же вывод =="
WASMH=$T/wasmh tests/ksh_test.sh $T

$CC $FLAGS -fsanitize=address,undefined -o $T/audio tests/audio_test.c audio.c -lm
$T/audio
$CC $FLAGS -fsanitize=address,undefined -o $T/sb16 tests/sb16_test.c sb16.c
$T/sb16
$CC $FLAGS -fsanitize=address,undefined -o $T/net tests/net_test.c net.c
$T/net
# фаззинг приёма кадров (А6): настоящий стек (net.c включён в тест, ядерные вызовы подменены), ip6.c, ipfrag.c; ARP, IPv4/IPv6, ICMP, UDP, TCP, DHCP, DNS, ND/RA, фрагменты;
# проверка всего, что стек отправил (длина, суммы, адреса). Зерно 1 и число сценариев задаёт KIRA_NET_FUZZ_SCEN (по умолчанию 20000, около 10 с); глубокий прогон: KIRA_NET_FUZZ_SCEN=2000000
$CC $FLAGS -fsanitize=address,undefined -o $T/net_fuzz tests/net_fuzz.c ip6.c ipfrag.c
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1 $T/net_fuzz ${KIRA_NET_FUZZ_SCEN:-20000} 1
echo "== fuzz: загрузчик ELF (инварианты elf.h на искажённых программах) и разборщик CBOR =="
$CC $FLAGS -fsanitize=address,undefined -o $T/elf_fuzz tests/elf_fuzz.c elf.c
if ls userland/*.elf >/dev/null 2>&1; then ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $T/elf_fuzz ${KIRA_FUZZ_ITERS:-20000} 1 userland/*.elf; fi
$CC $FLAGS -fsanitize=address,undefined -o $T/cbor_fuzz tests/cbor_fuzz.c cbor.c
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $T/cbor_fuzz ${KIRA_FUZZ_ITERS:-50000} 1
$CC $FLAGS -fsanitize=address,undefined -o $T/kdrv_fuzz tests/kdrv_fuzz.c kdrv_fmt.c
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $T/kdrv_fuzz ${KIRA_FUZZ_ITERS:-50000} 1
echo "== KiraFS: потеря питания при диске с кэшем записи (случайный набор секторов, обрыв на записи и на flush) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/kfs_crash tests/kfs_crash_test.c kfs.c sha256.c zstd.c store.c
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $T/kfs_crash 1 ${KIRA_KFS_CRASH_SEQ:-60}

echo "== журнал аудита (цепочка, подписи Ed25519, сегменты на KiraFS, все виды подмены, обрыв питания, полный диск) =="
$CC $FLAGS -fsanitize=address,undefined -o $T/audit tests/audit_test.c auditstore.c auditcore.c kfs.c sha256.c zstd.c store.c pk.c crypto.c
mkdir -p $T/auditdump
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $T/audit ${KIRA_AUDIT_CRASH_SEQ:-6} $T/auditdump
echo "== журнал аудита: независимая проверка tools/auditverify.py на файлах из теста (и подмена - обязана ловиться) =="
AD=$T/auditdump
python3 -I tools/auditverify.py --pub $AD/pub.hex --ckpt $AD/audit.ckpt $AD/audit.0*.log $AD/audit.log
python3 - $AD <<'PYEOF'
import sys
d = sys.argv[1]
b = bytearray(open(d + "/audit.0000000001.log", "rb").read()); b[256 + 160 * 5 + 70] ^= 1      # изменена одна запись
open(d + "/mut_modify.log", "wb").write(b)
b = bytearray(open(d + "/audit.0000000001.log", "rb").read()); del b[256 + 160 * 9: 256 + 160 * 10]      # удалена запись
open(d + "/mut_delete.log", "wb").write(b)
import struct
b = open(d + "/audit.log", "rb").read()
first = struct.unpack_from("<Q", b, 16)[0]; last = first + (len(b) - 256) // 160 - 1                  # номера записей активного файла
wit = struct.unpack_from("<Q", open(d + "/audit.ckpt", "rb").read(), 0)[0]                           # номер записи в файле-свидетеле
open(d + "/mut_trunc.log", "wb").write(b[:len(b) - (last - wit + 1) * 160])                         # отрезан хвост вместе с записью свидетеля (подписанный хвост пропал)
PYEOF
if python3 -I tools/auditverify.py --pub $AD/pub.hex $AD/mut_modify.log $AD/audit.0000000257.log $AD/audit.log >/dev/null; then echo "FAIL: auditverify.py пропустил изменённую запись"; exit 1; fi
if python3 -I tools/auditverify.py --pub $AD/pub.hex $AD/mut_delete.log $AD/audit.0000000257.log $AD/audit.log >/dev/null; then echo "FAIL: auditverify.py пропустил удалённую запись"; exit 1; fi
if python3 -I tools/auditverify.py --pub $AD/pub.hex --ckpt $AD/audit.ckpt $AD/audit.0000000001.log $AD/audit.0000000257.log $AD/mut_trunc.log >/dev/null; then echo "FAIL: auditverify.py пропустил обрезанный хвост"; exit 1; fi
echo "tools/auditverify.py: чистый журнал проходит, три вида подмены ловятся"
echo "== журнал аудита: мутации (убрали хэш, проверку цепочки или подписи - тест обязан упасть) =="
mkdir -p $T/mut
mut() {   # имя  образец  замена : копия auditcore.c с одним изменением, тест против неё должен упасть
    sed "s|$2|$3|" auditcore.c > $T/mut/auditcore.c
    if cmp -s auditcore.c $T/mut/auditcore.c; then echo "FAIL: мутация $1 не применилась (образец не найден)"; exit 1; fi
    $CC -O1 -Wall -DKIRA_HOST_TEST -I. -Iarch/x86_64 -no-pie -o $T/mut/audit_$1 tests/audit_test.c auditstore.c $T/mut/auditcore.c kfs.c sha256.c zstd.c store.c pk.c crypto.c
    if $T/mut/audit_$1 1 >/dev/null 2>&1; then echo "FAIL: мутант «$1» выжил: тест журнала не заметил, что защиты нет"; exit 1; fi
    echo "   мутант «$1» убит"
}
mut no-prev      'mcpy(r->prev, c->head, 32);' ''
mut no-hash      'sha256(r, sizeof \*r, out);' 'mzero(out, 32);'
mut no-chain     'else if (!meq(r.prev, v->head, 32)) fail(v, AUD_V_CHAIN, r.seq, off);' ';'
mut no-sig       'return ed25519_verify(pub, m, sizeof m, ((const aud_ckpt_t\*)r)->sig);' 'return 0;'
mut no-seq       'if (r.seq != v->next_seq) { fail(v, AUD_V_SEQ, v->next_seq, off); v->next_seq = r.seq; }' 'if (0) {}'
echo "== remote console (rcon): key derivation, password check, AUTH parsing =="
$CC $FLAGS -fsanitize=address,undefined -o $T/rcon tests/rcon_test.c rcon.c sha256.c
$T/rcon
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
