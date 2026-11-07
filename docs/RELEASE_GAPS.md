# KIRA OS — release blockers and acceptance criteria

**Snapshot:** 2026-11-07  
**Working base:** `v45/overhaul`  
**Status:** engineering work in progress; this document is not a release declaration.

This file separates implemented mechanisms from the capabilities still required before the next combined update. Passing CI does not close a blocker unless the corresponding acceptance criteria are met.

## 1. Boot chain — BLOCKED
Current x86_64 image uses GRUB 2 / Multiboot2. GRUB is included in the ISO and is therefore a runtime boot-chain dependency.

Required:
- KIRA-owned BIOS boot path and UEFI application.
- A versioned boot handoff ABI carrying memory map, framebuffer, ACPI RSDP, modules/initrd and boot mode.
- Bounds, overflow, relocation and page-table validation.
- BIOS + OVMF tests for low/high memory maps, framebuffer formats and malformed handoff data.
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

## 4. Driver package authenticity — BLOCKED
KDRV currently has SHA-256 integrity checks and explicit user confirmation. SHA-256 does not prove who published a package. The current `crypto.h` API exposes ChaCha20/Poly1305, X25519, AES-GCM and hash primitives, but no Ed25519 API was found in this snapshot.

Required:
- Implement and test an Ed25519 verifier, or document a separately reviewed KIRA-owned verifier before integration.
- Canonical signed manifest with key ID, target ABI/device IDs, package digest, version and validity interval.
- Trust-key provisioning, rotation/revocation and persistent anti-rollback state.
- Fail-closed handling for invalid signatures, unknown/revoked keys, expiry, malformed manifests and interrupted installs.
- UI must distinguish unsigned/hash-only from authenticated publisher packages.

## 5. DMA isolation — PARTIAL (fail-closed fallback implemented)
The current kernel still has no VT-d / AMD-Vi / ARM SMMU domain implementation. A DMA-capable device without an IOMMU domain can access physical memory outside a process's address-space boundary.

Implemented in the v46 working branch:
- KDRV refuses Ring 3 driver packages requesting CAP_DMA or CAP_IRQ unless the policy is explicitly told that this exact device has an isolated IOMMU domain.
- The current KDRV call site passes "not isolated"; therefore no untrusted Ring 3 driver can enable bus mastering or MSI.
- PCI configuration writes from Ring 3 no longer permit setting PCI_COMMAND_MASTER.
- Host policy tests cover MMIO-only acceptance and DMA/MSI rejection without isolation.

Still required:
- Platform discovery and validation for x86 VT-d / AMD-Vi and ARM SMMU.
- Per-device domains, map/unmap, invalidation and fault reporting.
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
