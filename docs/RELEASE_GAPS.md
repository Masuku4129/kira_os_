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
- A native x86_64 UEFI application and adapter now exist and compile into PE32+ EFI/BOOT/BOOTX64.EFI; the kernel ELF is emitted beside it. This is build evidence only: OVMF end-to-end boot has not yet been demonstrated, and the loader is not yet the ISO's independent default boot path.
- The ISO still uses GRUB 2 / Multiboot2. A KIRA-owned BIOS stage chain is not yet integrated. No independent BIOS/UEFI release boot path is claimed complete.

Required:
- Finish and integrate both KIRA-owned BIOS and UEFI boot paths so they populate BootInfo v1 and transfer control to the architecture entry point; the current UEFI binary still needs OVMF boot validation.
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

## 3. RTL8139 driver provenance — BLOCKED
`drivers/rl/rl.c` and `drivers/rl/if_rlreg.h` identify the current implementation as a port derived from FreeBSD `rl(4)`.

Required:
- Specification-based clean-room implementation with documented source provenance.
- PCI discovery, reset, DMA ring lifecycle, RX/TX, interrupts, link status, timeouts and malformed-frame handling.
- Fault-injection and QEMU integration tests; physical NIC testing reported separately.

Do not remove provenance notices without replacing the implementation and reviewing licensing.

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
