# KIRA OS — Stage 0 component and workstream matrix (2026-11-14)

**Pinned source baseline:** `main` @ `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Status:** PRELIMINARY INVENTORY — not a component acceptance report.  
**Rule:** no area is marked READY in this matrix. Presence in source/build lists is not proof of runtime correctness or physical hardware support.

Status vocabulary:
- **PARTIAL** — production source and/or tests exist, but one or more required contracts, lifecycle, architecture parity or acceptance criteria remain open.
- **EXPERIMENTAL** — prototype/research path; not a production release guarantee.
- **BRANCH-ONLY** — exists in an unmerged PR/branch, not current main.
- **UNVERIFIED** — insufficient evidence to make a readiness claim.
- **BLOCKED** — a known safety/contract gap prevents enabling the relevant capability.

## 1. Fourteen master-plan areas A–N

| Area | Main-tree inventory | Preliminary status | Confirmed gaps / next audit |
|---|---|---|---|
| **A. Architecture & base platform** | `ARCHITECTURE.md`, `docs/MASTER_DEVELOPMENT_PLAN.md`, boot/CPU/memory/device contracts spread across source | PARTIAL | Current architecture baseline, versioned BootInfo, Native ABI, IPC, Device Model and service contracts are not all consolidated in main. ADR-0006 proposes the BootInfo contract; implementation and producer/consumer tests remain open. Target architecture package in PR #135 is BRANCH-ONLY and heavily diverged. F-004/F-005/F-006. |
| **B. KIRA Kernel** | `kernel.c`, `pmm.c`, `vmm.c`, `proc.c`, `sched.c`, `syscall.c`, `percpu.c`, `kobj.c`, `futex.c`, x86-64 and ARM64 backends | PARTIAL | Boot and memory lifecycle, SMP, process/thread teardown, syscalls, IPC and lock ordering need a line-by-line contract audit. F-001 and F-008 are confirmed stack lifecycle/hardening gaps; x86-64 PMM release (F-009) and TLB shootdown (F-010) are verified on unmerged Draft PRs; ARM64 reserved-page ownership and atomic block release (F-013/F-014) are verified 43/43 on PR #158 but remain absent from main until integration. |
| **C. KIRA Virtual Runtime / application ABI** | `userland/wasm.c`, `userland/wasm_jit_x64.h`, `userland/wasm_jit_x64_gen.h`, module build/tests | PARTIAL | Existing WASM is a bounded subset, not full modern WASM conformance. Parser/verifier, type/index bounds, quotas, cancellation, JIT W^X and host API need independent audit. KVM1 research implementation is in PR #134/branch-only and must not be treated as production KVR; ADR-0001 keeps WASM primary. |
| **D. KIRA Mind** | `mind.c`, `mindnet.c`, `mind_sys.c`, `experience.c`, `watchdog.c` | PARTIAL | Telemetry causality, policy authority, audit/rollback, lock/latency impact and all actions that can change scheduler/resources need explicit policy-boundary review. Tests exist but do not prove safe authority boundaries. |
| **E. KCC / IR / toolchain** | `kcc/`, `userland/kcc.c`, x86-64 and ARM64 build integration | PARTIAL | Compiler/IR/ABI/version contracts, relocations, diagnostics, reproducible bootstrap, memory-safety checks and backend parity remain to be specified and tested. |
| **F. Memory safety & execution security** | `caps.c`, `uaccess.c`, `trust.c`, `immune.c`, `kdrv.c`, W^X/NX mappings, package hashes | BLOCKED for DMA/MSI; otherwise PARTIAL | No IOMMU implementation is present in main. Draft PR #161 adds a current-main fail-closed gate for Ring 3 CAP_DMA/CAP_IRQ and verifies PCI quiesce readback before rejection; exact head `bf65ea8e302871eb847bdaa232f0462697b00512` passed 43/43 checks (run `43601644815`). Draft PR #165 separately sanitizes firmware-enabled PCI bus mastering on endpoints/host bridges while preserving only real forwarding bridges, but its exact-head CI exposes a `vm (usb-uhci)` boot regression (run `43602087949`) and is not accepted. Until fixes are integrated, main still has the original firmware handoff gap; real VT-d/SMMUv3 domains remain absent. Real per-device VT-d/SMMUv3 domains, secure boot, hardware anti-rollback anchor, DMA pin/unpin and complete teardown remain open. F-002, F-009 and F-010. |
| **G. Drivers & Driver Manager** | `pci.c`, `virtio.c`, `virtio_blk.c`, `nvme.c`, `ahci.c`, `usb.c`, `xhci.c`, `uhci.c`, `usbhid.c`, `usbhub.c`, `usbmsc.c`, `e1000.c`, `virtio_net.c`, `hda.c`, `audio.c`, `kdrv.c`, `userland/drvget.c`, `userland/drvman.c` | PARTIAL | Generic bus-independent Device Model/resource lifecycle not integrated. F-003 is under current-main Draft PR #162 with nonzero-root and subordinate-window tests; exact-head CI pending. DMA/MSI fail-closed policy is under Draft PR #161 verification; DMA/MSI remains blocked until real IOMMU. Driver source acquisition is not automatic universal Windows/Linux kernel-driver translation; compatibility must be explicitly scoped and certified. |
| **H. KiraFS & storage** | `kfs.c`, `store.c`, `vfs.c`, `fat.c`, `part.c`, `blk.c`, `virtio_blk.c`, `nvme.c`, `ahci.c` | PARTIAL | KiraFS and FAT/VFS paths have substantial host tests; block lifecycle, flush/barriers, concurrent access, crash recovery on real devices, encryption/key management and format migration remain open. |
| **I. Networking & communications** | `net.c`, `ip6.c`, `ipfrag.c`, `e1000.c`, `virtio_net.c`, userland network programs | PARTIAL | Audit protocol completeness, socket API, retransmission/congestion behavior, DNS/DHCP, firewall/capability policy, DMA-safe buffers, device reset and recovery. Wi-Fi/Bluetooth are not established as supported. |
| **J. Graphics & desktop** | `display.c`, `wm.c`, `gfx2d.c`, `gfxui.c`, `gfxsrv.c`, `fb.c`, `virtio_gpu.c`, `apps.c`, `explorer.c`, `dock.c` | PARTIAL | Compositor responsiveness, scheduling/locking, 2D/3D acceleration, device failure and input/resize regression need dedicated audit. Spatial Minimal is the target design, not a completed visual implementation. PR #150 has an exact-head host failure at `tests/wm_test.c:84`; it remains branch-only. |
| **K. System services & application platform** | `fsrv.c`, `progs.c`, `kobj.c`, `life.c`, `devhealth.c`, `kpar.c`, `fscmd.c` | PARTIAL | Many service functions remain linked into kernel image/context. A separate service manager with dependency graph, capability grants, lifecycle, crash recovery and stable System API is not yet established as a complete contract. |
| **L. Boot, install & update** | `build_clang.sh`, `build_arm64.sh`, `bootmap.c`, `mbinfo.c`, GRUB ISO creation, ARM64 image creation | PARTIAL | Current build path uses GRUB; unified BootInfo and full native boot-manager/installer/A-B update/recovery contracts remain open. x86-64 `--android` profile removes TESTPROGS; do not use it when a complete test/educational ISO is required. F-004/F-007. |
| **M. Architectures & real hardware** | `arch/x86_64/`, `arch/aarch64/`, separate build scripts and QEMU tests | PARTIAL / UNVERIFIED on physical hardware | Build/QEMU evidence is not physical-hardware support. x86-64 and ARM64 behavior must be checked separately for boot, interrupt, SMP, memory, PCI, DMA/IOMMU, drivers and recovery. |
| **N. Development tools, automation & quality** | `tests/`, `ci/`, `.github/workflows/ci.yml`, build scripts, host tests and QEMU matrix | PARTIAL | Exact-SHA evidence exists for individual PRs, not a blanket pass for main. Docs consistency, complete image manifests, file-loss guard and independent architecture checks are incomplete or branch-only. PR #148 status ledger is unmerged. |

## 2. Twelve parallel workstreams

| Stream | Scope | Preliminary status | Primary dependencies / immediate gate |
|---|---|---|---|
| **1. Kernel + processes + IPC + SMP** | Boot, exceptions, scheduler, process/thread lifecycle, IPC, synchronization | PARTIAL | A, B, F, M. Close stack lifecycle F-001/F-008; integrate verified x86/ARM64 PMM F-009/F-013/F-014 and TLB F-010; complete MMIO F-011 integration (PR #160 43/43 PASS) and verify process teardown/cross-CPU TLB. |
| **2. KVR + application ABI + compatibility runtimes** | WASM, Native ABI, KIRA VM decision, Linux/Windows app compatibility | PARTIAL / EXPERIMENTAL | A, B, C, F, K. WASM audit first; no second production VM ISA without ADR. Windows/Linux kernel drivers are not app-runtime compatibility. |
| **3. KCC + IR + backends + build pipeline** | Compiler, object/link formats, x86-64/AArch64 backends, bootstrap | PARTIAL | A, E, F, N. Freeze ABI/IR contracts before expanding compiler scope. |
| **4. Mind Core + specialized models + safe control** | Telemetry, prediction, diagnosis, bounded resource control | PARTIAL | A, B, D, F, N. Policy-mediated requests only; no arbitrary kernel memory/MMIO/lock access. |
| **5. Memory safety + IOMMU + secure boot/signatures** | Ownership, W^X, DMA isolation, signing, trust and rollback | BLOCKED for DMA/MSI | B, F, G, M. Fail closed before DMA/MSI; implement VT-d and SMMUv3 lifecycle; PMM transactional block release and ARM64 reserved ownership are verified on PR #154/#158 (unmerged); MMIO F-011 is verified 43/43 on PR #160 (unmerged); integrate before treating these as main baseline. |
| **6. Driver framework + drivers + Driver Manager** | Device discovery, registry, resources, bind/unbind, drivers, acquisition pipeline | PARTIAL | A, F, G, M. DEV-001, PCI range, IOMMU, stable handles and resource ownership. |
| **7. KiraFS + storage + recovery** | Filesystems, block layer, caching, flush, crash consistency, removable media | PARTIAL | B, F, G, H, L. Block/VFS contracts and recovery/fault injection. |
| **8. Network stack + network services** | Ethernet, IPv4/IPv6, transport, sockets, TLS, policy | PARTIAL | F, G, I, K. Audit protocol semantics, DMA-safe buffers and isolation. |
| **9. Desktop/compositor + toolkit + system apps** | Display, WM, input, toolkit, desktop, apps | PARTIAL; implementation paused behind foundation | B, F, G, J, K. Do not resume visual expansion before kernel/driver/security dependencies are stable. |
| **10. BIOS/UEFI + installer + update/recovery** | Firmware boot, handoff, ISO, install, atomic update and recovery | PARTIAL | A, B, F, L, M. BootInfo contract and full-profile artifact manifest. |
| **11. x86-64 + ARM64 + HAL** | CPU/platform abstraction, interrupt/timer/memory/device backends | PARTIAL; physical hardware UNVERIFIED | A, B, F, G, M. Exact architecture parity and QEMU evidence; then physical test matrix. |
| **12. CI, fuzzing, performance and release engineering** | Host tests, sanitizers, VM matrix, reproducible artifacts, release gates | PARTIAL | All areas. Every PASS exact-SHA; test profile and artifact manifest explicit; docs guard must be merged after review. |

## 3. Bottom-up integration gates

1. **Firmware and platform handoff:** BIOS/UEFI/Multiboot2/ARM64 FDT (close F-015 parser bounds), memory map, modules, framebuffer and versioned BootInfo (ADR-0006 proposed; implementation pending).
2. **CPU/HAL:** CPU features, GDT/IDT, exception frame, IRQ, timers, APIC/GIC, SMP and TLB operations.
3. **Device discovery and ownership:** PCI host range, bridges, stable handles, resources, DMA/MSI policy, IOMMU and device lifecycle.
4. **Physical/virtual memory:** PMM, VMM, page-table ownership, stack/heap guards, W^X/NX, TLB, DMA pin/unpin.
5. **Kernel execution:** processes/threads, scheduler, syscalls, user access, IPC, synchronization, teardown.
6. **Security policy:** capabilities, signatures, trust, isolation, anti-rollback, audit and recovery.
7. **Storage/network/services:** block/VFS/KiraFS, network protocols, service lifecycle and stable APIs.
8. **Execution/toolchain:** Native ABI, WASM/KVR, KCC, packages and later compatibility runtimes.
9. **Mind:** telemetry, diagnosis and policy-mediated actions.
10. **Graphics/user environment:** compositor, display, input, audio, apps and accessibility.
11. **Hypervisor:** separate hardware virtualization subsystem; do not conflate with WASM/KIRA VM.
12. **Release:** install/update/recovery, full test image, manifests, x86-64/ARM64 QEMU, Limbo and physical hardware matrix.

This is an integration order. All twelve workstreams remain visible in the project ledger; no area is silently dropped because another area is currently blocking.

## 4. Evidence and status rules

- Source/build presence => inventory evidence only.
- Host test => logic evidence only, not a hardware driver proof.
- x86-64 build => x86-64 compile evidence only.
- ARM64 build => ARM64 compile evidence only.
- QEMU => virtual hardware evidence only.
- Limbo => Limbo-specific x86 virtual hardware evidence only.
- Physical hardware => only the named, tested device/firmware configuration.
- A PR's CI result applies only to its exact tested SHA. It does not automatically transfer to main or dependent PRs.
- Unrun or unavailable checks remain UNVERIFIED.
- A release image must be checked against an explicit profile manifest; a successful ISO build alone does not prove all required components are present.

## 5. Next source-audit queue

1. Finish exact-head validation of PR #152 (F-001) and close any CI failures without weakening tests.
2. Complete platform handoff and early boot audit on x86-64 and ARM64.
3. Audit IRQ/timer/SMP/TLB operations and per-CPU state.
4. Audit PMM/VMM page-table ownership, reserved-page semantics, atomic block release on x86/ARM64, process address-space lifecycle, user-access boundaries and MMIO arithmetic.
5. Audit Device Model/PCI topology and DMA/MSI security before enabling device DMA.
6. Reconcile source inventory with build scripts and PR #148 file-loss manifest.
7. Produce a complete file-to-component-to-build-profile matrix for all 829 baseline blobs.


## 6. Status correction — 2026-11-14 follow-up

This section supersedes earlier PR #165/#166 status descriptions in the preliminary rows above.

- **F-002 / Area F + G:** PR #165 exact head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, run `43603442098`, completed successfully; all 30 returned jobs succeeded, including `vm (usb-uhci)`. Temporary trace cleanup produced head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`; exact-head CI for that cleanup is still UNVERIFIED. PR #161's Ring 3 DMA/MSI fail-closed gate and PR #165's firmware handoff sanitization are complementary, not IOMMU implementation. Real per-device VT-d/SMMUv3 remains BLOCKED.
- **F-016 / Area A + B + F + M:** PR #166 exact head `9d6443ff2db7195e8abf0c6d63a6257c1c38f229`, run `43602565437`, completed successfully; all 30 returned jobs succeeded. The firmware FDT reservation map is parsed and applied during ARM64 startup on this branch.
- **Integration dependency:** PR #166 must be combined with or follow PR #158's ARM64 PMM reserved-page ownership fix. Otherwise a later PMM free can release a late-reserved page. #158 exact head `70d0e89a65414818c22d61532f7ca4288c464d2c`, run `43592051859`, 43/43 PASS. Add an FDT-to-PMM integration regression before marking the whole memory-reservation lifecycle accepted.

All areas remain PARTIAL/BLOCKED/UNVERIFIED as previously stated; no area is promoted to READY by these branch-only results.


### Follow-up — FDT-to-PMM integration added to PR #166 (2026-11-14)

- PR #166 was expanded beyond the original FDT parser patch to carry the verified ARM64 PMM reserved-page ownership and atomic contiguous-release implementation from PR #158.
- Added an end-to-end host regression in `tests/fdt_test.c`: parse the FDT header reservation, apply it to ARM64 PMM, prove the reserved page cannot be freed, exhaust all remaining allocatable pages and prove the reserved page is never returned.
- Added the independent `tests/pmm_arm_test.c` suite and wired it into `tests/run_host_tests.sh`; the FDT test now links the actual ARM64 PMM implementation.
- Updated PR #166 description to state the combined scope and exact verification boundary. Current head: `5846df979218bd43e0edfdd456f7042d45e8c195`; exact-head workflow `43608492629` is pending. The earlier FDT-only head `9d6443ff2db7195e8abf0c6d63a6257c1c38f229` passed 29/30 returned jobs, but that result does not transfer to the expanded head.
- PR #158 remains open as the standalone source of the PMM change; do not merge both independently or duplicate the same implementation. Review the combined #166 diff and decide which PR is the integration vehicle after its exact-head CI completes.
- PR #165 diagnostic-cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb` has workflow `43608372681` queued/in progress; the previous pre-cleanup head passed all 30 returned jobs (not 29; the latest job listing contains 30 entries). No result is claimed for the cleanup head yet.


## New source finding F-017 — firmware DMA remains enabled during PCI BAR probing (2026-11-14)

**Severity:** HIGH; DMA safety remains BLOCKED until safe handoff or IOMMU isolation is established.

### Evidence
- Current-main PCI enumeration calls `probe_bars(d)` before any firmware `PCI_CMD_MASTER` sanitization.
- `probe_bars()` temporarily clears only `PCI_CMD_IO | PCI_CMD_MEM`, writes all ones to BAR registers to measure sizes, restores BARs and restores the original command word. If firmware left `PCI_CMD_MASTER` enabled, bus mastering remains enabled throughout this probe.
- Draft PR #165 currently clears Bus Master immediately after `probe_bars()`, before the device is published to a driver. This closes inherited permission at driver handoff, but not the earlier enumeration/probe window.
- The first experiment that cleared Bus Master before BAR probing caused a reproducible `vm (usb-uhci)` boot regression. Moving the clear after BAR probing made the pre-cleanup exact-head run pass; the safe quiesce ordering is still unresolved.
- No real per-device VT-d/SMMUv3 domain exists in main. PR #161 blocks Ring 3 DMA/MSI but does not protect firmware DMA before the kernel has taken ownership.

### Required closure
1. Establish a safe pre-probe handoff: either quiesce firmware-running devices with class-aware bounded procedures before clearing Bus Master, or bring up verified IOMMU isolation before probing devices that can DMA.
2. Preserve PCI bridge forwarding while ensuring endpoint DMA is denied until an owning driver has a verified DMA domain and explicit policy grant.
3. Add tests for command-register state before/during/after BAR probing, including a controller left active by firmware; test that no endpoint DMA can reach unowned memory.
4. Re-run the full QEMU matrix including UHCI, xHCI, NVMe, VirtIO and SMP on the exact combined head; physical hardware remains a separate required qualification.

### Cross-PR dependency
- PR #165 F-002 and PR #162 F-003 both modify `pci.c`. #162 fixes nonzero ECAM root-bus enumeration and parent-bounded subordinate windows; #165 changes Bus Master handoff. Their changes must be reconciled and tested together rather than accepted independently as the final PCI baseline.
- PR #165 pre-cleanup head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, workflow `43603442098`: all 30 returned jobs succeeded. Cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`, workflow `43608372681`: queued/in progress at last check.

F-017 is OPEN. PR #165 is a partial mitigation, not full DMA-handoff acceptance.


## New source finding F-018 — FDT v16 structure-size compatibility (2026-11-14)

**Severity:** MEDIUM; boot compatibility gap, not a memory-safety bypass once malformed blobs are rejected.

### Evidence
- The main FDT parser accepted `version >= 16` but unconditionally read header word 9 (`size_dt_struct`), which is introduced by FDT v17. FDT v16 has no such header field; its structure extent is derived from the offset ordering of the structure and strings blocks.
- The parser also used this value as the structure-block bound for every token traversal. There was no v16 fixture in the host test.

### Remediation under verification
- PR #166 now derives v16 structure size as `off_dt_strings - off_dt_struct` and uses the explicit `size_dt_struct` field only for v17+.
- Added a v16 fixture with the v17-only header word cleared; tests validate the tree root, compatible string and memory-reservation map.
- Current PR #166 head after combined FDT/PMM and v16 changes: `61b124d9a912b4c5cc2106ab6aadbf28f289c9ab`. Exact-head CI was not yet visible at last check; no PASS claimed.

### Required closure
- Host FDT parser tests under ASan/UBSan and complete ARM64/QEMU matrix pass on the exact head.
- Add or confirm at least one real bootloader-produced v16 DTB fixture if support for v16 is retained; otherwise explicitly reject v16 and document v17+ as the minimum supported FDT ABI.

F-018 remains OPEN pending exact-head verification and compatibility decision.


## New finding F-019 — userland artifact manifest omitted explicit UELF outputs (2026-11-14)

**Severity:** HIGH for release completeness / project-control integrity; no kernel runtime defect implied.

### Evidence
- The three-stage status manifest listed only names collected from `for app in ...; do` loops.
- Both build scripts also append five independently linked programs directly to `UELF`: `fetch`, `net6`, `kcc`, `drvget`, `drvman`. These were absent from the manifest's prior 20-name arrays.
- x86-64 actual UELF inventory is 25 apps; ARM64 actual UELF inventory is 25 apps. The x86 Android/release profile removes `TESTPROGS` and retains 13 apps.
- The previous CI checker repeated the same loop-only assumption and could not detect loss of one of the five explicit outputs.

### Remediation under verification — PR #148
- Updated `status_manifest.json` to record all 25 x86-64 and 25 ARM64 app outputs and the 13-app x86 Android/release profile.
- Updated `ci/check_project_status.py` to combine loop-built and literal `UELF` outputs, exclude the literal loop variable `$app`, compare exact lists and check all corresponding userland source files.
- Current PR #148 head `8f6657ac793ccfc8c2755fdafdecbdebf413ec5f`; exact-head workflow `43608902362` pending. No PASS claimed.
- PR #148 remains Draft and separate from main. Do not treat the corrected registry as active protection until the PR is reviewed and integrated.

F-019 remains OPEN pending exact-head CI and review of release artifact contents.


## 9. Full baseline file inventory — 2026-11-14

Created `docs/architecture/STAGE0_FILE_BUILD_MATRIX_2026-11-14.csv` from the recursive Git tree pinned to main SHA `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`. The snapshot contains exactly **829/829 tracked blobs**; the tree API reported `truncated=false`.

### Exact production build inputs
- x86-64 kernel common C sources: **98** = 80 shared with ARM64 + 18 x86-only.
- ARM64 kernel sources: **88** = 80 shared with x86-64 + 8 ARM64-only.
- x86-64 architecture source list: **16**.
- Userland UELF outputs: **25 x86-64 + 25 ARM64**; the x86 Android/release profile retains **13** after filtering `TESTPROGS`. The corrected source-of-truth and guard are in PR #148.

### File classification and unresolved coverage
- 365 KCC host fixtures; 64 host-test script references; 10 test helpers/fixtures needing linkage review.
- 22 architecture-support files need exact assembly/linker/build linkage review; 12 other architecture-support files are explicitly referenced by build scripts.
- 60 userland files are build outputs or linked support requiring file-level linkage review.
- 129 auxiliary source/support files are not in the pinned kernel source manifests and still require semantic ownership/build-profile review.
- Remaining documentation, CI, build tools, driver package inputs and packaged resources are categorized in the CSV.

The CSV is an **inventory baseline**, not a claim that every file has passed semantic review. Every row includes a primary area, provisional component, build profile, status and classification basis. Any `*_REVIEW` status remains open until its inclusion, dependency or intentional non-production role is verified against the build graph and source references.


## 10. Preserved branch-only work — priority review queue

The pinned all-branch audit from PR #148 records **302 refs**, **205 unique non-CI tree heads**, **722 unique branch-only paths**, and **77 non-CI branches without a PR**. No branch-only path is treated as integrated merely because it exists in a branch; all remain preserved until exact changes are reviewed.

Branch-only path classes:
- 201 source/runtime paths;
- 217 tests and test fixtures;
- 164 research/prototype files;
- 74 documentation files;
- 62 CI/build/release files;
- 4 assets/other files.

High-priority source families visible in the preserved branch inventory include:
- Versioned BootInfo producers/consumers: `bootinfo.c`, `bootinfo_mb2.c`, `bootinfo_fdt.c`, `bootinfo_uefi.c` and BIOS/UEFI architecture adapters.
- IOMMU and DMA isolation: `iommu_vtd*`, `iommu_arm*`, `drivers/iommu/amd_vi*`, plus tests.
- Device Model and bus routing: `device_model.*`, `drivers/bus/*`, `acpi_prt.*`, PCI routing and resource lifecycle.
- Driver trust/signatures/revocation/state transitions: `kdrv_sig*`, `kdrv_trust.h`, `kdrv_revocation*`, `kdrv_transition*`, signing tools and tests.
- GICv3, UEFI loaders, VMM policy and KIRA Mind synchronization/build subsystems.
- Release pipeline: image audit, release gate, package update, signed artifacts and reproducibility tests.

These are not approved designs or production-ready code by presence alone. They must be mapped to their exact source branch/PR, compared against the 829-file main baseline, reviewed for API/ABI conflicts, and tested before integration. Priority order is BootInfo/boot, PMM/IOMMU/DMA, Device Model/PCI, driver trust/lifecycle, then release and higher services.


### CI failure/fix record — PR #166 FDT v16 test (2026-11-14)

- Exact head `61b124d9a912b4c5cc2106ab6aadbf28f289c9ab`, workflow `43608737160`: `host-tests` failed at `ci/lint.sh` before compilation. Lint flagged the newly added fixture string `linux,dummy-virt` as a dummy marker. This was a test-fixture naming false positive, not a compiler or runtime-test failure; host tests were not reached.
- Removed the marker from the v16 fixture assertion; no lint rules were disabled or relaxed.
- Corrected branch head: `539285048cdd97f5d71ba5aefc9afda4058b7512`. Fresh exact-head workflow had not appeared at the last check. Do not carry forward any PASS from the failed head.


### CI completion correction — PR #165 diagnostic cleanup (2026-11-14)

- PR #165 cleanup exact head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`, workflow `43608372681`: **30/30 jobs completed successfully**, zero failed jobs. This supersedes earlier queued/in-progress notes for that head.
- Temporary boot/PCI diagnostics are removed; the earlier UHCI regression did not recur on this cleanup head.
- F-017 remains OPEN: Bus Master is still cleared after BAR probing, leaving the firmware-DMA window during BAR probing unresolved. PR #162's PCI host-bus/subordinate-window changes still need reconciliation with PR #165 in `pci.c`.
- This is a Draft branch result only; no main merge and no physical hardware claim.


## New finding F-020 — IOMMU and Device Model implementations are split across incompatible branch stacks (2026-11-14)

**Severity:** CRITICAL for DMA isolation / architecture integration. Main remains fail-closed for unisolated Ring 3 DMA, but does not contain production IOMMU domains or the common Device Model.

### IOMMU branch evidence
- PRs #115, #116 and #120 contain SMMUv3 address-width, FDT `iommu-map` requester-ID and stage-1 domain work. Their successful CI runs are on base `autodrv`, not current main.
- PR #125 attempts to combine SMMUv3 controller/domain integration, per-requester mapping, DMA map/unmap, PCI bus-master gating and VT-d domain work. It is also based on `autodrv`, not main.
- PR #125 exact head `cf35a78b385c0b9939702e19b097b01a4296e273`, workflow `43207054075`: **host-tests failed**, **ARM64 build failed**, and one QEMU VM scenario failed (`usb-uhci`). Host/ARM64 compiler error: `iommu_arm64.c:351: IOMMU_E_UNSUPPORTED undeclared` (the SMMU layer exposes `KIRA_SMMU_UNSUPPORTED`). VM failure: `usb-uhci`. Other build, audit, driver-E2E, GUI and KCC lanes passed.
- The PR #125 branch's `build_clang.sh` contains `iommu_vtd.c`, `iommu.c`, `drivers/iommu/intel_vtd.c`, `drivers/iommu/vtd_domain.c`, `kdrv_sig.c` and other branch-only sources. Current main's x86 build list does not contain those IOMMU files. The ARM64 branch builder explicitly includes `drivers/iommu/arm_smmu.c`, `arm_smmu_domain.c` and `iommu_arm64.c`, but this is not current-main integration.
- PR #161 is the current-main fail-closed Ring 3 policy transplant, exact head `29a654abe16fa8ee355a330b9a90dc4d9dfb2095`, workflow `43602754339` successful. It deliberately passes `device_isolated=0` because main has no implemented per-device IOMMU domain. This is a safe restriction, not IOMMU completion.
- PR #128's ARM64 DMA coherency fail-closed change passed CI on base `v46/release-completion`, not main. Its policy and tests must be reconciled with the current FDT/PMM and SMMU design.

### Device Model branch evidence
- PR #139 introduces bounded PCI host bus enumeration and FDT nonzero `bus-range` support, stacked on PR #135's target architecture branch. Exact head `ea83f83bdc59307bba22a6bd013b24c5c5c09731` has successful runs `43408282733`, `43407402297` and `43407396416`; an earlier run failed. It is not based on main.
- PR #141 adds the common bus-neutral device registry and PCI discovery adapter, stacked on #139. Exact head `cda2ba2b3b9432f0dcae1843bc331ac858c4b6e2` has five successful workflow runs; it is not based on main.
- PR #165 is a main-based PCI firmware Bus Master mitigation, but PR #162/#139 PCI bus-range changes overlap in `pci.c`. They must be reconciled with the Device Model adapter rather than merged independently.

### Integration order
1. Preserve PR #125's source stack and resolve its compile/UHCI failures on its original base before transplanting individual modules.
2. Rebase/adapt the SMMUv3 and VT-d APIs onto current main's contracts; no branch-only success counts as current-main evidence.
3. Integrate PCI host ranges and Device Model registry before enabling per-device IOMMU policy, so requester identity and resource ownership are stable.
4. Integrate PR #161 fail-closed KDRV policy with real domain readiness; keep DMA/MSI blocked until domain setup and teardown are verified.
5. Add end-to-end tests: firmware device -> PCI/Device Model identity -> IOMMU requester/stream -> domain map/unmap -> driver lifecycle -> fault/quarantine/teardown.
6. Keep x86-64 VT-d and ARM64 SMMUv3 separate backends behind one documented DMA/IOMMU contract; require exact-head host, x86, ARM64 and QEMU checks.

F-020 remains OPEN. No SMMU/VT-d code is considered integrated into main; do not rewrite or discard the branch-only implementations.


### CI failure/fix record — PR #148 userland inventory checker (2026-11-14)

- Exact head `8f6657ac793ccfc8c2755fdafdecbdebf413ec5f`, workflow `43608902362`: host-tests failed with Python `SyntaxError: unterminated string literal` in the new `android_release_app_names()` function; the host suite did not run.
- Restored the complete TESTPROGS regex, missing guard and return expression. No test was bypassed.
- Corrected PR #148 head: `2bbcef9eb86a43919099d2f26eb8af817c4c893b`. Fresh exact-head CI has not yet appeared; do not count the failed head as verified.


### F-004 branch-preservation follow-up — BootInfo and firmware loaders

- The unified handoff is not absent from project history: it exists in preserved branch stacks and must be reused rather than recreated.
- PR #12 (`v46/release-completion`, base `v45/overhaul`, head `c3342321667045a29b3c7e4726a31b926117a9b1`) adds `bootinfo.c`, `bootinfo_mb2.c`, `bootinfo_fdt.c`, BootInfo tests, ARM64 PMM, and related build/security/release changes. Its two recorded CI runs `43236520117` and `43236520194` succeeded, but this branch is not based on main.
- PR #46 (`v46/unified-release-integration`, base `v46/kdrv-catalogue-signature-policy`, head `dacc04d0dafd507ef0721698b5e9c2512f1947a5`) adds UEFI BootInfo, BIOS/UEFI boot composition, UEFI builder/tests, IOMMU modules, driver trust/signatures and broad release integration. No exact-head workflow run was found for #46.
- PR #45's UEFI ELF loader hardening is a separate branch on the same v46 stack; its recorded run `42385882597` failed. PRs #60/#61 preserve UEFI command line/modules and initrd loading on base `autodrv` and each passed CI; PR #130 adds module-to-usable-RAM validation on the v46 release branch and is closed there. None of these results are current-main acceptance.
- Integration rule: retain BootInfo v1 ABI and producer tests from the v46/autodrv work; compare the exact structures and ownership rules against proposed ADR-0006; transplant one adapter at a time onto current main; do not replace working MB2/FDT paths until the common consumer and PMM reservation ledger pass both-architecture boot tests.


## 11. Current-main kernel remediation stack — verified but not integrated

These are separate Draft PRs directly based on current main. Their exact-head CI has passed, but main remains unchanged:

| PR | Scope | Exact-head run | Result | Integration note |
|---|---|---:|---|---|
| #154 | Kernel stack slot ownership/generation, partial-map cleanup, x86 PMM atomic block release | `43590094470` | success | Overlaps #164 in `kstack.h`, `sched.c`, `sched.h`; integrate together. Also overlaps standalone #153 in `pmm.c`; do not merge both PMM implementations. |
| #155 | x86 TLB shootdown fail-closed behavior | `43590730265` | success | Separate SMP/VMM contract; re-run with the final combined memory branch. |
| #158 | ARM64 PMM reserved-page ownership, overflow-safe reserve, atomic contiguous release | `43592051859` | success | Required by FDT reservation-map application in #166; #166 now carries this implementation. |
| #161 | Ring 3 DMA/MSI denial until per-device IOMMU is verified | `43602754339` | success | Current-main security gate; intentionally blocks DMA/MSI because main has no domain implementation. |
| #164 | Remove heap-stack fallback, zero new stacks, fail closed on guarded stack exhaustion | `43600699384` | success | Overlaps #154's scheduler/stack ownership changes; combine and re-run together. |
| #165 | PCI firmware Bus Master handoff mitigation, diagnostics removed | `43608372681` | 30/30 success | F-017 remains open during BAR probing; reconcile with PCI range #139/#162. |
| #166 | FDT reservation map + ARM64 PMM integration + v16 parser | current head `539285048cdd97f5d71ba5aefc9afda4058b7512` | pending | Previous expanded head failed lint only; corrected head requires exact CI. |

### Required integration sequence
1. Combine #154 + #164 in one scheduler/stack ownership change; keep #153 as an overlapping alternate, not a second independent merge.
2. Integrate #155 against the final VMM/stack baseline and verify cross-CPU shootdown failure semantics.
3. Integrate #158 through #166's FDT-to-PMM test; ensure permanent firmware reservations cannot be freed.
4. Keep #161's DMA/MSI denial active until real VT-d/SMMUv3 domains are integrated and per-device readiness is proven.
5. Reconcile #139/#141 PCI identity/range work with #165's firmware Bus Master logic before accepting Device Model/PCI.
6. Run the full exact-head host, x86-64, ARM64 and QEMU matrix after all overlapping changes are combined. Individual green PRs do not prove the combined tree.


### CI failure/fix record — PR #148 duplicate checker tail (2026-11-14)

- Exact head `2bbcef9eb86a43919099d2f26eb8af817c4c893b`, workflow `43609381410`: host-tests failed with `SyntaxError: unmatched ')'` at line 211. A duplicated stale tail of the previous checker implementation remained after the final success messages.
- Removed the entire duplicate tail; the single checker now ends after the four success messages. No checks were bypassed.
- Corrected PR #148 head: `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`. Fresh exact-head CI has not yet appeared; no PASS claimed.


## 12. Test execution strategy — dependency-first, risk-tiered, artifact-driven

The audit will no longer treat a full multi-shard CI run as the first diagnostic step for every defect. Full CI remains mandatory at acceptance gates, but diagnosis and regression selection will be staged to reduce repeated wait time and improve fault localization.

### Test tiers

| Tier | Purpose | Required evidence | When to run |
|---|---|---|---|
| T0 — static/source | Catch API, ownership, arithmetic, ABI and build-manifest defects before boot | exact diff review, compiler/lint, dependency and source-provenance checks | every change |
| T1 — focused host | Fast deterministic unit/regression coverage | named host test, sanitizer where supported, negative cases | every logic change |
| T2 — focused VM | Reproduce the specific runtime contract on one ISA/device | QEMU scenario, serial log, result JSON, QEMU log, bounded timeout | after T1 passes |
| T3 — affected matrix | Exercise directly coupled components and adjacent drivers | selected QEMU shards, boot modes, SMP/device cases, both ISAs where shared code changes | before considering a fix stable |
| T4 — full acceptance | Detect cross-subsystem regressions and release/build omissions | exact-head full CI, all host/build/ARM64/UEFI/QEMU jobs, artifact audits | only after T0–T3 pass and before integration acceptance |
| T5 — external qualification | Validate platform behavior CI cannot prove | Limbo on user's phone; later real x86/ARM hardware and firmware combinations | after software gates, with limitations recorded |

### Dependency graph and test selection
- Each finding gets one owner component, upstream prerequisites, downstream dependants, exact source paths, test IDs and acceptance evidence.
- A change to a shared contract (BootInfo, PCI config access, PMM ownership, syscall/ABI, KDRV or DMA) automatically expands the affected set to every consumer on x86-64 and ARM64.
- A leaf-only change starts with T0/T1 and its focused T2 scenario; it does not trigger unrelated GUI/Mind/network suites until a dependency edge requires them.
- Any shared kernel or architecture boundary change requires at least one T2 case per affected ISA. A driver change also requires its class-specific scenario and one neighboring device scenario.
- Full T4 runs are performed on the final combined candidate, not separately on every overlapping PR. Exact SHA and artifact links are recorded; results never transfer between heads.

### Failure handling and evidence quality
1. Preserve the first failing artifact before reruns; do not overwrite or infer from a later timing-sensitive pass.
2. Classify failure as source/build, host regression, boot, runtime contract, harness/regex, timeout, or nondeterministic/timing-sensitive.
3. Inspect result.json and serial first; inspect QEMU log only around the relevant interval or with bounded streaming so multi-gigabyte logs do not exhaust memory.
4. Add the smallest instrumentation possible; instrumentation-only passes are diagnostic, not acceptance evidence.
5. Reproduce at least twice without instrumentation for timing-sensitive failures before closing them.
6. A test is PASS only when the exact head, job, scenario, result and expected assertion are known. A green sibling shard does not offset a failed shard.

### Integration gates
- Gate A: source/API compatibility and no unreviewed file loss.
- Gate B: focused tests pass for every changed contract.
- Gate C: affected x86-64/ARM64 runtime matrix passes.
- Gate D: overlapping PR changes are combined on one candidate and the complete matrix passes.
- Gate E: release image contents, manifest and boot modes are audited; only then may ISO generation be considered.

This strategy changes test scheduling, not the acceptance standard. No feature is considered complete because a reduced test set passed; reduced tiers are for faster diagnosis, while T4 remains mandatory for integration.


### Test inventory baseline — pinned main SHA 811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4

- Host runner tests/run_host_tests.sh: **47 named host suites**.
- QEMU scenario catalogue ci/scenarios.py: **38 named VM scenarios**.
- Coverage families include boot/storage, PMM/VMM, scheduler/context/FPU, SMP, ACPI/IOAPIC, PCI/virtio, KiraFS/VFS/FAT, USB UHCI/xHCI, network IPv4/IPv6/TLS, KCC/WASM, KIRA Mind, graphics/UI/apps, power, fault isolation and driver package/install/management.
- These are test entry-point counts, not proof of complete coverage. Each scenario must be mapped to ISA, build profile, hardware model, prerequisite services, assertions and known omissions. In particular, FDT property-name string-table termination is not covered by the existing FDT host fixture; see F-027.

### Exact-head correction — PR #166 FDT/ARM64 PMM gate (2026-11-14)

- PR #166 current head 4bc69a224887ead8d8d1737e782ed76cf4b8e95e, associated workflow 43610515593: **30/30 jobs succeeded**.
- This supersedes earlier pending notes for the prior corrected head. It verifies the current PR #166 implementation on its exact head, not main.
- The new FDT header reservation-map validation/application, block-overlap checks, v16 structure-size handling and reg index/address-overflow tests are present. F-027 remains open for bounded property-name lookup.


### BootInfo / firmware handoff branch map

| Source stack | Existing implementation | Exact evidence | Main integration state |
|---|---|---|---|
| PR #12, base v45/overhaul | KBI v1 common validator, MB2 producer, FDT producer, FDT/PMM reservation tests | runs 43236520117 and 43236520194 succeeded | not main-based; stronger module-RAM and FDT reservation semantics |
| PR #46, base v46/kdrv-catalogue-signature-policy | UEFI KBI producer, PE32+ loader, GOP/RSDP capture, ExitBootServices retry, direct KBI kernel entry | no exact-head CI run found | divergent stack; requires exact OVMF/combined verification |
| PR #45, base v46/kdrv-catalogue-signature-policy | UEFI ELF loader hardening | run 42385882597 failed | preserve and review failure before reuse |
| PR #60/#61, base autodrv | UEFI command line/module and initrd handoff preservation | runs 42779406776 and 42780403870 succeeded | closed PRs on divergent base |
| PR #130, base v46/release-completion | validate loader modules against declared physical RAM | runs 43164087429 and 43164087483 succeeded | closed PR on divergent base |

**Contract warning (F-028):** PR #12 and #46 both call the contract BootInfo v1, but their validators and FDT adapters differ. Canonicalize the stronger module-in-RAM and reservation behavior before adding the UEFI producer. Main still has direct MB2 and FDT paths; no common BootInfo integration is claimed.


### PCI/UHCI exact-head workflow terminal correction

- PR #125 exact head fee8c9aed19ffc37dab7fd599c2bd63bfc496924 / run 43613583149: 23 terminal jobs = 20 success, 1 failure (vm (0)), 2 skipped package jobs. All non-VM lanes and 9/10 VM shards passed; UHCI failed.
- PR #165 exact head 7ad140cfadb51c4dacf809eca77fff60ff7fe12c / run 43613588426: 29/30 success; UHCI failed.
- Neither candidate is green. Keep F-017/F-025 open and do not merge.


### CI execution topology — verified from pinned-main workflow and harness

| Lane | Runtime coverage | Execution details |
|---|---|---|
| Host | 47 named suites in tests/run_host_tests.sh | One host job; deterministic parser/model/regression suites, sanitizer where configured |
| x86-64 VM | 40 available scenarios = 38 explicit declarations + 2 generated stress-spam variants | Main workflow matrix runs 39; KVM enabled on GitHub runner; named scenario produces serial/result/artifacts |
| ARM64 VM | 16 scenarios | Separate ARM64 build and QEMU TCG harness; GICv2/v3, EL1/EL2, SMP, GUI, drivers, storage, network and DMA-noncoherent cases |
| Mind training | 1 long mind-train scenario | Separate manual workflow, about 25 minutes; not part of routine CI matrix |

The 38 explicit x86 declarations include mind-train; the two generated stress-spam variants are added at module load. The main workflow excludes mind-train and includes the two variants, hence 39 x86 jobs. Do not conflate the explicit declaration count, expanded catalogue count and workflow matrix count.

### Dependency-first regression graph (provisional; finalize after remaining source audit)

- Firmware/boot: BIOS/UEFI/MB2/FDT producers → BootInfo normalization → memory map and reservations → PMM → VMM/MMIO → heap → scheduler/SMP → process/syscall/IPC → system services and applications.
- Device path: ACPI/FDT/PCI config and host ranges → stable device identity/Device Model → resource ownership/BAR/IRQ → IOMMU domain and DMA mapping → driver authorization/binding → block/network/USB/audio services → applications.
- Storage path: PCI/virtio/NVMe/AHCI/USB storage drivers → block layer → KiraFS/store and FAT → VFS → file services and applications.
- Graphics path: framebuffer/virtio-gpu/input devices → display backend → graphics server/compositor → window manager/shell → desktop applications.
- Toolchain path: host KCC tests → target compiler/linker/ELF contract → in-OS KCC self-hosting → userland runtime and applications.
- Mind path: telemetry sources → quality/calibration/holdout tests → bounded policy proposal → capability/security gate → scheduler/resource API. Mind must not bypass device ownership, DMA isolation or kernel locks.

Regression selection follows dependency edges: a shared contract change runs the upstream producer test, consumer test, at least one runtime case per affected ISA, and one downstream smoke case. Only after these pass do we run the full matrix. This reduces redundant waits without lowering the final acceptance bar.


## 13. Three-bucket project preservation model

Use these buckets for every tracked source, test, resource and planned subsystem. This is the user's requested separation of stable project contents, active work and future work; it does not imply a component is functionally READY.

| Bucket | Meaning | Current contents / rule |
|---|---|---|
| **1. BASELINE — always preserve** | Files already tracked as production inputs, build support, tests, CI, docs or packaged resources. They remain present in every complete project update and applicable full build. | Confirmed x86/ARM64 kernel inputs, architecture sources, userland build outputs, boot/build scripts, host tests, KCC fixtures, CI tooling and packaged resources. Never delete or silently omit; validate per build profile. |
| **2. UNDER AUDIT / REWORK** | Existing files or APIs whose ownership, semantic role, build linkage, architecture parity or correctness is not yet fully verified; plus active Draft PR changes. | 224 source/support paths currently marked for review: 129 auxiliary sources, 22 architecture-support headers/files, 10 test helpers, 60 userland build/support files and 3 driver recipe/source files. Also the active kernel/PCI/FDT/PMM/GUI Draft PR stacks. |
| **3. PLANNED / BRANCH-ONLY** | Target capabilities not integrated into current main production build, including preserved implementation work and approved architecture targets. | Unified BootInfo/UEFI integration, full Device Model lifecycle, VT-d/SMMUv3 domains, driver trust/revocation lifecycle, installer/update/recovery and other target architecture items. Keep branch sources/tests intact; do not count as production support until reconciled and accepted. |

### Exact inventory counts feeding the buckets

- Pinned main tree: **829 tracked blobs** (tree API reported not truncated).
- Review-status queue: **224 paths** across the five review classes listed above.
- KCC test fixtures: **365** separately classified fixtures.
- Kernel inputs: x86-64 **98** common C sources (80 shared + 18 x86-specific); ARM64 **88** (80 shared + 8 ARM64-specific); x86 architecture source list **16**.
- Architecture build support: **12** files referenced by build scripts; boot build inputs **4**.
- These are file-role classifications, not test results. Baseline means preserve, not "bug-free"; planned/branch-only means preserve, not "integrated".


### Architecture-support header review notes

- Reviewed the 22 architecture-support headers and mapped the 12 assembly/linker support files to the build-support category.
- Preserve architecture-specific thread-entry ABI: x86-64 initial stack includes the return-address slot; ARM64 uses aligned SP and LR. Do not unify these by copying one architecture's stack convention.
- x86-64 host-test no-op CPU/FPU helpers are behind KIRA_HOST_TEST / KIRA_HOST_REAL_FPU guards. They are not production implementations.
- ARM64 crash-test mode 1 is explicitly unavailable (-5) because software cannot inject SError on the tested platform without RAS support; the ARM64 VM scenario checks this explicit limitation.
- FPU save-state layouts differ (x86-64 512 bytes; ARM64 528 bytes). Keep architecture-specific implementations and tests.
- No production stub was found in this reviewed header set; project-wide source stub audit remains open.


### Test-helper boundary verified

- All 10 TEST_HELPER_REVIEW paths were inspected. The apparent kernel API stub at tests/stub/kira.h is used only by the host paint/window test via the host runner's tests/stub include directory.
- Production build_clang.sh and build_arm64.sh do not include the tests tree; the real userland ABI header is userland/kira.h. USB/driver/KFS reference models and TLS/WASM helpers are test-only.
- Keep test doubles isolated to host tests; never substitute them for production driver, filesystem, kernel API or hardware implementations.


### Three-stage status registry already implemented in PR #148

The canonical project ledger for eventual integration is the PR #148 set:
- docs/PROJECT_STATUS/01_READY.md — baseline files and current published/build artifacts.
- docs/PROJECT_STATUS/02_IN_PROGRESS.md — preserved branch/PR work, tree differences and current blockers.
- docs/PROJECT_STATUS/03_PLANNED.md — future capabilities with completion criteria.
- docs/PROJECT_STATUS/status_manifest.json + ci/check_project_status.py — pinned inventory and automated loss/build-list guard.

Exact current PR #148 head bf35ca714526d38f5fdc3021c03b2bedf3ec2c99 passed workflow 43609627054 at **30/30**. This is the preferred source of truth for the user's three-section organization when integrating project controls; the Stage 0 three-bucket model above is a matching audit view, not a replacement. PR #148 remains Draft; main is unchanged.


### Boot sequence comparison — architecture-specific, contract-equivalent

| Dependency stage | x86-64 current main | ARM64 current main | Required verification |
|---|---|---|---|
| Firmware handoff | Multiboot2 relocation and early console | FDT validation and early PL011 discovery | bounded loader data, source provenance, malformed-input rejection |
| CPU/interrupt foundation | GDT, per-CPU, PIC/IDT, FPU; later APIC/IOAPIC | MMU, GICv2/v3, generic timer, PSCI | exception entry, timer tick, interrupt routing and SMP startup |
| Memory | MB2 map → PMM/VMM → null guard, SMEP/SMAP | FDT memory banks → MMU → ARM PMM and later reservations | no reserved page allocation, overflow-safe map, guard/fault tests |
| Scheduler/process | heap/scheduler/process/syscall before filesystem/Mind | heap/scheduler and early SMP before process/syscall | ABI, context/FPU state, affinity, teardown, IPC |
| Storage/services | experience → filesystem → life/Mind | PCI/storage → experience → filesystem → life/process/Mind | block readiness, KiraFS mount, persistence and failure recovery |
| Device/UX | display/input/USB/network/audio before final timer/APIC/SMP completion | PCI/RNG/services, then display/input/USB/network/audio after SMP | device identity, DMA/IRQ policy, UI and I/O smoke tests |

Do not require identical source order across ISAs. Require explicit prerequisite edges and equivalent user-visible contracts, then run the matching x86 and ARM64 scenario for each shared contract change.


### F-025 additional evidence — UHCI DMA storm

PR #165's full QEMU log (4,187,160,076 uncompressed bytes) contains more than 15.37 million rejected DMA reads at each of 0x0, 0x4, 0x8 and 0xC. The scenario uses the chipset UHCI controller with USB tablet and keyboard. This is consistent with an active UHCI frame-list fetch from an invalid/zero base, but the register values are not captured. Require USBCMD/USBSTS/FLBASEADD instrumentation and a bounded halt/reset experiment before declaring root cause.


### F-029 USB DMA ownership gate

The PCI enumeration Bus Master clear is not sufficient if a driver re-enables it too early:
- UHCI currently enables IO + MASTER before USBCMD stop/HCHalted and before replacing FRBASE.
- xHCI currently enables MEM + MASTER before BIOS handoff, stop/reset and replacement of DCBAAP/CRCR/event-ring state.
- Required driver lifecycle: enable register decode only → bounded firmware handoff/stop/reset → allocate and program kernel-owned DMA structures → grant MASTER only through verified DMA/IOMMU policy → start controller. Failure must leave MASTER denied and quarantine the device.
- Add QEMU scenarios for firmware-started controllers with stale/invalid DMA bases; the existing UHCI scenario currently fails earlier in PCI enumeration, so it cannot yet validate this downstream gate.


### F-030 cross-driver DMA grant matrix

| Driver | Current Bus Master grant | What happens afterward | Required order |
|---|---|---|---|
| VirtIO (block/net/GPU/input) | MEM + MASTER in virtio_pci_init | status reset, feature negotiation, queue allocation/programming | decode → reset/negotiate → queues → MASTER → DRIVER_OK |
| NVMe | MEM + MASTER at start of ctl_init | controller disable/RDY wait, admin queue allocation/programming | decode → stop → queues → MASTER → CC.EN |
| AHCI | MEM + MASTER at start of ctl_init | BIOS handoff, GHC reset, port stop/start and command structures | decode → handoff/reset → port structures → MASTER → FRE/ST |
| e1000 | MEM + MASTER at start of e1000_probe | device reset, RX/TX ring allocation/programming | decode → reset → rings → MASTER → RCTL/TCTL enable |
| HDA | MEM + MASTER at start of hda_probe | controller reset, CORB/RIRB/BDL allocation/programming | decode → reset → rings → MASTER → CORB/RIRB/stream run |
| AC'97 | IO + MASTER at start of ac97_probe | codec reset/init, DMA buffers and BDL allocation | IO decode → reset/init → BDL/buffers → MASTER → DMA run |
| UHCI/xHCI | IO/MEM + MASTER at start of controller start | stop/reset and replace firmware DMA pointers | decode → halt/reset → DMA structures → MASTER → RUN |

This is an ordering audit, not a claim that all devices fail today. Tests must model firmware-active devices and stale DMA addresses; the final gate is exact-head x86/ARM64 runtime evidence and IOMMU policy.


### CI diagnostic profile improvement (planned after Stage 0)

- Current ci/kira_vm.py always passes QEMU -d guest_errors and -D qemu.log. This is valuable for full evidence but the UHCI failure generated a 4.18 GB log in 90.9 seconds.
- The runner already supports per-scenario boot_timeout. Add an optional QEMU guest-error logging switch and a short 10–15 s PCI/USB diagnostic scenario after the audit. Routine focused checks should rely on serial/register assertions; turn guest_errors logging on only for a targeted evidence run.
- Keep the existing 90 s USB acceptance scenario and full T4 matrix unchanged. Faster diagnostic profile must never replace the acceptance test.


### F-031 KDRV startup DMA gate

Current KDRV start() turns on PCI Bus Master before spawning any driver with CAP_DMA or CAP_IRQ. That precedes device reset and DMA setup. The syscall permits later command-register writes, but there is no kernel-owned readiness state or IOMMU-domain verification. The contiguous identity-mapped DMA allocator tracks memory for the driver but does not constrain the hardware requester.

Required sequence: register DMA allocations → quiesce/reset controller → program validated DMA structures → verify IOMMU requester domain (or deny DMA) → grant Bus Master → verify readback. CAP_IRQ alone must not imply unrestricted DMA. Revoke and verify Bus Master on teardown/failure.


### F-032 Mind memory-pressure termination policy

Current ACT-mode memory policy: cache trim at 88%, heap-growth restriction at 93%, and at 97% potentially terminate the largest process if its heap is at least 16 MiB and 20% of RAM. The callback is direct proc_kill(). The reviewed contract does not distinguish essential services, foreground work, pending storage transactions, drivers, or restartability.

Before this can be treated as safe autonomous policy, add kernel-owned process criticality/protection classes, termination vetoes, reversible reclaim-first behavior and adversarial tests. The implemented Mind is presently a heuristic/profile-based resource controller; general AI reasoning and policy authority remain planned, not proven by mind.c.


### F-033 / F-034 storage closure gates

- FAT flush currently drops the block-device flush error. Propagate it and add failure injection to tests/fat_test.c; external-volume eject must fail closed when flush fails.
- VFS per-slot operations are locked, but vfs_any() and root vfs_list() inspect mount entries without slot locks. Snapshot state safely under locks, then call listing callbacks outside locks to avoid re-entrancy deadlock.
- KiraFS file replacement writes new data/metadata before switching the inode and freeing old blocks, but it is not a general journaled filesystem. Preserve its crash/recovery tests and do not imply full transactional semantics.


### Test and verification matrix

See [STAGE0_TEST_MATRIX_2026-11-14.md](STAGE0_TEST_MATRIX_2026-11-14.md) for exact existing host-test binaries, QEMU scenarios, ARM64 coverage, untested contracts for current findings and the proposed T0–T6 verification sequence. The proposed short QEMU diagnostic profile is not yet implemented.


### F-035 Driver Manager provenance and capability boundary

Current drvget is a genuine constrained source build flow (HTTPS/TLS, recipe, per-file scanning/pins, manual source-set hash confirmation, KCC compile, KDRV package). It does not translate arbitrary Linux/Windows binary drivers or ABIs.

Installed records currently preserve the recipe URL and declared license plus combined source-set/package/ELF hashes, but not each source file's URL, detected license and individual hash. Require a versioned per-file provenance manifest, recipe/file license consistency and signed trust metadata. The minimum 8-hex confirmation prefix is 32 bits and is not a signature; increase the default minimum and label its role accurately.


### F-035 / F-036 network and cryptography closure gates

- UDP owner validation and teardown must share the UDP table lock, and the slot must remain pinned across the whole syscall to prevent release/reuse after validation. Audit TCP/listener owner tables for the same lifecycle race. Add concurrent teardown/send/receive/reopen and packet-delivery tests.
- HKDF-Expand must reject output beyond 255 × 32 bytes, and all HKDF info/label/context limits must be explicit errors rather than silent truncation. Add boundary tests at 8160/8161 bytes. Audit ChaCha20/AES-GCM counter-length limits and ensure APIs can report invalid sizes.


### F-037 / F-038 TLS and certificate-validation gates

- TLS and X.509 parsing/verification must be reentrant per connection; remove mutable file-scope/static scratch state or enforce a documented and tested serialization contract. Test concurrent handshakes and certificate/OCSP verification.
- X.509 must reject every unknown critical extension, parse/enforce basicConstraints pathLenConstraint, and enforce keyUsage plus serverAuth EKU for TLS server identity. Add hostile certificate fixtures for each negative case.


### F-039 TCP lifecycle acceptance gate

TCP state and queue fields are changed under tlk but tcp_wait() predicates and several syscall checks access them without that lock. Timeout cleanup and handle reuse also need a consistent lifetime contract. Close/reset/receive, accept/unlisten, connect timeout and process teardown must be stress-tested with generation-tagged or pinned handles.


### F-040 KCC API concurrency acceptance gate

KCC's public compile API and internal preprocessing/parsing/IR/backend state are singleton globals. Parallel invocations are unsafe until all mutable state is moved into a per-compilation context. Until then, enforce a documented single-active-compile contract and add parallel x86-64/AArch64 isolation tests after refactoring.


### F-041 WASM hostile-input acceptance gate

Every parser read must be bounded by the active section/body end; signed/unsigned LEB128 must reject truncated and overlong encodings. Add exhaustive section/opcode truncation and oversized-length tests, then run interpreter and JIT paths under ASan/UBSan plus bounded malformed-module fuzzing.


### F-042 WASM import ABI acceptance gate

Reject out-of-range import type indices before storing them, validate every imported function against exact host signatures, and reject unsupported imported entry points. Test missing/out-of-range type sections, mismatched ABI signatures and more-than-four-argument imports under ASan/UBSan on interpreter and JIT paths.

### ELF loader hardening follow-up

`elf_parse()` already enforces ET_EXEC/architecture, bounded program-header table, page-aligned PT_LOAD, W^X, window and file bounds, non-overlapping pages, and executable entry point. Add explicit ELF header-size/version and PT_LOAD p_align validation plus malformed-header tests; this is a hardening gap, not currently classified as an exploitable mapping bypass.


### System-call and user-memory security audit coverage

Reviewed `cap_needed()` authorization mapping, capability assignment/trust checks, user-pointer range validation and fault-contained copy entry points. No new confirmed privilege escalation in this bounded dispatcher/uaccess review. Subsystem-specific syscall handlers still require independent validation; a clean dispatcher table does not close the UDP/TCP, FS, KDRV, IPC or graphics findings.


### F-043 Audio stream lifecycle acceptance gate

The audio syscall path currently validates reusable stream slots outside the mixer lock and does not pin a slot across a complete operation. Close/process teardown can race with write/gain, and gain is written without the lock used by the mixer reader. Require generation-tagged/pinned handles, closing/drain semantics, and one synchronization contract for format, resampler, gain and queue state. Test write/gain/close/process-exit/open/reuse concurrently; run TSan where supported.


### IPC trust and object-lifetime acceptance gate

Port and shared-memory object references, handle close, mapping retention and port waiter queueing have a coherent locking/refcount design in the reviewed paths. Before closing IPC security, document whether globally named objects are intentionally public or must be access-controlled. If protected, enforce owner/ACL or capability checks at open/send/receive/map; test guessed-name cross-process access, close-vs-in-flight operations, waiter cancellation, process exit and shared-memory unmap. Synchronize diagnostic counters such as object count and queued-message count.


### F-044 Input ring concurrency acceptance gate

Keyboard and mouse producer/consumer paths currently mix plain volatile reads with atomic release stores for consumer-owned tail indices. The diagnostic dropped counter is also read without synchronization. Require consistently atomic index access, explicit acquire/release semantics and atomic/locked diagnostics. Sequential decoder tests do not close this issue; add concurrent producer/consumer wraparound and overflow tests, TSan and QEMU SMP validation.


### F-045 Graphics event dequeue acceptance gate

Multiple threads in one process are supported, while graphics event dequeue uses an unprotected tail read/copy/store. Two consumers can duplicate delivery and fail to advance the queue correctly. Require per-app dequeue serialization or an enforced single-consumer contract, with multi-thread same-PID queue-wrap, copy-failure and teardown tests. Separately verify graphics app-slot lifetime across process exit and compositor reclamation.
