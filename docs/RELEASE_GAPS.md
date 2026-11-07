# KIRA OS — release blockers and acceptance criteria

**Snapshot:** 2026-11-07  
**Working base:** `v45/overhaul`  
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
- This is the common handoff contract only. The existing Multiboot2 entry has not yet been replaced by a KIRA-owned BIOS stage loader or UEFI application; no boot path is claimed independent yet.

Required:
- KIRA-owned BIOS boot path and UEFI application that populate BootInfo v1 and transfer control to the architecture entry point.
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

## 4. Driver package authenticity — BLOCKED (signature primitive already exists)
KDRV currently has SHA-256 integrity checks and explicit user confirmation. SHA-256 does not prove who published a package. Correction to the earlier inventory: the repository already contains a KIRA-owned Ed25519 verifier, `ed25519_verify()`, in `pk.c` / `pk.h`, with RFC 8032 vectors and malformed-signature tests in `tests/pk_test.c`. It is currently linked into selected userland TLS clients, but KDRV does not yet use it.

Required:
- Integrate the existing Ed25519 verifier into the kernel KDRV validation path and run the signature vectors in the kernel-compatible build.
- Define a canonical signed manifest covering key ID, target ABI/device IDs, package digest, version and validity interval.
- Provision trusted public keys through a kernel-owned trust store, with explicit fingerprint confirmation, rotation and revocation.
- Persist the highest accepted version per publisher/package and reject rollback.
- Fail closed for invalid signatures, unknown/revoked keys, expiry, malformed manifests and interrupted installs.
- UI must distinguish unsigned/hash-only from authenticated publisher packages.

## 5. DMA isolation — PARTIAL (fail-closed fallback implemented)
The current kernel still has no VT-d / AMD-Vi / ARM SMMU domain implementation. A DMA-capable device without an IOMMU domain can access physical memory outside a process's address-space boundary.

Implemented in the v46 working branch:
- KDRV refuses Ring 3 driver packages requesting CAP_DMA or CAP_IRQ unless the policy is explicitly told that this exact device has an isolated IOMMU domain.
- The current KDRV call site passes "not isolated"; therefore no untrusted Ring 3 driver can enable bus mastering or MSI.
- PCI configuration writes from Ring 3 no longer permit setting PCI_COMMAND_MASTER.
- Host policy tests cover MMIO-only acceptance and DMA/MSI rejection without isolation.
- The generic virtio DMA allocator records CPU pointer, device-visible address, exact byte size, page count and allocator owner; it rejects invalid, mismatched and duplicate releases.
- DMA backing memory is released outside the allocator registry spinlock. Virtqueue memory can be released only after the device status has reached zero following reset.
- virtio-blk failed-probe and request-timeout paths attempt to reset the device before releasing queue and bounce buffers. If reset cannot be confirmed, backing memory is deliberately retained to avoid device DMA into freed RAM.
- The ARM64 non-coherent DMA pool now uses a page bitmap and exact allocation records instead of a monotonic bump pointer; released extents can be reused. A boot-time self-test checks page alignment, zeroing, exact release and reuse. If it fails, DMA remains disabled rather than falling back to cacheable PMM pages.
- The platform DMA pool hook validates both CPU and device-visible page alignment and rejects address-range overflow.
- A validated DMA synchronization API now checks allocation ownership and byte ranges, including interior subranges. Queue rings, block buffers, RNG buffers, GPU command/response/framebuffer buffers, network buffers and input event buffers explicitly transfer ownership with sync-for-device/sync-for-CPU calls. ARM64 uses system-scope barriers for the Normal-NC pool; x86 relies on coherent DMA plus memory barriers.
- Host regressions cover DMA registry exhaustion/reuse, DMA sync range boundaries, repeated late virtio-blk probe failures, explicit shutdown before PCI model reset, and timeout cleanup/reprobe. CI run 686 exposed a segmentation fault in the virtio host test; the fixture and lifecycle were corrected in the subsequent validation snapshot, whose complete CI result is still pending.

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
