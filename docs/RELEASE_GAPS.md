# KIRA OS — release blockers and acceptance criteria

**Snapshot:** 2026-11-07  
**Working branch:** `v46/unified-release-integration`  
**Status:** engineering work in progress; this document is not a release declaration.

This file separates implemented mechanisms from the capabilities still required before the next combined update. Passing CI does not close a blocker unless the corresponding acceptance criteria are met.

## 1. Boot chain — BLOCKED
Current x86_64 image uses GRUB 2 / Multiboot2. GRUB is included in the ISO and is therefore a runtime boot-chain dependency.

Implemented in the v46 working branch:
- Added a fixed-layout, versioned KIRA BootInfo v1 contract (`bootinfo.h`) with physical-address semantics, memory-map records, framebuffer, ACPI/FDT pointers, modules, command line, architecture and boot mode.
- Added strict bounds/overflow/shape validation (`bootinfo.c`), compiled into the kernel, plus host regression tests.
- Normalized the current Multiboot2 handoff into BootInfo before PMM setup; PMM now reserves boot modules and framebuffer ranges even when firmware reports the containing RAM as usable, and keeps those reservations when high memory is mapped. Host regression cases cover low-memory modules and a high-memory framebuffer.
- ARM64 PMM now maintains a distinct reserved-page bitmap, avoids double-counting overlapping FDT RAM ranges, clamps reserve operations to managed memory, and validates contiguous frees atomically. A host regression test covers reserved-page freeing and overlapping regions.
- ARM64 FDT handoff is normalized into BootInfo v1; the ARM PMM consumes the normalized RAM map. The adapter carries framebuffer and bootargs, while firmware memreserve entries and /reserved-memory ranges are explicitly reserved before normal allocation.
- FDT parsing now validates the reservation-map terminator and block overlap, validates the complete structure-token stream and property names, bounds string comparisons, and rejects overflowing `reg` ranges.
- A native x86_64 UEFI application and adapter compile into PE32+ EFI/BOOT/BOOTX64.EFI (`build_uefi.sh`). **OVMF end-to-end boot is demonstrated**: `tests/uefi_boot_test.sh` (and CI job uefi-ovmf, including the GPT variant) boots a GPT + ESP (FAT32) disk made by `tools/mkuefi.py` (`kira-uefi.img`, no GRUB) under QEMU + OVMF up to the command line (BootInfo v1 accepted, native RSDP, Mind online). Not demonstrated: real hardware, Secure Boot, framebuffer formats beyond the OVMF GOP. Build note: gnu-efi must be built in call-wrapper mode as distro packages are (a fresh gnu-efi tree needs `GCCNEWENOUGH=0`), and `.rodata` must be kept by objcopy for newer linker scripts.
- The ISO still uses GRUB 2 / Multiboot2. - KIRA-owned BIOS chain now exists (`arch/x86_64/bios/stage1.asm`, `stage2.asm`, `tools/mkbios.py` -> `kira-bios.img`): A20, E820 -> Multiboot2-compatible info, EDD read of the ELF64 kernel via unreal mode to 16 MiB, CRC32 check, strict ELF64/segment/E820 validation, PT_LOAD copy + bss zeroing, entry with the GRUB contract. `tests/bios_boot_test.sh` boots it under SeaBIOS in QEMU (success, corrupted image, bad p_paddr, 16 MiB RAM, 64 MiB RAM). Not done: VBE framebuffer, modules/cmdline, partition table for USB-HDD firmware, real hardware. The ISO still uses GRUB; the GRUB-free images are `kira-bios.img` (BIOS) and `kira-uefi.img` (UEFI, GPT). No independent release boot path is claimed complete.

Required:
- Finish and integrate both KIRA-owned BIOS and UEFI boot paths so they populate BootInfo v1 and transfer control to the architecture entry point.
- BIOS + OVMF tests for low/high memory maps, framebuffer formats and malformed handoff data.
- Bounds, overflow, relocation and page-table validation.
- Binary inspection proving GRUB binaries/modules are absent from the release ISO.

GRUB fallback may remain during development, but it must not be silently described as removed.

## 2. KCC toolchain — BLOCKED
KCC is an in-OS C compiler for a subset of C. The full OS build still uses external Clang, LLD, NASM and wasm-ld.

Required:
- Explicitly documented supported C subset and diagnostics.
- Stage-1 / stage-2 self-hosting build with deterministic output comparison.
- ABI, relocation, object/link and assembler strategy for both x86_64 and AArch64.
- Tests that compile KCC with KCC and exercise representative kernel/userland translation units.

Do not call KCC a self-hosting system compiler until the bootstrap chain actually builds the required system components.

Status (evidence: `tests/kcc/selfhost.sh`, CI job kcc-tests): the x86-64 compiler itself bootstraps - stage1 (built by host-built stage0), stage2 and stage3 are byte-identical and stage2 reproduces stage0 output byte for byte on 51 corpus programs. Run under the host Kira loader only (not in QEMU/Kira, not on hardware). Still open: building the kernel/userland/boot code with KCC, assembler for .s files, multi-object linking, AArch64 self-hosting. The gate stays open.

## 3. RTL8139 driver provenance - CODE DONE, hardware evidence open
The FreeBSD-derived `drivers/rl` port is removed. `drivers/net/rtl8139.c|h` is a clean-room implementation written from the public Realtek programming specification (BSD-3-Clause, SPDX headers), with a Ring-3 wrapper `rtl8139_kdrv.c` and recipe `drivers/net/drvget.rec` (installed by drvget as `rl`).
Evidence: `tests/rtl8139_test.c` (chip model, ASan/UBSan, overrun/bad-header/TX round-robin/link cases) in `run_host_tests.sh`; `tests/drvget_e2e.sh` (on-device KCC build + install). Still open: QEMU `-device rtl8139` traffic test in CI and physical NIC testing (reported separately).

## 4. Driver package authenticity — BLOCKED (kernel verification and installer import implemented)

KDRV uses a detached Ed25519 signature over the complete .kdrv package digest, size, publisher key ID and validity interval. The kernel verifies it on every load against its trust-key table. Invalid, unknown, revoked and expired signatures fail closed. The installer now requests drvget.sig beside the recipe over HTTPS, verifies it against the exact locally built package before writing anything, and installs the signature with .kdrv and .rec. HTTP 404 is explicitly treated as a local-only install; any other fetch error or any present malformed/invalid signature aborts installation. The host driver E2E suite includes a malformed-signature rejection case.

Important distinction: kernel-side verification of an individual `.kdrv` package is implemented and tested separately from authentication of the catalogue that describes available drivers. The recent catalogue parser and Ed25519 regression additions do not yet make `drivers/index.txt` an authenticated source of authority, and they do not replace the kernel's per-package signature check. The real-Ed25519 catalogue test is awaiting a fresh complete CI result.

Still required:
- Persistent anti-rollback tied to a storage/firmware trust anchor that cannot be reverted merely by restoring an old disk image. A normal KiraFS file alone is not sufficient evidence of anti-rollback.
- A tested trust-key rotation and revocation lifecycle, including recovery after compromise of the current owner key; the current trust table is compiled into the kernel and has no authenticated update path.
- The authenticated catalogue parser now advances the supplied monotonic version-floor service only after the entire signed catalogue has parsed successfully; malformed catalogues and floor-storage failures are rejected. This is a module-level contract, not yet wired to a production firmware-backed monotonic store.
- The system catalogue now supports an explicit `publisher-required` policy bit. `drvget confirm` re-reads the system catalogue, re-pins the exact recipe and rejects HTTP 404 for a required signature; the KDRV record and kernel also retain/enforce the signed state so deleting `.sig` cannot downgrade the package. However, `drivers/index.txt` itself is not digitally signed, and the production RTL8139 entry remains optional until its `.sig` is signed with the offline publisher key. Signed catalogue authentication and production key publication remain open.
- Full QEMU end-to-end test with a valid publisher signature fetched over HTTPS, installed by drvget and then verified again by the kernel at driver load. The host installer E2E now includes a deterministic test-only trust anchor and a valid signed-package path (fetch -> verify -> install -> drvget verify), alongside malformed-signature rejection and unsigned local fallback; the new path still requires a fresh green CI run.

Unsigned local packages remain explicitly untrusted by publisher and require the existing local user confirmation. This is not publisher authentication.

## 5. DMA isolation — PARTIAL (fail-closed fallback implemented)
The current kernel does not yet have a production-ready, hardware-validated IOMMU domain path. The branch contains partial Intel VT-d code and ARM SMMUv3 controller/domain scaffolding, but neither is sufficient evidence of safe DMA translation. A DMA-capable device without a correctly configured IOMMU domain can access physical memory outside a process's address-space boundary.

Implemented in the v46 working branch:
- KDRV refuses Ring 3 driver packages requesting CAP_DMA or CAP_IRQ unless the policy is explicitly told that this exact device has an isolated IOMMU domain.
- The current KDRV call site passes "not isolated"; therefore no untrusted Ring 3 driver can enable bus mastering or MSI.
- PCI configuration writes from Ring 3 no longer permit setting PCI_COMMAND_MASTER.
- Host policy tests cover MMIO-only acceptance and DMA/MSI rejection without isolation.

Still required:
- Complete platform discovery and validation for x86 VT-d / AMD-Vi and ARM SMMU.
- Finish architecturally correct per-device domains, map/unmap, invalidation and fault reporting. SMMUv3 currently allocates a context table and stage-1 root but does not yet populate a validated Context Descriptor linking the translation root; do not enable device attachment or treat map/unmap as safe hardware translation.
- Add SMMUv3 host regression coverage for prepare/allocation unwind (now added; CI result pending), followed by spec-derived tests for STE/CD encoding, StreamID ownership, CMDQ wrap/full, CMD_SYNC completion, timeout and teardown.
- DMA masks, cache maintenance and bounce-buffer fallback where appropriate.
- Negative hardware-model tests proving a device cannot DMA outside its assigned pages.
- Trusted in-kernel DMA drivers need a separate platform-level isolation and memory ownership review.

This fallback is intentionally restrictive: DMA-backed external drivers will not start until isolation is implemented. QEMU modelling is not physical IOMMU validation.

## 6. Platform completeness — OPEN
- ACPI AML `_PRT` and legacy PCI INTx routing need completion or explicit unsupported-device reporting.
- ARM64 non-coherent DMA cache-maintenance paths need architecture-level review and physical board validation.
- Real PCI, USB, NIC, audio and storage devices remain unverified until physical hardware is available.

## 7. GUI and Mind — CONTINUOUS VALIDATION
The current CI has GUI regression and Mind quality jobs. These are useful but do not prove all real workloads.

Required:
- Long-running GUI tests with window movement/resizing, input, redraw, memory pressure and app crashes.
- Mind temporal holdout and policy regressions; check persistence, corrupted state recovery and resource bounds.
- Verify UI-reported Mind activity against actual telemetry and model updates.
- Keep test fixtures clearly separated from learned/user experience.

## 8. Release gate
The next combined user update is eligible only when:
1. Every blocker above is either implemented with evidence or explicitly deferred by a documented product decision. Core requirements (boot independence, original driver code, signed driver trust, safe DMA policy) are not silently waived.
2. x86_64 and ARM64 builds pass with `-Werror`.
3. Host, compiler, driver E2E, GUI, Mind and full QEMU matrices pass.
4. Final ISO and KiraFS receive binary-level dependency/provenance inspection.
5. Release archive, install instructions, manifest and SHA-256 are generated from the exact tested commit.
6. Report clearly separates CI/QEMU evidence from physical-hardware evidence.

## Tracking
- #5 release integration
- #6 BIOS/UEFI loader
- #7 KCC self-hosting
- #8 clean-room RTL8139
- #9 KDRV signatures and anti-rollback
- #10 IOMMU and DMA safety
- #11 ACPI, ARM DMA, GUI/Mind and release audit

## v46 preview: фактическое состояние (честно)
Сделано и проверено: KCC-оптимизатор (mem2reg, inlining, свёртка, dominators, regalloc), 458 тестов KCC, selfhost, сборки x86_64/ARM64 с -Werror, загрузка ISO в QEMU.
НЕ сделано: собственный BIOS stage2, драйвер RTL8139, подписи драйверов/anti-rollback, DMA-изоляция (VT-d/SMMU), ACPI _PRT. Релизный гейт остаётся открытым; сборка - PREVIEW.

## Аудит защиты памяти (по чтению кода, 2026-11-08; сборка и тесты при аудите не запускались)
Подтверждено кодом: NXE/WP/SMEP/UMIP/SMAP (x86), PAN (aarch64), guard-зоны стеков потоков, проверка границ и прав в uaccess по таблицам страниц, копирование в ядро до использования, W^X для программ, запрет CAP_DMA/CAP_IRQ драйверам Ring 3 без IOMMU, подпись Ed25519 на каждой загрузке драйвера.
Исправлено в этом обновлении: Bus Master и MSI/MSI-X больше не остаются включёнными прошивкой у драйвера без DMA/IRQ (kdrv.c); обнуление frames[] в shm_open (kobj.c); общий лимит записей нескольких MADT (acpi.c).
ОТКРЫТО (не заявляется как сделанное): IOMMU (VT-d/AMD-Vi/SMMU) не собирается и не вызывается, встроенные драйверы дают устройствам любые физические адреса; нет W^X для самого ядра (x86: первый гигабайт RW без NX; aarch64: без PXN/WXN) и KASLR; нет анти-отката и отзыва ключей драйверов (код счётчика в drivers/kdrv не собирается), ключ один и зашит; нет AML/_PRT/IVRS; на aarch64 нет ловушки NULL; приложения проверяются только SHA-256; USB-накопитель не read-only по умолчанию; векторы MSI не освобождаются (16); двойное освобождение стека потока не ловится.
