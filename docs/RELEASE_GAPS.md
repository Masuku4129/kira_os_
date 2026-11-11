# KIRA OS — release blockers and acceptance criteria

**Snapshot:** 2026-11-11  
**Working base:** `v45/overhaul`  
**Status:** engineering work in progress; this document is not a release declaration.

This file separates implemented mechanisms from the capabilities still required before the next combined update. Passing CI does not close a blocker unless the corresponding acceptance criteria are met. Evidence snapshot: CI run `42946165561` completed successfully on code commit `15c7bb4ca7fc656b9968b8faefe4e454c8bead63`: x86_64 and ARM64 `-Werror` builds, host tests, KCC suite, Driver E2E, GUI, Mind, 40 x86 QEMU scenarios and 13 ARM64 QEMU scenarios all passed. A later documentation-only commit updates KCC status; no code changed after the tested code commit.

## 1. Boot chain — BLOCKED
Current x86_64 image uses GRUB 2 / Multiboot2. GRUB is included in the ISO and is therefore a runtime boot-chain dependency.

Implemented in the v46 working branch:
- Added a fixed-layout, versioned KIRA BootInfo v1 contract (`bootinfo.h`) with physical-address semantics, memory-map records, framebuffer, ACPI/FDT pointers, modules, command line, architecture and boot mode.
- Added strict bounds/overflow/shape validation (`bootinfo.c`), compiled into the kernel, plus host regression tests.
- Normalized the current Multiboot2 handoff into BootInfo before PMM setup; ACPI RSDP tags are copied into aligned kernel-owned storage and exposed through the loader-neutral `rsdp_phys` field, so kernel ACPI discovery no longer depends on the Multiboot2 tag layout; PMM now reserves boot modules and framebuffer ranges even when firmware reports the containing RAM as usable, and keeps those reservations when high memory is mapped. Host regression cases cover low-memory modules and a high-memory framebuffer.
- ARM64 PMM now maintains a distinct reserved-page bitmap, avoids double-counting overlapping FDT RAM ranges, clamps reserve operations to managed memory, and validates contiguous frees atomically. A host regression test covers reserved-page freeing and overlapping regions.
- ARM64 FDT handoff is normalized into BootInfo v1; the ARM PMM consumes the normalized RAM map. The adapter carries framebuffer and bootargs, while firmware memreserve entries and /reserved-memory ranges are explicitly reserved before normal allocation. The non-coherent DMA pool selector also avoids firmware memreserve, fixed `/reserved-memory` child ranges and simple-framebuffer ranges; dynamic reserved-memory children without `reg`, missing/non-empty `/reserved-memory/ranges`, malformed cell widths, partial `reg` tuples, zero-sized entries, and malformed framebuffer ranges make the pool unavailable (fail closed), rather than guessing their eventual allocation. It searches downward in aligned extents and fails closed if no safe pool fits.
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
- Explicitly documented supported C subset and diagnostics in `docs/KCC.md`; the document now distinguishes the tested in-OS compiler from the missing self-hosting system toolchain.
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

## 4. Driver package authenticity — PARTIAL (canonical verifier and signer; loader not enforced)

KDRV now has a fixed 132-byte canonical v1 manifest (`kdrv_manifest.h`), an Ed25519 verifier (`kdrv_manifest.c`) and a host-side signing CLI (`tools/kdrvsign.py`). The manifest binds the complete package SHA-256, package version and ABI; its key ID is the first 16 bytes of SHA-256(public key). The signer accepts an external raw 32-byte Ed25519 seed, requires restrictive key-file permissions, writes atomically and never stores private keys in the repository. CI generates a disposable RFC 8032 test key and verifies signer/verifier interoperability, package binding, key fingerprint, revocation and hostile inputs.

This is not yet publisher authentication in the running OS: `pkg_load()` still validates the package against the local confirmation record and does not require or read a manifest. The v1 format also has no trusted timestamp or anti-rollback state.

Required:
- Integrate manifest verification into `pkg_load`, `verify`, installation, removal and recovery paths; bind the manifest to the exact `.kdrv` bytes.
- A compiled owner public-key trust anchor and `kdrv_manifest_verify_owner()` adapter now exist; its bytes are compared against `keys/KIRA_OWNER_ED25519_PUBLIC.hex` and its fingerprint is checked in host tests; the RFC test signer is rejected by this trust table. **The corresponding private-key possession/provisioning has not been verified**, no production package is signed with it, and the loader does not yet call this adapter. No production private key is present in Git or CI.
- Define key rotation/revocation delivery and trusted persistent state for anti-rollback. Do not claim rollback protection from mutable `/drv/*.rec`.
- Add trusted-time/expiry semantics only with a defensible clock source; v1 currently has no expiry fields.
- Update the driver UI to distinguish unsigned/hash-only from authenticated publisher packages.
- Enable fail-closed loader enforcement only after package migration and the signed fixture pipeline are complete.

`kdrv_manifest_verify()` is a standalone verified primitive, not a claim that installed drivers are currently authenticated.

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
- Host regressions cover DMA registry exhaustion/reuse, DMA sync range boundaries, repeated late virtio-blk probe failures, explicit shutdown before PCI model reset, and timeout cleanup/reprobe.
- virtio-rng, virtio-gpu, virtio-net and virtio-input now release partially allocated queues and DMA buffers on failed startup, but only after a confirmed device reset. Drivers retain ownership and refuse reprobe when reset cannot be confirmed.
- virtio-blk now clears device state only after all queue and bounce-buffer allocations have been returned; an unconfirmed reset prevents reuse of that device slot.
- Before a virtio reset, the common PCI teardown masks MSI and MSI-X and clears PCI bus mastering. The device status must then reach zero before any DMA allocation is returned to its allocator.
- CI run 718 exposed two virtio-blk lifecycle defects: the fixture reset a live model without shutdown, and a post-timeout write dereferenced the released bounce buffer. The fixture now shuts down before model reset, and all block I/O entry points reject operations after teardown. ARM64's forward declaration issue was also fixed. The later CI run `42946165561` passed host tests, x86_64/ARM64 builds, all 40 x86 QEMU scenarios and all 13 ARM64 QEMU scenarios, including `arm64-dma-nc`.

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

## 8. Supply-chain scanning — PARTIAL

Added in the v46 working branch:
- `.github/workflows/security.yml` runs GitHub CodeQL with `security-extended` for C/C++ (manual build through `build_clang.sh --no-run`) and Python (no-build extraction), on pull requests, pushes to main/stable, weekly schedule and manual dispatch.
- CodeQL workflow run `43016801131` completed successfully on source commit `4e60ad9df1953ca472ea604c11f9c5c1a1b105b4`; both C/C++ and Python analysis jobs succeeded. This is scan execution evidence, not a reviewed/zero-alert claim. The newer run for `374e88c11ebcd1f3967fc0a534d103cc03f467ce` is separate and must complete before its result is treated as current.
- `.github/dependabot.yml` enables weekly GitHub Actions dependency update checks.

Still missing:
- OSV/NVD/GHSA vulnerability scanning for applicable source/build dependencies and a reviewed applicability/remediation register.
- SPDX or CycloneDX SBOM for source inputs, toolchain/build environment, ISO and KiraFS image.
- Secret scanning and explicit review of workflow/action pinning and artifact provenance.
- Retrieval and triage of CodeQL alerts; a successful workflow does not imply zero findings.
- A release gate based on reviewed, applicable findings; do not fail releases on untriaged or irrelevant CVEs.

The current repository has no confirmed comprehensive third-party runtime dependency manifest. Do not invent package versions or claim an empty CVE result until the actual source/build inputs and final images have been inventoried.

## 9. Release gate
The next combined user update is eligible only when:
1. Every blocker above is either implemented with evidence or explicitly deferred by a documented product decision. Core requirements (boot independence, original driver code, signed driver trust, safe DMA policy) are not silently waived.
2. x86_64 and ARM64 builds pass with `-Werror`.
3. Host, compiler, driver E2E, GUI, Mind and full QEMU matrices pass on the same code commit. PR CI now runs the full x86 and ARM64 QEMU suites instead of skipping them.
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
