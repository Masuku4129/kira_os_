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
