# KIRA OS — Stage 0 source audit findings (2026-11-14)

**Audit baseline:** `main` @ `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Status:** CONFIRMED SOURCE FINDINGS — runtime reproduction and regression tests are still required.  
**Parent record:** [Stage 0 repository audit](STAGE0_REPOSITORY_AUDIT_2026-11-14.md)

A source finding is not automatically a runtime failure. The findings below are confirmed by the source at the pinned SHA; test evidence is listed separately and is not implied.

## F-001 — Kernel stack allocator does not validate ownership or exact base

**Severity:** Critical memory-safety risk  
**Confidence:** High — direct source evidence in both ISA backends  
**Master-plan mapping:** B (kernel/process/thread lifecycle), F (memory safety), M (architecture parity), N (tests/quality); streams 1, 5, 11, 12.

### x86-64 evidence

In `vmm.c`, `vmm_kstack_free()` checks only that the supplied address is somewhere inside the kernel stack window, then unconditionally appends the derived slot to `ks_free[8192]` and increments `ks_nfree`. It does not require the exact base returned by the allocator, check that the slot was allocated, track active ownership, reject a duplicate free, or bound the append.

Source: [vmm.c at baseline SHA, lines 141–145](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/vmm.c#L141-L145).

### ARM64 evidence

In `arch/aarch64/main.c`, `vmm_kstack_free()` accepts any address inside the stack window and writes the free-list link directly into the supplied address. There is no exact-base or active-slot ownership check. This is a separate implementation of the same unsafe lifecycle.

Source: [arch/aarch64/main.c at baseline SHA, lines 353–361](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L353-L361).

### Consequence

A duplicate or interior-pointer release can corrupt the free list and make the same kernel stack slot available more than once. Two kernel threads could then receive overlapping stacks; repeated releases can also exceed the free-list capacity. This is a kernel memory-safety issue, not merely an accounting defect.

### Related work and evidence limits

- Draft PR [#132](https://github.com/Masuku4129/kira_os_/pull/132) contains a shared slot-ownership helper, x86-64 and ARM64 changes, and a host test for exact-base, bounds, duplicate-free, reuse and ARM64 slot-index limits.
- PR #132 is based on `v46/unified-release-integration`, not current main. It must not be merged or copied wholesale without reconciling its full diff and dependencies.
- At this audit snapshot, PR #132 head `2b8569ad1baf8596e48c81ebebcdcefe8fae859d` has no check-runs. The PR itself says no isolated allocator host test previously existed; the added helper test is not a complete integration test of the production allocator.
- Current main `tests/run_host_tests.sh` has no kernel-stack slot lifecycle test.
- Initial main-based remediation is in [PR #152](https://github.com/Masuku4129/kira_os_/pull/152). Its active-bitmap-only version passed host tests and architecture builds but had an `apps-drvman` QEMU regression.
- Combined [PR #154](https://github.com/Masuku4129/kira_os_/pull/154) also reproduced that regression, isolating it to F-001 stack-slot changes; PMM-only PR #153 passed the same scenario.
- Active-bit ownership alone does not prevent ABA: a delayed free from an old owner can arrive after the slot has been reused. The latest PR #154 adds a 64-bit generation per slot, passes it through `vmm_kstack_alloc/free` and stores it in `thread_t`; a stale owner token cannot free a new owner's slot.
- Latest generation-token head: `e299838c074604ee9cfeb734c819f84a2a5185ae`; exact-head CI run `43587913640` is in progress. No PASS is claimed yet.

### Additional x86-64 failure path

When a new stack mapping fails after one or more pages have already been mapped, `vmm_kstack_alloc()` returns without advancing `ks_used_hi`; already mapped pages remain. A later allocation retries the same virtual slot, but `map_in()` refuses to overwrite the existing PTE, so the slot can remain permanently unusable and its earlier pages remain allocated.

Source: [vmm.c at baseline SHA, lines 116–137](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/vmm.c#L116-L137).

**Required closure:** shared allocator-state logic for both ISAs; exact-base/active ownership; bounded free-list operations; safe partial-map failure handling; production allocator lifecycle tests including duplicate/interior/foreign frees, reuse, capacity, injected partial mapping failure and concurrent allocate/free; exact-head host, x86-64 and ARM64 CI. Do not claim closure from helper-only tests.

## F-002 — Ring 3 DMA/MSI can enable PCI bus mastering without per-device IOMMU isolation

**Severity:** Critical security boundary  
**Confidence:** High — direct source evidence and no IOMMU source module in main tree  
**Master-plan mapping:** A, B, F, G, M, N; streams 1, 5, 6, 11, 12.

### Evidence

- The current main tree contains no IOMMU/VT-d/SMMUv3 implementation files.
- In `kdrv.c`, driver startup rejects DMA only for the ARM64 non-coherent-pool case. It then enables `PCI_CMD_MASTER` whenever either `CAP_DMA` or `CAP_IRQ` is granted. MSI/MSI-X also depends on bus mastering.
- `sys_dma_alloc()` allocates contiguous physical pages and maps them into the Ring 3 driver; no per-device IOMMU domain is established in this path.
- ARM64 has a non-coherent DMA pool and GIC ITS/MSI allocation code. These are cache-coherency and interrupt-delivery mechanisms, not DMA address-space isolation.

Sources:
- [kdrv.c at baseline SHA, lines 206–224 and DMA allocation path](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/kdrv.c#L206-L224)
- [syscall.c at baseline SHA, capability gate](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/syscall.c#L119-L143)
- [ARM64 DMA pool and coherency selection](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L165-L261)
- [ARM64 GIC ITS/MSI implementation](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L460-L580)

### Consequence

A Ring 3 driver with DMA or interrupt rights can cause a bus-master-capable device to write to physical memory outside its assigned buffers if no hardware DMA isolation exists. A cache-incoherent DMA pool does not prevent arbitrary DMA.

### Related work and evidence limits

- [PR #136](https://github.com/Masuku4129/kira_os_/pull/136) proposes fail-closed denial of `CAP_DMA` and `CAP_IRQ` until per-device IOMMU isolation is confirmed. Its current head `761a212f3ab73c41f7a867c0337526b70cb8a174` has no check-runs.
- [PR #137](https://github.com/Masuku4129/kira_os_/pull/137) proposes clearing firmware-enabled PCI bus mastering before driver ownership. Its current head `d35c00069bf41bc0316a0dc743c7a9fe4a01be56` has no check-runs.
- These are separate protections: firmware handoff sanitization does not replace IOMMU isolation; a KDRV policy gate does not implement an IOMMU.
- Do not label DMA safe, isolated or production-ready until per-device domain setup, requester-ID mapping, attach/detach, invalidation, teardown, fault handling and adversarial tests are demonstrated on x86-64 and ARM64.

### Remediation on current-main draft branch

- Previous PR #136 was based on a stale main ancestor. Its changes have been transplanted to Draft PR #161 based directly on the Stage 0 main SHA.
- `kdrv_dma_policy_allow()` rejects `CAP_DMA` and `CAP_IRQ` while no per-device IOMMU domain is confirmed; current production call deliberately passes false.
- Before rejecting the package, `quiesce()` disables MSI/MSI-X and clears PCI IO/MEM/BUS_MASTER command bits.
- MMIO-only packages remain allowed. Host policy tests and x86-64/ARM64 QEMU scenarios cover denial and MMIO-only operation/cleanup.
- First PR #161 current-main run `43593017450` exposed three test/behavior defects and no PASS was claimed: `apps-drvman` lacked an attached RTL8139 endpoint, `drvman-edu` lost the last bound package identity after stop, and `drvget-rl` replaced its virtio control network with the DMA-gated RTL8139 on relaunch.
- Corrected by adding RTL8139 to the Driver Manager scenario, preserving the stopped package identity through `KDRV_SYS_PCILIST` and using that identity in Driver Manager, and retaining virtio-net alongside RTL8139 for HTTPS/DHCP control. Run `43593787066` confirmed RTL8139 denial/quiesce readback but exposed a global DHCP log-forbid that incorrectly rejected virtio-net DHCP; narrowed it to RTL8139 registration only. Run `43594252642` confirmed denial/quiesce but exposed a stale synchronous error expectation; Driver Manager accepts requests asynchronously and reports no-start plus kernel-log reason. Corrected the assertion to the actual async contract. Exact head `bf65ea8e302871eb847bdaa232f0462697b00512`, workflow `43601644815`: 43/43 checks PASS, including host policy tests, ARM64 build/QEMU driver scenarios and full x86-64 QEMU matrix. The first run exposed two stale ARM64 assertions; both were corrected without weakening production behavior.
- Complementary firmware handoff sanitization has been transplanted from stale PR #137 onto current-main Draft PR #165 (`626a21c029a203e3e4634886c4a894d27d8f0ce5`): clear firmware `PCI_CMD_MASTER` on endpoints, host bridges and non-forwarding bridge-class functions; preserve only real PCI-to-PCI and CardBus forwarding. Host regression added; exact-head CI pending.

**Required closure:** first establish and verify the fail-closed DMA/MSI policy plus firmware handoff sanitization; then implement and test real per-device IOMMU domain lifecycle for VT-d and SMMUv3 before allowing DMA/MSI. MMIO-only drivers remain a separate policy case.

## F-003 — PCI ECAM host bus range is stored but enumeration always starts at bus 0

**Severity:** High platform compatibility defect  
**Confidence:** High — direct source evidence  
**Master-plan mapping:** A, G, M, N; streams 5, 6, 11, 12.

### Evidence

- `pci_use_ecam()` records `bus_start` and `bus_end`.
- The ECAM backend returns `0xFFFFFFFF` for buses below `ecam_bus0`.
- `pci_init()` unconditionally calls `scan_bus(0)`; there is no range-aware root enumeration in current main.
- Therefore a valid ECAM host bridge whose first bus is nonzero is not enumerated correctly. Recursive bridge traversal also needs to be constrained to the host's declared range and each ancestor bridge's subordinate range.

Source: [pci.c at baseline SHA, lines 47–59 and 170–188](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/pci.c#L47-L59).

### Related work and evidence limits

- [PR #139](https://github.com/Masuku4129/kira_os_/pull/139) implements range-aware enumeration, bounded subordinate traversal and generation-safe handles, but it is stacked on PR #135 rather than main. Its current exact-head check-runs show 45/46 success with `vm (drv-edu)` failed.
- [PR #141](https://github.com/Masuku4129/kira_os_/pull/141) builds a common registry/PCI discovery adapter on top of PR #139; its 47/47 check-runs are on that dependent branch chain, not current main.
- Neither branch is an integrated main implementation. Their code and test assumptions must be reconciled against current main before use.

### Remediation on current-main draft branch

- Draft PR #162 adds `pci_init_range()`, makes ECAM `pci_init()` start at `bus_start`, and validates primary/secondary/subordinate bridge numbers while inheriting the parent subordinate limit.
- Host regressions cover nonzero root, nested windows, host-range escape, invalid primary, invalid range snapshot preservation, legacy bus-0 and actual ECAM wrapper.
- Draft PR #162 exact head `2e30dc5c9edff973a949d40dacbed18d2d666ab8`; workflow `43594509620`: **43/43 checks PASS**, including ASan/UBSan host coverage for nonzero root, nested subordinate windows, host-range escape, invalid primary, snapshot preservation, legacy bus 0 and the ECAM wrapper; x86-64/ARM64 builds and QEMU matrix also pass. Verified on unmerged Draft PR; main unchanged. ADR-0005 defines the target contract.

**Required closure:** exact-head host ASan/UBSan, x86-64/ARM64 builds and QEMU on final SHA; then integrate with current-main device discovery/registry work and verify ARM64 FDT bus-range handoff. Contract and acceptance gates are recorded in [ADR-0005](https://github.com/Masuku4129/kira_os_/blob/9d10d50095e406794f925a384e2e006db53599f8/docs/adr/ADR-0005-pci-host-bus-range-enumeration.md).

## F-004 — Boot handoff contract is not unified in current main

**Severity:** Architecture gap  
**Confidence:** Medium — source/tree inventory; detailed boot-path review remains open  
**Master-plan mapping:** A, B, L, M, N; streams 1, 10, 11, 12.

The x86-64 entry path receives Multiboot2 `magic` and `addr` in `kernel_main()`; ARM64 platform bring-up receives the FDT directly in `arch/aarch64/main.c`. No common `BootInfo` contract/module is present in the current main tree. This does not prove that the current boot paths fail; it means the cross-firmware/platform handoff contract is not yet unified and versioned in main.

Sources:
- [kernel.c at baseline SHA, kernel_main entry](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/kernel.c#L87-L105)
- [ARM64 main entry](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c)

### Contract proposal

- [ADR-0006 — Versioned cross-architecture BootInfo handoff](ADR-0006-versioned-bootinfo-handoff.md) is proposed.
- Defines immutable versioned header, normalized half-open physical ranges, memory ownership/reservation types, bounded optional records for FDT/ACPI/command line/modules/framebuffer/entropy/DMA/IOMMU, explicit source lifetimes, x86 Multiboot2 and ARM64 FDT adapters, and a future UEFI adapter.
- This is a contract decision proposal only. No BootInfo implementation has been added to main or accepted; current entry paths remain architecture-specific.

**Required closure:** approve/finalize the ABI, implement both architecture producers and common consumer without removing existing parsers prematurely, integrate reservation ledger before PMM free-list publication, then pass producer/consumer host tests plus x86-64 and ARM64 QEMU boot matrices.

## F-005 — Device Model is not yet a bus-independent lifecycle in main

**Severity:** Architecture gap / integration blocker  
**Confidence:** Medium-high — current source layout and PR dependency evidence  
**Master-plan mapping:** A, G, K, M, N; streams 5, 6, 11, 12.

Main contains a PCI device array and KDRV binding table, but no bus-independent registry with stable identity/generation, resource ownership and a generic driver lifecycle contract. PR #141 introduces a registry/PCI adapter, but depends on PR #139 and PR #135 and is not in main.

**Required closure:** define DEV-001 identity/generation and resource ownership, enumerate devices through bus adapters, bind/unbind drivers through a common lifecycle, and prove stale-reference, failure-unwind, hotplug/rescan and resource-revocation behavior.


## F-006 — Current architecture documentation contains stale statements that contradict main

**Severity:** High documentation/integration risk  
**Confidence:** High — direct contradiction between pinned docs, build inputs and test matrix  
**Master-plan mapping:** A, N; all 12 streams are affected because status decisions depend on accurate architecture records.

### Evidence

The current `ARCHITECTURE.md` is a long historical version narrative. Its early summary sections still state that the kernel supports only one CPU, USB is absent, NVMe is a future step, and no QEMU run has occurred. These statements contradict the same current main tree:
- x86-64 SMP implementation is in `arch/x86_64/smp.c`; scheduler/per-CPU code is built and CI has `smp`, `boot-8cpu` and `soak-8cpu` scenarios.
- USB stack and UHCI/xHCI drivers are built from `usb.c`, `usbhid.c`, `usbhub.c`, `usbmsc.c`, `uhci.c`, `xhci.c`; host tests model USB and CI has `usb-uhci`/`usb-xhci`.
- `nvme.c` is included in x86-64 and ARM64 build lists; CI has `boot-nvme`.
- `.github/workflows/ci.yml` runs the QEMU VM matrix.

Sources:
- [ARCHITECTURE.md historical claims, lines 48–54 and 103–108](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/ARCHITECTURE.md#L48-L54)
- [build_clang.sh source/build profile](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/build_clang.sh)
- [build_arm64.sh source/build profile](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/build_arm64.sh)
- [CI scenario matrix](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/.github/workflows/ci.yml)

### Consequence

An agent or developer following the stale summary may duplicate already-present work, misclassify working components as absent, or make unsafe changes based on an obsolete platform model. Conversely, later historical entries must not be treated as proof of present production readiness.

**Required closure:** label `ARCHITECTURE.md` as historical development log or rewrite its opening summary to current verified state; maintain a concise current architecture baseline, implementation matrix and contract registry. Every status must link to current source and exact-SHA tests.

## F-007 — x86-64 Android build profile deliberately omits test and educational programs

**Severity:** High release completeness risk  
**Confidence:** High — direct build-script logic  
**Master-plan mapping:** L, M, N; streams 2, 3, 10, 11, 12.

### Evidence

In current `build_clang.sh`, passing `--android` sets `RELEASE=1`; release mode filters the `TESTPROGS` list out of `UELF` before disk creation. The list includes `burn`, `mt`, `ipc`, `udpecho`, `melody`, `demo`, `fpu`, and multiple `atk-*` programs. The script comment explicitly says these test/educational programs are not placed on the user build disk, while CI builds the full set.

Source: [build_clang.sh at baseline SHA, lines 104–109](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/build_clang.sh#L104-L109).

### Consequence

An ISO made with `./build_clang.sh --android` is a release-filtered profile, not the complete CI/userland profile. It must not be described as containing every test and educational program. This is directly relevant to the project's no-loss/full-image requirement.

**Required closure:** define named build profiles with explicit manifests (full development/test image vs release-filtered image); make the chosen profile visible in build metadata; add a manifest diff test that proves which ELF files and resources are present; for the user's complete Limbo image, use the full profile and verify the ISO contents rather than relying on the filename or successful build.


## F-008 — Scheduler silently falls back to unguarded heap stacks and does not clear reused stacks

**Severity:** Medium-high kernel hardening gap  
**Confidence:** High — direct source evidence  
**Master-plan mapping:** B, F, N; streams 1, 5, 12.

### Evidence

- `sched.c:kstack_new()` first calls `vmm_kstack_alloc(SCHED_STACK_SIZE)`, but falls back to `kmalloc(SCHED_STACK_SIZE)` if the guarded stack window cannot provide a slot.
- `create_thread()` writes only the canary at the stack base; it does not clear the remaining stack bytes before constructing the initial context.
- The heap fallback is released through `kfree()`; it has no VMM guard page. The scheduler's canary is checked on context-switch paths, not as a page-level guard.

Sources:
- [sched.c stack allocator fallback, lines 291–299](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/sched.c#L291-L299)
- [sched.c thread stack initialization, lines 452–466](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/sched.c#L452-L466)
- [sched.c canary check, lines 300–305](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/sched.c#L300-L305)

### Consequence

When guarded stack allocation fails or the slot window is exhausted, a thread may still be created with an unguarded heap stack. This weakens the invariant that every kernel thread has a guard page. Reused stacks may also contain prior thread data until overwritten.

### Remediation on current-main draft branch

- Draft PR #164 removes the production heap fallback: when the guarded VMM stack window cannot allocate, thread creation fails closed.
- `create_thread()` now clears all stack bytes before writing the canary and publishing the thread.
- The host scheduler regression checks that bytes above the canary are zero on newly created stacks.
- Draft PR #164 exact head `3729347c19f8a5dad2ac5587f60876d8adca6cda`; workflow `43600699384`: **43/43 checks PASS**, including host scheduler regression, x86-64/ARM64 builds and QEMU matrix. The host test covers stack clearing; a dedicated production guarded-window exhaustion scenario remains a follow-up.

**Required closure:** exact-head host suite, x86-64/ARM64 builds and QEMU matrix; add/confirm a production test that exhausting the guarded stack window cannot produce a heap-backed kernel thread. Do not silently treat heap fallback as equivalent to guarded stacks.


## F-009 — PMM contiguous-block release is not atomic

**Severity:** High memory allocator integrity risk  
**Confidence:** High — direct source evidence; used by DMA and device-buffer paths  
**Master-plan mapping:** B, F, G, H, J, M, N; streams 1, 5, 6, 7, 9, 11, 12.

### Evidence

`pmm_free_contiguous(paddr, pages)` currently loops over the block and calls `pmm_free_page()` separately for every page. Each individual call acquires/releases the PMM lock and may succeed or fail independently. If one page is reserved, already freed, or otherwise invalid, earlier and later pages may still be freed; another CPU can allocate a just-freed page before the block release finishes.

The function therefore has neither all-or-nothing validation nor one lock covering the complete block transition.

Source: [pmm.c at baseline SHA, lines 357–363](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/pmm.c#L357-L363).

### Impacted callers

- `kdrv.c`: DMA block cleanup, allocation rollback and user mapping failure.
- `virtio.c`: contiguous DMA allocation rollback.
- `usb.c`: USB DMA allocation rollback, including below-4-GiB validation.
- `nvme.c`, `ahci.c`, `virtio_blk.c`, `xhci.c`, `uhci.c`: use the shared DMA allocators.

### Consequence

A failed release can leave a physically fragmented ownership state while the caller treats the range as one allocation. Because individual pages are returned under separate lock acquisitions, another CPU can reuse part of a block before the original release completes. This can become DMA use-after-free or allocator double ownership.

### Remediation implemented on draft branch

- Each shootdown publishes a monotonically increasing 64-bit request generation after writing `root/va/pages`.
- Each target CPU stores the generation it acknowledged only after completing its local TLB invalidation.
- The sender accepts only acknowledgements equal to the exact current generation; stale acknowledgements cannot satisfy a newer request.
- After 200 ms the sender emits a warning but keeps the request lock and backing-memory ownership; it does not return or allow another request to overwrite the payload until every target CPU responds. A permanently unresponsive CPU therefore blocks reclamation (fail-closed) instead of risking use-after-free.
- Added host regression cases for multi-CPU acknowledgement, partial acknowledgement, stale-generation rejection, zero generation and empty target mask.
- Exact-head CI is pending; this is not yet accepted as fixed.

### Required closure

Implement a single-lock transaction:
1. Validate nonzero page count, alignment, range/overflow and managed bounds.
2. Under one PMM lock, validate every page is allocated and not reserved before changing any bit.
3. If any page is invalid, return an error without freeing any page.
4. Otherwise clear all allocation bits and update `free_pages`/search hint once.
5. Add host tests for valid release, invalid middle page, double release, range overflow, zero length and concurrent allocation/release stress where supported.
6. Re-run all DMA/device scenarios and exact-SHA host/build/VM CI.


## F-010 — x86-64 TLB shootdown timeout can return before remote CPUs acknowledge invalidation

**Severity:** Critical SMP memory-safety risk  
**Confidence:** High — direct source and caller evidence  
**Master-plan mapping:** B, F, M, N; streams 1, 5, 11, 12.

### Evidence

- `arch/x86_64/smp.c::vmm_tlb_shootdown()` waits up to 200 ms for `sd_pending`; on timeout it logs a warning, breaks the wait loop, clears `sd_busy` and returns normally. The API returns `void`, so callers cannot detect failure.
- The pending mask and request parameters (`sd_root`, `sd_va`, `sd_pages`) are global. After timeout, the next shootdown can overwrite them while an old IPI is still pending. A late IPI may then flush the newer request's range and clear a bit in the newer pending mask, falsely acknowledging the wrong request.
- `proc_shm_unmap()` unmaps, calls the void shootdown, then releases the shared-memory object.
- `proc_unmap_pages()` unmaps and calls the void shootdown; DMA cleanup paths can then release the physical pages.
- ARM64 has a separate broadcast TLBI implementation using `vae1is/aside1is` and barriers; this specific timeout/global-request issue is in the x86-64 software IPI implementation.

Sources:
- [x86-64 shootdown and IPI handler, lines 123–166](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/x86_64/smp.c#L123-L166)
- [proc_shm_unmap(), lines 558–576](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/proc.c#L558-L576)
- [proc_unmap_pages(), lines 590–598](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/proc.c#L590-L598)
- [ARM64 broadcast TLBI, lines 172–181](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/vmm_user.c#L172-L181)

### Consequence

If a remote CPU does not acknowledge before timeout, its stale translation may remain usable while the caller frees/reuses the backing pages. Reusing the global request state before a late IPI arrives can also create a false acknowledgement. This can become use-after-free, cross-process memory exposure or DMA/page-table corruption.

### Required closure

1. Publish immutable request payload under one serialized request owner and assign a nonzero 64-bit generation.
2. Each target CPU acknowledges that exact generation only after completing local invalidation.
3. The sender may return and permit reclamation only after every target CPU in the snapshot acknowledges the generation.
4. A timeout is diagnostic, not permission to reclaim: warn, retain the request lock and backing-memory ownership, and continue waiting. A permanently failed CPU may stall reclamation/system operations; this deliberate liveness trade-off is safer than stale-translation use-after-free.
5. Any future CPU-offline recovery must prove that the CPU is stopped and cannot resume before it is removed from the target set; merely marking it failed is insufficient.
6. Add host generation/late-ack tests and QEMU SMP regression/fault injection; separately verify ARM64 broadcast TLBI completion and ASID lifecycle.

## F-011 — MMIO page-range alignment can overflow after the initial range check

**Severity:** Medium; malformed firmware/device BAR address can turn a rejected-range expectation into a wrapped page extent  
**Confidence:** High — direct arithmetic evidence  
**Master-plan mapping:** B, G, M, N; streams 1, 6, 9, 11, 12.

### Evidence

- `vmm_map_mmio()` checks only whether `pa + len` wraps.
- It then computes `last = (pa + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)`. The second addition can overflow even when the first `pa + len` did not.
- If the result wraps, `last - first` can become zero or a small wrapped extent; subsequent mapping and overlap accounting no longer describe the requested physical range.
- The path is used for device MMIO/BARs and other hardware register mappings. Physical addresses near the top of the 64-bit range are not valid on ordinary current systems, but malformed/untrusted firmware or device metadata must be rejected explicitly rather than relying on typical address width.
- ARM64 has a separate implementation that checks raw addition overflow and enforces its 48-bit mapping limit; this F-011 arithmetic defect is specific to the x86-64 helper path.

Sources:
- [vmm_map_mmio(), lines 169–185](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/vmm.c#L169-L185)
- [vmm_map_mmio declaration and contract](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/vmm.h)

### Remediation on draft branch

- Added checked half-open range helper `vmm_mmio_page_range()`: validates nonzero length, raw-end addition, page align-up addition, and strictly increasing aligned bounds.
- `vmm_map_mmio()` now obtains `first/last` only through the checked helper and reuses the validated raw end for overlap checks.
- Added host tests for normal alignment, cross-page ranges, exact final representable page, raw addition overflow and align-up overflow; wired them into `tests/run_host_tests.sh`.
- Original PR #156 run `43591102543` had one `drv-edu` log assertion failure; its serial artifact exposed the separate F-012 logging defect. PR #157 fixes F-012 and passed 43/43 on exact head `59b391696baabfd49f2ff48acf5aba2b9db538de` (run `43591626068`).
- Combined integration PR #160 is based on that verified UART head and includes F-011; exact head `d5628989db142212bf6af26510a06e5bd0e70c90`, workflow `43592802619`: 43/43 PASS, including host, x86-64, ARM64 and QEMU. Verified on unmerged Draft PR; main unchanged.

### Required closure

1. Run the exact-head host suite with ASan/UBSan and x86-64/ARM64 builds.
2. Confirm malformed 64-bit BAR/firmware ranges fail without changing MMIO allocator state.
3. Add a VM-side negative test if a suitable PCI/MMIO injection path exists.

## F-012 — complete driver-start log record is split at the per-CPU line-buffer limit

**Severity:** Low functional / medium observability and CI reliability  
**Confidence:** High — QEMU serial artifact and exact source path  
**Master-plan mapping:** B, G, N; streams 1, 6, 12.

### Evidence

- `linebuf.h` defines `LB_SIZE=192`; `lb_putc()` flushes immediately when the buffer reaches that size, even without a newline.
- `kdrv.c` assembles the full driver-start record into a local `ln[320]` buffer and then passes it to streaming `uart_print()`. The record is longer than 192 bytes.
- In PR #156 exact-head QEMU run `43591102543`, scenario `drv-edu` failed the expected log regex. The downloaded `vm-drv-edu` serial artifact shows the driver did start, MSI was enabled, DMA round-trip passed and the driver printed PASS; the kernel log record was split at the 192-byte boundary and another CPU's user log appeared between the chunks.
- This is not evidence of driver-start failure; it is a real log atomicity defect and a CI observability false negative.

Sources:
- [linebuf.h, LB_SIZE=192](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/linebuf.h)
- [linebuf.c, flush at LB_SIZE](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/linebuf.c)
- [kdrv.c, complete record assembly and uart_print](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/kdrv.c#L238-L253)

### Remediation on draft branch

- Added `uart_print_atomic()` for already assembled complete strings; it uses the existing uart/klog/early-console lock and emits the whole record in one critical section.
- Changed the driver-start record to use the atomic path; ordinary per-CPU streaming line buffers remain unchanged.
- Draft PR #157; exact-head CI pending.

### Required closure

1. Exact-head QEMU `drv-edu` must pass and serial artifact must contain one contiguous driver-start record.
2. Host/build/ARM64/full QEMU matrix must pass.
3. Keep streaming line-buffer semantics unchanged for ordinary multi-call output.

## F-013 — ARM64 PMM does not distinguish reserved pages from allocated pages

**Severity:** Critical memory-ownership violation  
**Confidence:** High — direct source evidence  
**Master-plan mapping:** B, F, M, N; streams 1, 5, 11, 12.

### Evidence

- `pmm_arm_init()` marked every non-free page as used, then `pmm_arm_reserve()` marked kernel/FDT/early-allocation/DMA ranges by setting the same used bit.
- `pmm_free_page()` checked only whether the page was used. Unlike x86-64 PMM, ARM64 had no reserved bitmap and therefore could not reject a free request for a reserved page.
- A stale/invalid free of a kernel, FDT, early page-table or DMA-pool page could make it allocatable again.

Source:
- [ARM64 PMM implementation](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/pmm_arm.c)

### Remediation on draft branch

- Add a separate reserved bitmap. Non-RAM holes begin unavailable/reserved; only validated RAM pages become allocatable.
- Reservation marks both reserved and used; page and block free reject reserved pages.
- Host regression covers kernel reservation and late reservation rejection.
- Draft PR #158 exact head `70d0e89a65414818c22d61532f7ca4288c464d2c`; workflow `43592051859`: 43/43 checks PASS, including host ASan/UBSan, x86-64 build, ARM64 build and QEMU matrix. Verified on unmerged Draft PR; main unchanged.

## F-014 — ARM64 PMM contiguous release is not atomic

**Severity:** High allocator-integrity risk  
**Confidence:** High — direct source evidence  
**Master-plan mapping:** B, F, G, H, M, N; streams 1, 5, 6, 7, 11, 12.

### Evidence

- ARM64 `pmm_free_contiguous()` called `pmm_free_page()` sequentially and returned at the first failed page.
- If a middle page was already free or otherwise invalid, all preceding pages had already been released. This violated all-or-nothing block ownership and differed from the x86-64 transactional implementation in PR #154.
- ARM64 contiguous allocation/free is used by page-run consumers and must have the same contract on both architectures.

Source:
- [ARM64 PMM contiguous free](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/pmm_arm.c)

### Remediation on draft branch

- Validate alignment, range bounds, page ownership and reserved status under one PMM lock.
- Only after every page validates, release the complete range and update the free count once.
- Add host regression for invalid middle page and unchanged free count on failure.
- Draft PR #158; exact-head CI pending.

## F-015 — FDT `reg` parser can wrap record indices and address ranges

**Severity:** High firmware-handoff input validation risk  
**Confidence:** High — direct parser arithmetic and caller usage  
**Master-plan mapping:** A, B, L, M, N; streams 1, 10, 11, 12.

### Evidence

- `fdt_reg()` checked `(uint64_t)(idx + 1) * stride`, but `idx + 1` was evaluated in 32-bit unsigned arithmetic before the cast. `UINT32_MAX` wrapped to zero and bypassed the record bounds check.
- The parser decoded 64-bit address and size cells but did not reject a nonzero-size range where `addr + size` wraps.
- ARM64 startup consumes `fdt_reg()` for RAM regions, UART, GIC, ECAM, framebuffer and other MMIO resources; the RAM path then derives `top`, `lo`, `hi`, DMA pool and PMM bounds from these values.

Source:
- [fdt_reg() implementation](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/fdt.c#L172-L190)
- [ARM64 RAM discovery consumes reg ranges](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L1067-L1082)

### Remediation on draft branch

- Compute record offset with 64-bit multiplication and subtraction-based bounds checks.
- Reject nonzero-size ranges whose half-open end would overflow uint64.
- Add host tests for `UINT32_MAX` record index and crafted wrapping 64-bit address/size.
- Draft PR #159 exact head `1a61bd5bdc11b3dd0338c3142113c963308598bf`; workflow `43592382535`: 43/43 checks PASS, including FDT host tests under ASan/UBSan, x86-64 build, ARM64 build and QEMU matrix. Verified on unmerged Draft PR; main unchanged.

### Required closure

1. Exact-head FDT host suite passes under sanitizers.
2. ARM64 build/QEMU matrix passes.
3. Audit remaining FDT-derived arithmetic (overlapping RAM ranges, total-size accounting, reserved-memory ranges and address-width limits) separately; this patch only closes the parser-level wrap.

## F-016 — ARM64 ignores the FDT header memory-reservation map

**Severity:** High memory ownership / firmware handoff risk  
**Confidence:** High — direct absence in parser API and ARM64 PMM initialization  
**Master-plan mapping:** A, B, F, L, M, N; streams 1, 6, 10, 11, 12.

### Evidence

- The FDT header includes `off_mem_rsvmap` at byte offset 16. The map is a sequence of 64-bit big-endian address/size pairs terminated by a zero/zero pair.
- Before this remediation, `fdt_valid()` did not validate the reserve-map offset, alignment, bounds, terminator or address/size overflow, and `fdt.h` exposed no iterator.
- ARM64 PMM startup reserved the kernel, DTB blob, early allocations, DMA pool and `/reserved-memory` nodes, but did not consume the header's independent reservation map.
- Firmware-reserved physical pages described only in `mem_rsvmap` could therefore be marked free and overwritten by general allocation.

Sources:
- [FDT header validation and parser](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/fdt.c)
- [ARM64 PMM initialization and reservations](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c)

### Remediation on current-main Draft PR

- PR #166 validates the reservation-map offset/alignment, bounded entry count, in-blob terminator, non-wrapping ranges and non-overlap with the structure/strings blocks.
- Adds `fdt_reserve_count()` and `fdt_reserve_get()`; ARM64 startup intersects each entry with all discovered RAM banks and reserves those pages before allocator exposure.
- Carries forward the independently verified F-015 `fdt_reg()` overflow guard.
- Host tests cover a valid range, invalid alignment, overlap, missing terminator and address+size wrap.
- Exact-head CI pending; no PASS claimed.

### Required closure

1. Host FDT suite passes under ASan/UBSan.
2. ARM64 build and QEMU matrix pass.
3. Confirm the PMM cannot allocate any page intersecting a firmware reservation; preserve the separate reserved-page ownership guarantees from F-013.

## Verification ledger

| Finding | Source reviewed | Runtime reproduction / branch evidence | Targeted regression evidence | Status |
|---|---|---|---|---|
| F-001 | x86-64 and ARM64 stack allocator | Combined PR #154 exact head `4dd1454093af90f7d569df03b4425650da8da8bc`; CI run `43590094470` | 43/43 checks PASS, including apps-drvman, host, ARM64 and QEMU matrix | FIX VERIFIED ON DRAFT BRANCH; main unchanged, merge/review pending |
| F-002 | KDRV, syscall capability gate, PCI firmware handoff, ARM64 DMA/MSI paths | Draft PR #161 current-main fail-closed CAP_DMA/CAP_IRQ gate + verified quiesce; Draft PR #165 clears inherited firmware bus mastering | PR #161 exact head `bf65ea8e302871eb847bdaa232f0462697b00512`, run `43601644815`: 43/43 PASS; PR #165 still fails `vm (usb-uhci)` on run `43602087949` and is under regression investigation | RING 3 DMA/MSI GATE VERIFIED ON DRAFT; FIRMWARE HANDOFF FIX NOT ACCEPTED; REAL IOMMU STILL ABSENT |
| F-003 | PCI ECAM backend and scan root | Draft PR #162 adds nonzero root and parent-bounded bridge traversal | Exact head `2e30dc5c9edff973a949d40dacbed18d2d666ab8`, run `43594509620`: 43/43 PASS incl. ASan/UBSan host, x86/ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-004 | x86-64 and ARM64 entry paths/tree | Not run in this session | No unified BootInfo test in main | ARCHITECTURE GAP |
| F-005 | PCI/KDRV structure and PR #141 dependency | Not run in this session | No generic registry test in main | ARCHITECTURE GAP |
| F-006 | ARCHITECTURE.md vs current sources/build/CI | Source/document comparison only | No docs consistency guard in main | CONFIRMED DOCUMENTATION DRIFT |
| F-007 | build_clang.sh release filtering | Static build-script review | No full-image manifest comparison in main | CONFIRMED PROFILE COMPLETENESS RISK |
| F-008 | sched.c stack fallback and initialization | Draft PR #164 removes heap fallback and clears reused stacks | Exact head `3729347c19f8a5dad2ac5587f60876d8adca6cda`, run `43600699384`: 43/43 PASS; guarded-window exhaustion-specific runtime test remains follow-up | VERIFIED ON DRAFT PR; main unchanged; one targeted coverage gap remains |
| F-010 | x86-64 TLB shootdown timeout and late IPI handling | Draft PR #155 exact head `f2df3472d78241e6287d5cc7601b5d3a632b3e52`; CI run `43590730265` | 43/43 checks PASS, including host, ARM64 and QEMU SMP matrix | FIX VERIFIED ON DRAFT BRANCH; main unchanged, review/merge pending |
| F-011 | MMIO page-range overflow during page align-up | Combined Draft PR #160 based on verified PR #157 UART fix | Workflow `43592802619`: 43/43 PASS incl. host, x86-64, ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-013 | ARM64 PMM reserved-page ownership | Draft PR #158 exact head `70d0e89a65414818c22d61532f7ca4288c464d2c` | Workflow `43592051859`: 43/43 PASS incl. host, ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-016 | FDT header `mem_rsvmap` and ARM64 PMM init | Draft PR #166 exact head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e` includes reserve-map parser, ARM64 integration, reserved-page ownership and atomic PMM release | Workflow `43610515593`: 30/30 jobs PASS, including host-tests, x86 build, ARM64 and all returned QEMU jobs | VERIFIED ON DRAFT PR; main unchanged, review/merge pending |
| F-015 | FDT reg index/address-size overflow | Draft PR #159 exact head `1a61bd5bdc11b3dd0338c3142113c963308598bf` | Workflow `43592382535`: 43/43 PASS incl. ASan/UBSan FDT tests, ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-014 | ARM64 PMM contiguous release is non-atomic | Draft PR #158 validates whole range under one lock | Workflow `43592051859`: 43/43 PASS incl. host, ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-012 | Driver-start log line split at 192-byte buffer boundary | QEMU serial artifact `43591102543` confirms driver success but interleaved log | Draft PR #157 exact head `59b391696baabfd49f2ff48acf5aba2b9db538de`; run `43591626068` 43/43 PASS including ARM64 and QEMU `drv-edu` | FIX VERIFIED ON DRAFT BRANCH; main unchanged, review/merge pending |
| F-009 | PMM contiguous block release | Source review and caller inventory | PR #153 exact-head CI 43/43 PASS; combined PR #154 integration pending | FIX VERIFIED ON DRAFT BRANCH; combined integration pending |

No code was changed on `main` by this audit. All implementation changes remain in separate unmerged Draft PRs. No test was executed locally. Exact-head CI evidence from other PRs is branch-only and is not transferred to `main`.


## Follow-up — F-002 / F-016 exact-head evidence and PMM dependency (2026-11-14)

- **F-002 firmware handoff:** PR #165 exact head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, workflow `43603442098`, completed successfully; all 30 returned jobs succeeded, including `vm (usb-uhci)`. This supersedes the earlier entry that described the UHCI regression as still open on the previous head. Temporary diagnostic traces were removed afterward; cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb` has no exact-head CI result yet. Keep F-002 branch-only until that cleanup head passes. Real per-device VT-d/SMMUv3 domains remain absent; PR #161's fail-closed Ring 3 DMA/MSI gate does not implement IOMMU.
- **F-016:** The latest observed PR #166 head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`, workflow `43610515593`, completed successfully: 30/30 jobs PASS. This head includes both the FDT reservation-map parser and the ARM64 reserved-page ownership/atomic contiguous-release changes; the earlier PMM dependency is integrated in this candidate. The PR remains draft and unmerged; main is unchanged.
- These are branch-level results only. No result is transferred to main, and no physical ARM64 hardware qualification is claimed.


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


### CI failure/fix record — PR #166 FDT v16 test (2026-11-14)

- Exact head `61b124d9a912b4c5cc2106ab6aadbf28f289c9ab`, workflow `43608737160`: `host-tests` failed at `ci/lint.sh` before compilation. Lint flagged the newly added fixture string `linux,dummy-virt` as a dummy marker. This was a test-fixture naming false positive, not a compiler or runtime-test failure; host tests were not reached.
- Removed the marker from the v16 fixture assertion; no lint rules were disabled or relaxed.
- Corrected branch head: `539285048cdd97f5d71ba5aefc9afda4058b7512`. Fresh exact-head workflow had not appeared at the last check. Do not carry forward any PASS from the failed head.


### CI completion correction — PR #165 diagnostic cleanup (2026-11-14)

- PR #165 cleanup exact head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`, workflow `43608372681`: **43/43 jobs completed successfully**, zero failed jobs. This supersedes earlier queued/in-progress notes for that head.
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


### CI failure/fix record — PR #148 duplicate checker tail (2026-11-14)

- Exact head `2bbcef9eb86a43919099d2f26eb8af817c4c893b`, workflow `43609381410`: host-tests failed with `SyntaxError: unmatched ')'` at line 211. A duplicated stale tail of the previous checker implementation remained after the final success messages.
- Removed the entire duplicate tail; the single checker now ends after the four success messages. No checks were bypassed.
- Corrected PR #148 head: `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`. Fresh exact-head CI has not yet appeared; no PASS claimed.



## New finding F-021 — branch-only IOMMU backends do not share one fail-closed policy (2026-11-14)

**Severity:** HIGH / integration blocker. Do not transplant the x86 VT-d and ARM64 SMMUv3 glue as one finished security contract.

### Evidence from PR #125 exact head `cf35a78b385c0b9939702e19b097b01a4296e273`
- The shared `iommu.h` describes pass-through as an explicit state and says devices become DMA-ready only after private translated domains are established.
- The x86 VT-d glue in `iommu.c` logs that absence of ACPI DMAR leaves devices with unrestricted DMA; `iommu_device_ready()` returns true when no unit covers a requester. A covered device whose unit is inactive is denied, but an uncovered device follows legacy unrestricted DMA.
- The ARM64 SMMUv3 glue has a different rule when `iommu-map` is present: it quiesces PCI endpoints before parsing; malformed maps and unsupported multiple targets keep bus mastering blocked; unknown/uncovered requesters remain blocked; explicit bypass STEs are transition/quarantine states and are not DMA-ready until a private stage-1 domain replaces bypass.
- ARM64 also preserves legacy behavior when the PCI host has no `iommu-map` property (`iommu_device_ready() == 1`). Thus the effective policy differs by platform and firmware description.
- Shared StreamIDs are rejected for per-function isolation; the ARM code intentionally blocks them rather than pretending they are independent requesters.
- PR #125 exact-head workflow `43207054075` failed host-tests, ARM64 build and QEMU `usb-uhci`; the compile error is `IOMMU_E_UNSUPPORTED` undeclared at `iommu_arm64.c:351` (the SMMU API exposes `KIRA_SMMU_UNSUPPORTED`). The UHCI VM failed to reach the shell prompt within 90 seconds. Other lanes passing does not resolve these failures.

### Required resolution
1. Define one architecture-neutral contract for: no IOMMU, no firmware mapping, uncovered requester, malformed firmware map, shared requester ID, pass-through, isolated domain, failed invalidation, and teardown.
2. Keep the current-main PR #161 DMA/MSI deny gate in force until that contract is implemented and tested end-to-end.
3. Reconcile whether legacy DMA is ever permitted; if permitted, make the exception explicit, measurable and limited to a documented trust policy rather than implicit `return 1` fallbacks.
4. Add matching x86-64 and ARM64 policy tests for each state, including no-DMAR/no-`iommu-map`, uncovered devices, and failure after a device has started.
5. Fix the PR #125 compile and UHCI failures on its own branch before any source is transplanted.

F-021 remains OPEN. This is an audit finding, not a claim that either backend is integrated into current main.


## FDT validator correction discovered during PR #166 review (2026-11-14)

- Review of the exact PR #166 source found the structure/string overlap test paired `off_dt_struct` with `size_dt_strings` and `off_dt_strings` with `size_dt_struct`. This inverted the two ranges and could miss an actual one-byte overlap.
- Corrected the range pairing in PR #166 and added a v17 malformed-blob regression fixture that places the strings block one byte inside the structure block.
- PR #166's earlier exact head `539285048cdd97f5d71ba5aefc9afda4058b7512` passed 43/43 jobs, but the corrected head is now `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`; its own CI is still required. The prior green result does not verify this new fix.


## F-022 — scheduler stack fallback weakens guard and ownership guarantees (2026-11-14)

**Severity:** HIGH. Integrated fix is under exact-head verification.

- PR #154 adds generation-token ownership for guarded VMM stack slots and atomic PMM contiguous release. PR #164 independently adds fail-closed allocation and stack-memory clearing, but overlaps `kstack.h`, `sched.c`, `sched.h`, and scheduler tests.
- PR #154's initial implementation still fell back to `kmalloc(SCHED_STACK_SIZE)` when guarded VMM allocation failed. That made stack-window exhaustion silently weaken the guard guarantee.
- Integrated the non-fallback policy and stack clearing into PR #154's generation-aware branch. New production threads now fail creation if guarded stack allocation fails; allocated stack memory is zeroed before publishing the thread and then receives the stack canary.
- Added host coverage that checks the canary and verifies the stack body is zero while excluding the architecture's initial context frame.
- Current integration candidate: PR #154 head `bfdffa64555ae221aa66c7c71efbb47f169c3f6a`. Exact-head workflow `43611654110` completed **43/43 jobs successfully**, including host-tests, x86 build, ARM64 build and the full VM matrix.
- The UART oversized-line regression discovered in the earlier `drv-edu` VM run was corrected in the same branch and the exact corrected head passed. No test assertion was weakened.
- PR #153 duplicates the atomic PMM change already included in #154; PR #164 overlaps scheduler changes. Do not merge the duplicates independently. Keep #154 as the canonical integration candidate for this set, pending independent source review and explicit merge decision.

F-022's integrated branch CI gate is CLOSED (43/43 PASS). Its architecture-wide acceptance remains OPEN until all per-ISA stack allocator paths and the final merge composition are independently reviewed.


## F-023 — UEFI BootInfo capabilities are split across incompatible branch stacks (2026-11-14)

**Severity:** HIGH / release integration blocker.

### Exact branch evidence
- Unified UEFI integration PR #46 is based on `v46/kdrv-catalogue-signature-policy`, head `dacc04d0dafd507ef0721698b5e9c2512f1947a5`. Its `kira_uefi_kbi_info` contains kernel, framebuffer and RSDP fields only. Its loader does not collect `LoadedImage.LoadOptions` or load `EFI/KIRA/INITRD.IMG`. Its KBI-to-Multiboot2 adapter emits only memory-map, optional framebuffer, and end tags.
- UEFI command-line and initrd support exists in PRs #60 and #61, both merged into the separate `autodrv` branch on 2026-11-10. PR #61 adds LoadOptions UTF-16-to-UTF-8 conversion, bounded optional INITRD.IMG loading below the 1-GiB bootstrap map, KBI module/cmdline fields, and PR #60's Multiboot2 module/cmdline tag serialization.
- Those commits are not part of PR #46's base or head. Their existence in `autodrv` does not prove the unified UEFI branch carries the features.
- PR #12 / `v46/release-completion` is another separate base stack; PR #130's stricter module-range validation was merged there, not into `main` or PR #46.

### Required integration
1. Select one canonical KBI v1 contract and transplant the UEFI module/LoadOptions implementation into the selected unified loader branch.
2. Test the entire path: EFI file + LoadOptions -> KBI blob -> kernel entry -> compatibility MB2 tags -> kernel parser/consumer. Loader-side log lines alone do not prove the kernel receives the data.
3. Reconcile the module memory-map classification with PR #130 before combining the validator and UEFI loader; see F-024.
4. Preserve explicit no-initrd/no-LoadOptions boot as a valid path and test it.

F-023 remains OPEN. No UEFI release-complete claim is justified from these divergent branches.


## F-024 — strict BootInfo module-in-RAM validation conflicts with UEFI LoaderData classification (2026-11-14)

**Severity:** HIGH / security integration blocker.

- PR #130 adds `module_in_ram()` to the KBI validator. It accepts module bytes only when the entire range is covered by sorted `KBI_MEM_USABLE` or `KBI_MEM_MODULE` entries; reserved, bootloader-owned, MMIO, and unmapped ranges are rejected.
- PR #61's UEFI loader allocates INITRD.IMG using `AllocatePages(..., EfiLoaderData, ...)`. Its UEFI memory-map adapter classifies `EfiLoaderData` as `KBI_MEM_BOOTLOADER`, and the module is not overlaid/reclassified as `KBI_MEM_MODULE` in the observed `uefi_kbi.c` descriptor conversion.
- Therefore, transplanting PR #130's validator unchanged into the PR #61 UEFI module path would reject the loader-created initrd range. Conversely, weakening the validator to accept arbitrary `KBI_MEM_BOOTLOADER` would broaden the trust boundary to all loader-owned memory.
- Required resolution: identify and reclassify only the exact module pages as `KBI_MEM_MODULE` in the KBI memory map (splitting descriptors as needed), or introduce an equally explicit validated module-ownership rule. Preserve all other LoaderData as reserved.
- Add an integration test where the module occupies a subrange of an EfiLoaderData descriptor, and tests for adjacent, partial, overlapping, and out-of-map module ranges.

F-024 remains OPEN. The PR #130 policy and PR #61 loader must not be combined until this contract is resolved.


## F-025 — PCI host-range bounds and firmware DMA cleanup were split across main-based PRs (2026-11-14)

**Severity:** HIGH / integration candidate now under exact-head CI.

- PR #162 (main-based) adds host bridge bus-range tracking, starts enumeration at ECAM bus_start, rejects invalid ECAM ranges, validates bridge primary/secondary/subordinate windows, and bounds nested bridge traversal.
- PR #165 (main-based) clears firmware Bus Master on host bridges, endpoints and non-forwarding functions, while preserving PCI-to-PCI and CardBus bridge functions. Its original implementation still performed BAR probing before clearing Bus Master.
- The two changes overlapped in `pci.c`, `pci.h`, and host test wiring. Integrated #162's bounded enumeration into #165's security branch, preserving the Bus Master policy. Both PCI regression suites now run from `tests/run_host_tests.sh`; the virtio model explicitly sets the primary bridge bus.
- Combined PR #165 head: `f986d3f84fdc6bac6dd72ce7d4fd6a76bc4d3caf`. No exact-head CI result is available yet. Previous PR #165 30/30 success belongs to the earlier head and is not evidence for this combined change.
- PR #162 remains a separate Draft source PR; do not merge it independently after this integration. PR #139's PCI/device-registry stack is still based on a separate architecture branch and needs later reconciliation.
- Existing F-017 remains open: firmware Bus Master is cleared only after BAR probing, leaving a DMA exposure window during probing. A previous pre-probe change caused a UHCI VM regression; the cause and a safe transition sequence remain unresolved.

F-025 remains OPEN pending exact-head CI and later PCI/device-model reconciliation. F-017 remains independently OPEN.


## FDT overlap fix — exact-head verification complete (2026-11-14)

- PR #166 corrected head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`, workflow `43610515593`: **43/43 jobs succeeded**.
- This exact run includes host-tests (including FDT v16 and one-byte overlap regression), x86 build, ARM64 build/scenarios, and all VM matrix scenarios including `usb-uhci`.
- This verifies the FDT validator correction and the FDT reservation-map/ARM64 PMM integration on the PR branch. It does not integrate the separate branch-only IOMMU backends or close F-021.



## F-026 — Device Model registry is not yet a complete resource/lifecycle owner (2026-11-14)

**Severity:** HIGH / architecture acceptance blocker. Current implementation remains a discovery/identity slice.

### Exact source evidence
- PR #141 is Draft and stacked on PR #139's architecture branch, not main. Exact head `cda2ba2b3b9432f0dcae1843bc331ac858c4b6e2` has five successful workflow runs (latest `43413762769`).
- `device_model.h/.c` stores bus, location, parent, vendor/product/class and generation-bearing identity. It does not store typed MMIO/port/IRQ/DMA resources, IOMMU domain, power/reset dependencies, firmware provenance, package identity, rights or teardown status.
- The current lifecycle enum contains only DISCOVERED, VALIDATED, QUARANTINED and FAILED. The atomic compare-and-transition primitive does not implement RESOURCES_RESERVED, MATCHED, AUTHORIZED, STARTING, RUNNING, QUIESCING, STOPPED, REVOKED or REMOVED.
- KDRV uses the generation-bearing device handle and quarantines stale bindings while retaining DMA pages if quiesce cannot be confirmed. Its registered `kdrv_pci_rescan_allowed()` guard only checks KDRV binding slots; it returns true when those slots are empty.
- Built-in PCI drivers in the current source independently enable Bus Master and map BAR resources (e.g. virtio-blk, UHCI, xHCI, HDA and e1000). Those owners are not represented in the KDRV-only rescan guard. A post-boot rescan must therefore remain denied until a global owner/quiesce/revoke transaction covers built-in drivers too; a KDRV-only approval is insufficient.
- The RFC itself explicitly lists resource reservation, interrupt ownership, DMA/IOMMU binding, hardware quiesce/revoke, hotplug and generic driver binding as not implemented.

### Required resolution
1. Keep the Device Model labelled as a partial discovery/identity registry, not a complete Device Model.
2. Make post-boot PCI rescan globally fail closed until every built-in and KDRV owner participates in a coordinated quiesce, DMA drain, IRQ mask, mapping revoke and resource-release protocol.
3. Add typed resource records and owner handles before introducing RUNNING/REMOVED states or hotplug.
4. Keep the RFC Draft until cross-ISA resource ownership and failure/teardown tests exist.

F-026 remains OPEN. The five successful PR #141 CI runs verify the current implementation slice only, not the missing lifecycle contract.


## Scheduler integration CI follow-up — UART long-line failure diagnosed (2026-11-14)

- PR #154 integration head `8f4b9be58f63d93911817184dcbbce0e4eca36c1`, workflow `43610795635`: 29/30 jobs succeeded; only `vm (drv-edu)` failed its expected KDRV success-line regex.
- Downloaded and inspected the VM artifact. The driver did start, DMA round-trip matched, protected PCI config writes were rejected, and the user test printed `PASS`. The serial log showed the KDRV success line split at the 192-byte per-CPU line-buffer boundary in the middle of a UTF-8 character, with another CPU's user log interleaved.
- Fixed `uart_print()` in the PR #154 integration branch: strings at least `LB_SIZE` bytes now flush any pending prefix and emit the complete string under the UART line lock instead of being split into independently locked chunks. The test assertion was not weakened.
- Corrected PR #154 head: `bfdffa64555ae221aa66c7c71efbb47f169c3f6a`, workflow `43611654110` queued at last check. Exact-head verification is pending; the 29/30 result belongs to the previous head.



## Project status manifest guard — exact-head CI complete (2026-11-14)

- PR #148 exact head `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`, workflow `43609627054`: all 3 jobs succeeded (host-tests, x86 build, ARM64 build).
- The corrected manifest parser includes explicit UELF outputs and the Android release subset. Inventory counts remain 25 x86-64 apps, 25 ARM64 apps and 13 x86-64 Android release apps.
- This verifies the manifest guard on that branch head. It does not prove all repository files are semantically reviewed, and it does not authorize deleting or replacing any project content.



## Kernel memory/scheduler integration — exact-head CI complete (2026-11-14)

- PR #154 corrected head `bfdffa64555ae221aa66c7c71efbb47f169c3f6a`, workflow `43611654110`: **43/43 jobs succeeded**.
- Verified host-tests, x86 build, ARM64 build and full VM matrix, including the previously failing `drv-edu` and `usb-uhci` scenarios.
- The fix includes guarded kernel stacks, scheduler fallback behavior, PMM ownership/atomic contiguous release and UART atomic emission for oversized log lines.
- This is a green PR branch, not a main merge or release build. PR remains Draft for integration review.



## F-022 status correction — exact-head CI closed (2026-11-14)

PR #154 head `bfdffa64555ae221aa66c7c71efbb47f169c3f6a` / workflow `43611654110` completed 30/30 successfully. The earlier 29/30 failure on the preceding head was diagnosed from the VM artifact and fixed in UART output. F-022's CI gate is now closed; architecture-wide acceptance remains open pending per-ISA source review and final merge composition.


## F-025 investigation update — UHCI failure not reproduced with PCI trace (2026-11-14)

- Combined no-trace head `f986d3f84fdc6bac6dd72ce7d4fd6a76bc4d3caf`, workflow `43611237456`: build, ARM64 and host tests passed; `vm (usb-uhci)` failed before the PCI enumeration completion log, with serial output ending after syscall initialization and QEMU reporting `Kernel died`.
- Added temporary bus-scan trace on head `3c9e066ddf8fd1eed55c68aaa0378329b1a0fcaf`. The exact `usb-uhci` VM then passed, found 8 PCI devices, initialized UHCI, mounted USB storage and passed all checks. Trace showed a single root bus 00 and no downstream bridge recursion.
- Removed all temporary diagnostics. Clean head `4b99d54da30f20bf0bed98e6c8ddb630f9bfcd36` has a fresh workflow `43612506879` pending. The debug run was interrupted by the next push after 23 successes and two cancelled jobs; it is not a full green result.
- The first UHCI failure is therefore not yet reproduced, but timing sensitivity remains possible because the diagnostic build passed. Do not close F-025 or F-017 until the clean exact-head matrix passes repeatedly and the failure mechanism is understood.

F-025 remains OPEN.


## IOMMU common error contract correction — exact-head CI pending (2026-11-14)

- PR #125 exact head `cf35a78b385c0b9939702e19b097b01a4296e273` failed host tests and ARM64 compilation because `iommu_arm64.c` returned the missing shared enum member `IOMMU_E_UNSUPPORTED`.
- Added `IOMMU_E_UNSUPPORTED = -10` to the shared `iommu_error_t` contract, preserving a distinct result for a valid but unsupported multi-controller topology. No fail-closed checks were relaxed.
- Corrected PR #125 head `8dc795e14575b77bde6182dddfb9d4c1ece54a0c`, workflow `43612438370`, is running. Exact-head host, ARM64 and VM results remain pending.


## F-025 clean-run confirmation — PCI enumeration hang persists without trace (2026-11-14)

- Clean no-trace PR #165 head `4b99d54da30f20bf0bed98e6c8ddb630f9bfcd36`, workflow `43612506879`: 28/30 jobs succeeded; only `vm (usb-uhci)` failed (host-tests and ARM64 still running when inspected). The serial log again stopped immediately after syscall initialization, before `[KIRA PCI]`; QEMU accumulated tens of millions of rejected UHCI DMA reads from physical address 0 while the firmware controller remained active.
- A prior trace-only build of the same PCI-range logic passed UHCI, but was cancelled after 23 successful jobs when the trace was removed. This confirms timing sensitivity; it does not establish correctness.
- A new diagnostic head `2b3b59828bacabee18001fc80faea1ff3e39cc31` now logs each PCI function immediately before/after BAR probing and after the Bus Master policy. Workflow is queued/running. Its purpose is to identify the exact function/operation where the scan stalls.
- Do not merge #165 while this reproducible no-trace failure remains. F-025 stays OPEN; F-017 remains independently OPEN.



## F-022 architecture-source review addendum — stack allocator parity (2026-11-14)

- Reviewed the exact PR #154 stack path on both architectures: shared scheduler allocation/free wrapper in `sched.c`; x86-64 guarded mapping in `vmm.c`; ARM64 guarded mapping in `arch/aarch64/main.c`; shared generation/free-list helper in `kstack_slot.h`.
- Both implementations reserve 64-KiB slots, map only the 16-KiB stack region, retain the unmapped lower guard, quarantine partially mapped slots rather than reusing suspect virtual addresses, and use generation tokens to reject stale/double frees. ARM64 uses 65,536 slots (16-bit slot IDs) and its helper tests explicitly exercise the final slot.
- Host tests exercise shared bounds/ownership/ABA/free-list logic under ASan/UBSan. PR #154's full x86/ARM64 CI and VM matrices passed, including ARM64 scheduler/mutex worker scenarios.
- Remaining coverage gap: no dedicated QEMU test deliberately exhausts guarded slots or triggers a guard-page fault on each ISA. The source review found no concrete defect in the reviewed allocation/free paths, but this missing negative-path test keeps architecture-wide acceptance open.



## F-025 diagnostic result — exact PCI function isolated (2026-11-14)

- PR #165 diagnostic head `7ad140cfadb51c4dacf809eca77fff60ff7fe12c`, workflow `43613588426`, job `132504574309` (`vm (usb-uhci)`): failed after 90.9 s waiting for the shell prompt. The serial artifact is now readable and shows `begin 0:1.2` followed by no `probed 0:1.2`; all earlier functions 00:00.0, 00:01.0 and 00:01.1 complete all trace points. The stall is therefore inside `probe_bars()` for PCI function 00:01.2, the UHCI controller, not in later bridge traversal or the post-probe Bus Master policy.
- The QEMU log artifact is 4,187,160,076 bytes and its tail repeatedly reports rejected DMA reads at physical addresses 0x0, 0x4, 0x8 and 0xC. The current #165 code disables IO/MEM decode inside `probe_bars()` but does not clear PCI Bus Master until after `probe_bars()`; firmware DMA is therefore still enabled during this exact operation. This is strong causal evidence for the DMA/probe interaction, but the precise hardware/QEMU mechanism is not yet proven.
- PR #125 uses the opposite ordering (`pci_disable_bus_master(d)` before `probe_bars(d)`). Its corrected exact-head run `43613583149` has UEFI OVMF, build, ARM64, host tests, KCC, GUI, driver E2E and audit passing; several VM shards remain in progress. Compare its eventual UHCI artifact before changing policy or merging either branch.
- Keep F-025 and F-017 OPEN. Do not merge #165 or #125 on this evidence. Remove temporary UART trace after the comparison and rerun the full clean matrix.


## F-025 evidence correction — PCI trace boundaries and shared UHCI stop point (2026-11-14)

This section supersedes the previous F-025 paragraph that attributed the PR #165 stall to BAR probing. The prior interpretation was incorrect because it treated the absence of a later trace record as proof that the preceding function had not returned; the exact artifact shows otherwise.

### Exact artifact evidence
- PR #165 head 7ad140cfadb51c4dacf809eca77fff60ff7fe12c, workflow 43613588426, job 132504574309, artifact vm-usb-uhci: serial.log contains “probed 0:1.2” and no “secured 0:1.2”. Therefore probe_bars() returned for UHCI function 00:01.2. The stall is in the post-probe policy sequence, which reads Command and, when Bus Master is set, clears it via pci_write16(); exact sub-operation (read, write or lower-level config access) is not yet isolated.
- PR #125 head fee8c9aed19ffc37dab7fd599c2bd63bfc496924, workflow 43613583149, job 132504922587, artifact vm-shard-0/usb-uhci: serial.log contains “disable BM 0:1.2” and no “BM disabled 0:1.2”. In this branch the pre-probe pci_disable_bus_master() helper performs config read, write, readback and quarantine handling under cfg_rmw_lock; the trace only bounds the stall to that helper, not to one instruction inside it.
- Both exact artifacts fail the UHCI scenario after 90 seconds waiting for the shell prompt. Other scenarios in PR #125 shard 0 (including boot-virtio, apps-explorer2 and net-ipv6) passed their recorded checks. PR #125 run has 18 successful jobs, one failed VM shard and one VM shard still in progress at the time of inspection; PR #165 run has 29 successes and the UHCI VM failure.

### Source comparison
- PR #125 performs pci_disable_bus_master(d) before probe_bars(d); PR #165 probes BARs first, then reads/writes the PCI Command register to clear Bus Master.
- PR #125's helper uses a read-modify-write-readback sequence under cfg_rmw_lock; PR #165's policy uses pci_read16() + pci_write16() without that outer lock (the x86 config-port backend still serializes each CF8/CFC transaction).
- Both code paths attempt to clear the same UHCI function's Bus Master bit while the firmware-started controller is active. This is now the strongest shared hypothesis. It is not yet proven whether the stall is caused by controller activity/firmware DMA, config-space access ordering/locking, or a QEMU UHCI interaction.
- The previous claim that PR #165 stopped inside probe_bars() is withdrawn. Do not use that claim in integration decisions.

### Next discriminating checks
1. Add bounded, single-purpose trace points around Command read, Command write and readback, with no long UART strings and no trace-only changes to timing-sensitive behavior beyond the minimum.
2. Capture the exact Command value and UHCI USBCMD/USBSTS state before touching Bus Master; determine whether firmware left the controller running and whether its schedule base is zero/invalid.
3. Compare a class-aware UHCI quiesce sequence (halt/reset with bounded waits) against early IOMMU isolation; do not assume a generic Bus Master clear is safe before a controller-specific stop.
4. Add a host regression for the config lock/read/write/readback path and a QEMU negative test where firmware leaves UHCI active; assert bounded completion or explicit quarantine, never an indefinite boot hang.
5. Remove temporary trace after localization, then require repeated clean full exact-head x86/ARM64 CI. Do not merge #125 or #165 on current evidence.

F-025 remains OPEN. F-017 remains OPEN independently: firmware DMA must be contained before unsafe BAR probing/driver handoff, but the exact safe ownership transition is still undecided.


## New finding F-027 — FDT property-name lookup is not bounded by the strings block (2026-11-14)

**Severity:** MEDIUM robustness / malformed-firmware-input boundary; source-level gap, not yet reproduced as a fault.

### Evidence
- Current main FDT parser validates the strings block's offset and total size, and fdt_prop() checks that a property name offset is less than size_dt_strings.
- The comparison then calls streq(strings + nameoff, requested_name), but streq() has no remaining-length argument. It does not prove that the stored property name has a NUL terminator within the strings block.
- PR #166 adds strong checks for the reservation map, block overlap, FDT v16 structure sizing and reg index/address overflow. Its exact head 4bc69a224887ead8d8d1737e782ed76cf4b8e95e passed workflow 43610515593 (30/30), but the current fdt_prop()/streq() path remains unchanged and the expanded fdt_test.c still has no unterminated property-name fixture.
- A malformed nameoff at the final byte(s) of the strings block can make the comparison inspect a byte beyond that block. The precise impact depends on adjacent blob layout; do not describe this as a demonstrated arbitrary read or crash.

### Required closure
1. Replace unbounded streq() for property-name lookup with a length-bounded comparison that requires the NUL terminator before strings_size.
2. Extend fdt_valid() or the property parser to reject invalid name offsets and missing terminators without reading outside the block.
3. Add ASan/UBSan fixtures for nameoff == strings_size, nameoff at the final byte with no terminator, a name that matches a known property prefix but lacks NUL, and a valid final string with its terminator.
4. Run the focused FDT host test, ARM64 build and QEMU boot scenarios on the exact candidate; then run full CI.

F-027 remains OPEN pending a reproducing regression test and exact-head verification.


## New finding F-028 — BootInfo v1 implementations diverge across preserved branch stacks (2026-11-14)

**Severity:** HIGH integration/boot-contract risk. This is branch divergence, not evidence that the individual adapters currently fail their recorded tests.

### Evidence
- Current main still has separate loader handoffs: x86-64 BIOS enters through Multiboot2 and pmm.c parses the MB2 map directly; ARM64 arch/aarch64/main.c consumes the FDT directly. The versioned KIRA BootInfo consumer/producer contract is not integrated into main.
- PR #12 head c3342321667045a29b3c7e4726a31b926117a9b1 (base v45/overhaul) contains BootInfo v1, common validation, Multiboot2 and FDT adapters, and tests. Its exact-head runs 43236520117 and 43236520194 succeeded.
- PR #46 head dacc04d0dafd507ef0721698b5e9c2512f1947a5 (base v46/kdrv-catalogue-signature-policy) adds a UEFI producer and loader that calls kira_uefi_build_kbi(), exits boot services, and enters the kernel with RCX=BootInfo pointer and RDX=size. No exact-head workflow run is associated with #46.
- Both branches advertise BootInfo version 1 and share the same packed header/memory-map/module layouts, but their implementation files are not equivalent:
  - PR #12 bootinfo.c adds module_in_ram() and rejects modules unless their full physical extent is covered by ordered USABLE or MODULE memory-map ranges. PR #46's bootinfo.c lacks this validation.
  - PR #12 bootinfo_fdt.c consumes the FDT firmware reservation map, reserves the kernel image and DTB, subtracts /reserved-memory ranges, and reserves simple-framebuffer memory before publishing usable RAM. PR #46's bootinfo_fdt.c is a shorter implementation without the same reservation-subtraction path.
  - PR #46's bootinfo.h adds a kbi_to_multiboot2() compatibility declaration; its bootinfo_uefi.c implements that conversion. This adapter is additional surface, not part of the PR #12 header.
- PR #46 does include tests/uefi_kbi_test.c for descriptor conversion, memory-type classification, GOP geometry, overflow, overlapping descriptors and RSDP alignment. PR #12 includes common BootInfo and MB2/FDT conversion tests. Those test sets are complementary, not interchangeable.

### Required resolution
1. Freeze one canonical BootInfo v1 header and validation semantics before transplanting producers.
2. Carry forward the stronger module-in-RAM validation and FDT reservation subtraction; do not regress them when adding the UEFI producer.
3. Decide whether kbi_to_multiboot2() is a required compatibility adapter or legacy transition code; keep it out of the core ABI unless a real consumer needs it.
4. Reconcile UEFI kernel entry ABI, ExitBootServices retry behavior, BootInfo lifetime/physical ownership, and x86/ARM64 kernel consumer before replacing either current boot path.
5. Add cross-producer golden fixtures: MB2, UEFI and FDT should produce semantically equivalent normalized memory maps for equivalent firmware layouts, with reserved/module/framebuffer regions preserved.
6. Require exact-head host, BIOS, OVMF, ARM64 QEMU and memory-reservation tests on the combined candidate.

F-028 remains OPEN. PR #12 and PR #46 remain preserved branch work; neither is treated as integrated into main.


### F-025 final status for PR #125 diagnostic run — exact head completed (2026-11-14)

- PR #125 head fee8c9aed19ffc37dab7fd599c2bd63bfc496924, workflow 43613583149: all 23 jobs reached terminal state; 20 succeeded, one failed (vm (0), job 132504922587), and two packaging jobs were skipped by workflow conditions.
- The other 9 VM shards, host-tests, x86 build, ARM64 build, UEFI OVMF, audit, KCC, GUI, driver E2E and mind-quality succeeded. This does not make the workflow green: the UHCI VM failure remains a blocking failure.
- PR #165 head 7ad140cfadb51c4dacf809eca77fff60ff7fe12c, workflow 43613588426: 29/30 jobs succeeded; vm (usb-uhci) failed.
- Both diagnostic heads remain unaccepted. The exact artifact evidence and corrected trace interpretation are in F-025 above.


### PR #148 exact-head acceptance update — three-stage status registry (2026-11-14)

- PR #148 current head bf35ca714526d38f5fdc3021c03b2bedf3ec2c99, workflow 43609627054: **43/43 jobs succeeded**. This supersedes earlier notes that the corrected manifest head was still pending.
- The PR adds the requested three-stage project ledger: docs/PROJECT_STATUS/01_READY.md, 02_IN_PROGRESS.md and 03_PLANNED.md, plus a machine-readable status_manifest.json, branch-tree inventories and ci/check_project_status.py.
- The ledger carefully defines READY as present in current main/build baseline, not feature-complete or hardware-qualified. It distinguishes x86 artifacts published by main CI from ARM64 artifacts built but not published.
- The checker pins the 829-file baseline, 98 x86 and 88 ARM64 kernel input lists, 16 x86 architecture inputs, userland program inventories and all branch/PR snapshots; exact current PR head passed.
- PR #148 remains Draft and unmerged. Do not claim this guard is active on main until accepted/integrated; no main change made.


### F-025 QEMU DMA-log addendum — repeated UHCI frame-list reads from zero (2026-11-14)

- Stream-inspected the complete PR #165 UHCI qemu.log without loading it into memory. Uncompressed size: **4,187,160,076 bytes**.
- Counted rejected 4-byte DMA reads:
  - address 0x0: **15,377,534**
  - address 0x4: **15,377,534**
  - address 0x8: **15,377,534**
  - address 0xC: **15,377,533**
- The final log records repeatedly cycle through exactly these four low addresses. The matching VM scenario attaches USB tablet and keyboard to the chipset's UHCI controller. This is consistent with an active UHCI controller repeatedly fetching a frame list whose base is zero or otherwise invalid.
- This strengthens the controller-not-quiesced hypothesis, but the QEMU log alone does not prove the actual USBCMD/USBSTS/FLBASEADD register values or whether the PCI Command write completed. The serial trace still ends after “probed 0:1.2” and before “secured 0:1.2”.
- The next diagnostic must capture UHCI USBCMD, USBSTS and FLBASEADD before the Bus Master handoff; then test a bounded controller halt/reset before clearing Bus Master. Keep the IOMMU-first alternative in the design review. Do not classify the hypothesis as confirmed until the register state and a clean repeated scenario support it.


## New finding F-029 — USB drivers re-enable Bus Master before quiescing firmware controller state (2026-11-14)

**Severity:** HIGH DMA ownership/handoff risk. Source-level ordering defect; runtime impact on all physical controllers is not yet measured.

### Evidence
- In the PCI scan path, PR #165 clears endpoint Bus Master after BAR probing and before publishing the device. The USB driver starts later in usb_main(), after pci_init() returns.
- UHCI: uhci_start() calls pci_enable(p, PCI_CMD_IO | PCI_CMD_MASTER | PCI_CMD_INTX_OFF) before its first USBCMD=0 halt request and USBSTS.HCHalted wait. It only programs FRBASE to the new kernel-owned frame list after reset. Thus Bus Master is re-enabled while the firmware-owned controller state may still be active and before the new frame-list base is installed.
- xHCI: xhci_start() calls pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF) before BAR mapping, BIOS ownership handoff, USBCMD.Run clear, HCHalted wait and HCRST. It then allocates/programs DCBAAP, CRCR and event-ring registers and starts the controller.
- These orderings mean the generic PCI scan's earlier clear is not a durable DMA boundary: each USB driver grants Bus Master again before it has stopped/reset the controller and replaced firmware DMA state.
- The current UHCI QEMU failure occurs earlier in pci_init(), before uhci_start() is reached. F-029 is a separate downstream defect that would remain after F-025 is fixed.
- PCI Command I/O/MEM decode can be enabled without granting Bus Master. UHCI needs I/O decode to access its port registers; xHCI needs memory decode to access its BAR. The driver can defer PCI_CMD_MASTER until after bounded stop/reset and after valid kernel DMA structures/addresses are programmed.

### Required closure
1. Split decode enable from DMA permission: enable IO/MEM decode without MASTER so the driver can access controller registers while DMA remains denied.
2. Perform controller-specific BIOS handoff, stop and reset with bounded waits; capture status and quarantine the controller on timeout.
3. Allocate and initialize DMA structures, program UHCI FRBASE / xHCI DCBAAP+CRCR+event ring, and only then request Bus Master under the DMA policy/IOMMU gate.
4. Add host tests for PCI Command transition order and QEMU tests with firmware-started UHCI/xHCI controllers whose old DMA bases are invalid/nonzero.
5. Verify that failure at any point leaves Bus Master denied and does not expose partially initialized rings; repeat on x86-64 and relevant ARM64 xHCI path.

F-029 remains OPEN. Do not accept PCI or USB DMA safety based solely on the initial PCI enumeration clear.


## New finding F-030 — multiple PCI DMA drivers grant Bus Master before quiesce and DMA setup (2026-11-14)

**Severity:** HIGH, cross-driver DMA ownership risk. The call ordering is present in current main; real firmware-active hardware impact requires targeted QEMU/physical validation.

### Evidence from current-main source
- pci_enable() simply ORs requested bits into PCI Command. It has no staged decode-vs-DMA policy, no readiness check and no readback.
- VirtIO: virtio_pci_init() enables MEM | MASTER before virtio_begin() resets device status and before virtio_queue_setup() allocates/programs queue memory.
- NVMe: ctl_init() enables MEM | MASTER before disabling CC.EN/waiting CSTS.RDY=0 and before allocating/programming admin queues.
- AHCI: ctl_init() enables MEM | MASTER before BIOS/OS handoff and GHC controller reset; per-port command/FIS structures are initialized later.
- e1000: e1000_probe() enables MEM | MASTER before device reset and before RX/TX rings and buffers are allocated/programmed.
- HDA: hda_probe() enables MEM | MASTER before controller reset and before CORB/RIRB/BDL and stream buffers are allocated/programmed.
- AC'97: ac97_probe() enables IO | MASTER before cold reset/codec initialization and before DMA buffers/BDLs are allocated.
- UHCI/xHCI have the analogous sequence documented in F-029.
- Some devices may reset or remain idle after firmware handoff, but the kernel cannot assume that universally. A firmware-started device can retain DMA pointers/state when Bus Master is re-enabled.

### Required closure
1. Replace one-shot pci_enable(bits) usage with staged command policy: IO/MEM decode separately from MASTER; MASTER only after controller-specific stop/reset and kernel-owned DMA structures are programmed.
2. For each DMA driver, define a bounded quiesce sequence and a failure state that leaves Bus Master disabled.
3. Enable Bus Master only immediately before the device's documented start/ready transition; read back PCI Command and verify it matches policy.
4. Add tests that seed firmware-active register state and stale DMA pointers, then assert no device DMA occurs before its new queues/rings are valid.
5. Gate the final grant on a verified IOMMU domain where supported. Until real VT-d/SMMUv3 is integrated, retain fail-closed DMA policy for untrusted/Ring 3 drivers and clearly record remaining built-in-driver exposure.
6. Run focused device tests first (VirtIO, NVMe, AHCI, e1000, HDA, AC'97, UHCI, xHCI), then affected x86/ARM64 matrix and full exact-head CI.

F-030 remains OPEN. This finding is separate from F-017 (enumeration-time firmware DMA), F-025 (UHCI QEMU stall) and F-029 (USB-specific ordering).


### F-025 harness cost note

The 4.18 GB qemu.log was generated because the current VM harness always enables QEMU guest_errors logging. The same runner has a per-scenario boot_timeout setting (default 90 s). A future short diagnostic case should use a 10–15 s boot timeout and optional QEMU error logging; do not shorten the full USB acceptance scenario.


## New finding F-031 — KDRV grants PCI Bus Master before a Ring 3 driver has initialized the device (2026-11-14)

**Severity:** HIGH, breaks the intended least-privilege DMA boundary.

### Evidence
- In current-main kdrv.c, start() computes the user-approved capability intersection, enables PCI MEM decode for CAP_MMIO, then enables PCI_CMD_MASTER immediately whenever either CAP_DMA or CAP_IRQ is granted. This occurs before proc_spawn_driver().
- The driver process has not yet mapped BARs, allocated/initialized DMA buffers, reset the controller, or programmed new DMA addresses at the time Bus Master is enabled.
- CAP_IRQ is not equivalent to permission to DMA arbitrary buffers. MSI is a device-originated memory write, but the PCI Bus Master bit is a device-wide transaction permission; granting it for IRQ also opens the device's general bus-master capability. Without an IOMMU domain, MSI-only permission cannot be safely isolated from arbitrary DMA by this bit alone.
- The syscall path also permits a driver with CAP_DMA or CAP_IRQ to set PCI_CMD_MASTER through KDRV_SYS_CFG_WRITE. This can be a deliberate driver-controlled transition, but there is no required readiness state, IOMMU-domain token, or kernel-owned DMA-registration check before the bit is set.
- KDRV's DMA allocator creates contiguous identity-mapped pages and records them per binding, but the code does not establish an IOMMU translation domain or constrain a device to those pages. This is not DMA isolation: the device can still address any bus-visible physical address.
- The in-tree RTL8139 Ring 3 driver requests DMA and IRQ capabilities. Its start path maps the BAR, allocates RX/TX buffers, resets the NIC, then initializes ring addresses and starts RX/TX. However KDRV has already granted Bus Master before the driver process starts, so the safe order inside rl.c does not close the manager-level early grant.

### Required closure
1. Do not enable PCI_CMD_MASTER in start() merely because CAP_DMA or CAP_IRQ was approved. Keep decode permissions separate.
2. Model MSI as a distinct, narrowly scoped interrupt operation; do not treat CAP_IRQ as generic DMA authorization. Where the platform cannot enforce MSI separately from arbitrary requester DMA, require an IOMMU requester domain or keep the untrusted device's Bus Master disabled.
3. Add a kernel-owned DMA readiness/registration transition: registered DMA allocations + controller quiesced/reset + driver-reported/programmed ring addresses + IOMMU mapping all verified before Bus Master can be enabled.
4. Revoke Bus Master before process teardown and verify PCI Command readback; on failure, quarantine the device.
5. Add negative tests: CAP_IRQ-only must not grant unrestricted DMA; CAP_DMA driver must not obtain Bus Master before its first registered queue/ring; failed driver startup leaves Bus Master clear; teardown clears it.
6. Preserve explicit fail-closed behavior on platforms without an IOMMU. Do not claim that the existing contiguous allocator alone isolates DMA.

F-031 remains OPEN. This is a separate manager-level flaw in addition to the built-in driver's early-grant ordering in F-029/F-030.


## New finding F-032 — KIRA Mind can terminate the largest process without a critical-service exclusion contract (2026-11-14)

**Severity:** HIGH reliability/policy risk in ACT mode; the current rule is implemented, not a placeholder.

### Evidence
- mind.c memory() has three thresholds: at 88% memory it asks the kernel to trim caches; at 93% it restricts heap growth; at 97% it may kill a process, no more than once per 10 seconds.
- The kill candidate is the process with the largest reported heap, provided it uses at least 16 MiB and at least 20% of total RAM. The code does not rank by service criticality, user data durability, foreground status, or restartability before calling proc_kill().
- mind_sys.c supplies the callback directly as proc_kill(). proc_kill() marks the process dying and wakes its threads; teardown later closes network/audio/graphics and driver resources.
- The sample and policy interfaces shown in current main do not expose a protected/essential service class or a per-process kill-veto callback. Therefore a large system-facing process or a process with unsaved work may be eligible under the same heuristic as an ordinary application.
- This is not proof that a specific critical process is killed in the current boot profile; it is a missing policy contract that must be closed before autonomous memory-pressure termination is considered safe.
- The current Mind is an adaptive rule/profile-based resource controller (measured scheduler tuning, process profiles, thresholds, throttling and memory-pressure actions). The inspected implementation does not demonstrate a general neural/LLM reasoning model or an unrestricted AI authority. Keep those as planned architecture, not claim them as present.

### Required closure
1. Add explicit process classes and a kernel-owned non-killable/essential flag for boot/system services, drivers, storage/network core and user-confirmed protected work.
2. Require a termination policy callback that checks class, foreground status, pending writes/transactions, driver binding and restartability; default deny if metadata is absent.
3. Prefer reversible reclamation (cache trim, heap admission control, background throttling) and bounded service restart before application termination.
4. Expose a user-visible policy mode and auditable reason; provide observe-only default for new/untrusted deployments until the protection matrix is verified.
5. Add tests with synthetic process sets: largest process is essential, foreground, unsaved, driver, restartable, and ordinary app; assert only permitted victims are selected.
6. Add adversarial tests for threshold oscillation, multiple concurrent memory events, and process exit racing with the kill callback.

F-032 remains OPEN. Do not describe the current heuristic as safe general autonomous AI process management.


## New finding F-033 — FAT flush discards the underlying block-device flush error (2026-11-14)

**Severity:** HIGH data-integrity reporting defect.

### Evidence
- In current-main fat.c, fat_flush() writes the dirty FAT sectors and FSInfo, then calls v->dev->flush(v->dev) if available but discards its return value and unconditionally returns 0.
- fat_write(), fat_mkdir(), fat_unlink() and vfs_flush() rely on fat_flush() to report persistence completion. A device cache flush failure can therefore be reported as success even though durable media ordering is not confirmed.
- tests/fat_test.c supplies a memory block device with no flush callback, so the FAT suite does not exercise this failure. tests/store_test.c does test a flush failure for the separate store module; that does not cover fat_flush().
- This is a source-confirmed defect; no runtime media failure was needed to identify the lost error.

### Required closure
1. Return the exact nonzero device flush result as KFS_ERR_IO (and record device health) from fat_flush().
2. Add a fake block device with a controllable flush callback to tests/fat_test.c. Verify fat_flush(), fat_write(), mkdir/unlink and vfs_flush propagate the failure.
3. Test failure at each persistence boundary (FAT copy write, FSInfo write, device flush) and remount the image to check consistency.
4. Do not report an external volume as safely flushed/ejected if the flush operation failed.

F-033 remains OPEN.


## New finding F-034 — VFS root enumeration and availability checks read mount entries without their per-volume locks (2026-11-14)

**Severity:** MEDIUM concurrency/lifetime race.

### Evidence
- vfs_mount_fat(), vfs_unmount(), vfs_unmount_try(), vfs_flush(), vfs_mounts() and per-volume operations protect each mount slot with lk[i].
- In contrast, vfs_any() reads mnt[i].used without taking lk[i], and the root-directory branch of vfs_list() reads mnt[i].used and publishes the corresponding /usbN entry without locking that slot.
- A concurrent mount/unmount can change a slot while these reads occur. The result can be a stale or inconsistent root listing/availability answer; the code does not establish a coherent snapshot.
- The VFS root path and mount operations are reachable from separate processes/threads, so this is not inherently single-threaded.
- The per-volume FAT operations themselves are serialized by the slot lock. The finding is specifically the unlocked global/root observations, not a claim that all VFS operations are unprotected.

### Required closure
1. Take each slot lock while reading used/vol/label/description, or build a bounded consistent mount snapshot under a documented lock order.
2. Ensure root listing does not invoke callbacks while holding a lock if callbacks can re-enter VFS; copy slot presence to a local array under locks, then invoke callbacks after unlock.
3. Add concurrent mount/unmount versus vfs_any()/root-vfs_list() host and kernel tests, including slot reuse.
4. Document the VFS lock order relative to FAT/block-device locks.

F-034 remains OPEN.


### F-025 exact-current-run refresh

Current PR #165 head 7ad140cfadb51c4dacf809eca77fff60ff7fe12c still fails workflow 43613588426: 29/30 jobs, with vm (usb-uhci) failing. Exact serial output stops after the BAR probe marker for 00:01.2 and before the PCI "secured" marker. The PCI source at this head performs the Command register read and conditional Bus Master clear between these markers. Current evidence narrows the stall to that sequence but cannot yet distinguish read from write; separate markers and a command-register value are required. Keep F-025 OPEN.


## New finding F-035 — installed driver record loses per-file source and license provenance (2026-11-14)

**Severity:** MEDIUM supply-chain traceability gap; the current source-download and confirmation flow does function.

### Evidence
- drvget parses a recipe containing one or more source-file URLs, downloads each file over HTTPS, checks optional per-file SHA-256 pins, scans each file for an allowed license marker, and computes a combined hash over the recipe and all source contents.
- The install report prints the recipe URL, the declared recipe license, and each file's size, detected license and SHA-256. The actual per-file download URLs are printed during download, but they are not retained in the installed package record.
- The installed KDRV record stores one source string (the recipe URL plus a single declared license marker), one combined source-set SHA-256, the package hash and ELF hash. It does not retain the per-file URL/license/hash manifest.
- The code accepts a recipe-level declared license from the allowed set, while each source file is independently scanned for an allowed license. It does not require every detected file license to match the recipe's declared license; the report can therefore show differing values, while the installed record preserves only the recipe-level value.
- The user confirmation accepts an eight-or-more-hex-character prefix of the combined SHA-256. Eight hex characters are 32 bits; this is a human confirmation token, not a cryptographic signature or strong authenticity proof.
- drvget explicitly has no source signatures and does not translate arbitrary Linux/Windows binaries or ABIs. It is a constrained HTTPS source-recipe + KCC build workflow with a limited BSD compatibility header. The universal Driver Adaptation Layer remains planned (P-06).

### Required closure
1. Extend the installed provenance record to retain a versioned per-file manifest: normalized URL, detected license, file SHA-256, recipe hash, compiler/toolchain version and target ABI.
2. Require recipe license to be compatible with every file's detected license, or record the exact per-file license set and require explicit user review when they differ.
3. Make the default confirmation length materially stronger (e.g. at least 16 hex characters) while preserving the ability to enter the full digest; clearly label it as manual confirmation, not a signature.
4. Add signed recipe/package support with key rotation and revocation as a separate trust layer; never treat HTTPS alone as source authenticity.
5. Keep universal Linux/Windows binary/ABI translation as a separate planned project with source-level porting, sandbox build/test and rollback criteria. Do not market the present recipe installer as a universal adapter.

F-035 remains OPEN as a provenance/acceptance gap, not a claim that current source installation is a mock.


## New finding F-035 — UDP socket ownership teardown bypasses the UDP table lock (2026-11-14)

**Severity:** MEDIUM concurrency and socket-isolation race.

### Evidence
- UDP socket allocation and close mutate us[].owner, lport, fam, ring cursors and counters under ulk.
- Packet delivery (udp_deliver) also reads the slot and writes its ring under ulk.
- In contrast, udp_gone(pid) clears us[i].owner without acquiring ulk, and uown() reads the owner without the lock. net_proc_gone() calls udp_gone() during process teardown.
- A concurrent teardown can therefore race with packet delivery, socket close/reopen, or a syscall validating the socket. Slot ownership and slot metadata are not observed as one synchronized state. A slot can be released/reused while another path is still using it.
- This is a source-level race; no runtime corruption is claimed.

### Required closure
1. Make udp_gone() acquire ulk while revoking all sockets owned by the process.
2. Make uown() validate ownership under ulk, and ensure the subsequent operation cannot race with slot release/reuse (generation-tagged handles or a per-operation reference/pin is preferable to merely unlocking after the check).
3. Ensure send/recv copy the slot's family and local port from a pinned slot snapshot; do not read mutable slot metadata outside the lock.
4. Add concurrent tests: process teardown vs UDP receive, send, close, reopen, and packet delivery; verify no cross-process datagram exposure and no stale-handle reuse.
5. Audit tcp_owner[] and listen_owner[] for the same lifecycle synchronization contract; they are also accessed without an ownership-table lock.

F-035 remains OPEN.


## New finding F-036 — HKDF-Expand silently exceeds RFC output limits and truncates info (2026-11-14)

**Severity:** MEDIUM cryptographic API correctness.

### Evidence
- In current-main crypto.c, hkdf_expand() uses an 8-bit block counter and does not reject output lengths greater than 255 × 32 = 8160 bytes. For a larger request the counter wraps and repeats block labels, producing output outside RFC 5869's defined limit.
- hkdf_expand() silently truncates info to 300 bytes.
- hkdf_expand_label() silently truncates labels to 64 bytes and context to 64 bytes, and encodes olen into a 16-bit field without validating the TLS 1.3 label/output bounds.
- Existing tests/crypto_test.c checks the standard RFC 5869 42-byte vector and one TLS 1.3 "derived" label vector. It does not test maximum permitted output, one-byte-over-limit output, or oversized info/label/context rejection.
- Current in-tree TLS callers may only use short, bounded values; this finding concerns the public primitive API's missing input validation, not a demonstrated exploit in a current TLS handshake.

### Required closure
1. Enforce RFC 5869's L <= 255 × HashLen before entering the counter loop; return an error rather than wrap. Change the API contract if needed so callers can observe rejection.
2. Validate the TLS 1.3 HKDF label encoding constraints and exact supported label/context bounds; do not silently truncate.
3. Add boundary tests for 8160 and 8161 bytes, oversized info, label, context and output length, plus regression vectors for all supported valid cases.
4. Audit ChaCha20 and AES-GCM maximum-length/counter bounds as part of the same crypto API hardening pass; primitive callers must not be able to wrap a block counter and repeat keystream.

### Implementation tracking
- Draft PR #172: https://github.com/Masuku4129/kira_os_/pull/172
- The candidate implementation now covers HKDF output/input bounds, TLS HKDF label/context validation, ChaCha20 counter exhaustion and AES-GCM maximum message length. TLS call sites propagate derivation/sealing errors.
- Exact-head CI for PR #172 completed **30/30 jobs successfully**, including host crypto tests, x86-64, ARM64 and VM matrix.
- The candidate remains a Draft PR; the finding remains OPEN on `main` until integrated.

F-036 remains OPEN.


## New finding F-037 — TLS and X.509 verification use process-global mutable scratch state (2026-11-14)

**Severity:** HIGH security isolation / concurrency risk.

### Evidence
- userland/tls.c keeps the handshake transcript buffer and length, transcript hash context, Certificate message buffer, OCSP pointers, certificate pointer/length arrays and parsed leaf certificate in file-scope static variables.
- tls_connect() resets and mutates these globals for each connection. A tls_t* context does not own these buffers/state.
- userland/x509.c also uses static x509_t chain[8] in x509_verify_chain() and static certificate scratch objects in x509_ocsp().
- Two threads in the same process performing TLS handshakes or certificate/OCSP verification concurrently can overwrite each other's transcript, certificate pointers and parsed chain. The resulting behavior can include handshake failure or validation being performed against another connection's mutable certificate state.
- This finding assumes the API is used concurrently; no specific exploit or runtime race has been demonstrated. There is no per-context lock or documented single-active-connection contract in the inspected headers.

### Required closure
1. Move all handshake/transcript/certificate/OCSP scratch state into tls_t or a per-handshake context passed down the call chain.
2. Make x509_verify_chain() and x509_ocsp() reentrant by using caller-owned/local scratch rather than static mutable objects.
3. If any shared immutable trust data remains global, make it read-only after initialization.
4. Add two-thread tests that run simultaneous handshakes to different hosts, plus concurrent X.509 chain and OCSP verification with distinct certificate sets.
5. Run ThreadSanitizer host tests where supported and verify no cross-context certificate or transcript contamination.

### Implementation tracking
- Draft PR #175: https://github.com/Masuku4129/kira_os_/pull/175
- Candidate moves TLS handshake/transcript/certificate/OCSP state into each `tls_t`; X.509 chain and OCSP parser scratch objects are automatic locals.
- Exact-head commit `4c7f7562d30d18ca8a32f54e17bffc430edf04e1`; CI run #43630721004 completed **30/30 jobs successfully**, including TLS/X.509 host tests, x86-64, ARM64 and VM matrix.
- Dedicated concurrent independent-handshake and ThreadSanitizer stress remain follow-ups.

F-037 remains OPEN on `main` until the candidate is reviewed/integrated and concurrent handshake stress coverage is added.


## New finding F-038 — X.509 parser silently accepts unknown critical extensions and omits CA path-length enforcement (2026-11-14)

**Severity:** HIGH certificate-validation policy defect.

### Evidence
- userland/x509.c parse_ext() reads the optional critical BOOLEAN into a temporary DER value but does not retain or evaluate it. Unknown extension OIDs fall through the loop and are silently ignored even when marked critical.
- RFC 5280 requires a certificate to be rejected when it contains a critical extension the implementation does not recognize and process. Ignoring one can bypass a constraint the issuer intended the relying party to enforce.
- basicConstraints parsing records only the CA BOOLEAN; any pathLenConstraint field is not parsed or enforced.
- x509_verify_chain() checks that an intermediate has is_ca set, but it does not enforce pathLenConstraint. The inspected chain code also needs an explicit policy review for keyUsage and serverAuth EKU before accepting a server certificate.
- Existing tests/x509_test.c covers signatures, chain construction, SAN/hostname, time validity, untrusted and tampered certificates, and OCSP. It does not demonstrate rejection of unknown critical extensions or pathLenConstraint violations.

### Required closure
1. Track the critical flag for every extension. Reject unknown critical extensions; explicitly parse and enforce every supported critical extension.
2. Parse basicConstraints pathLenConstraint and enforce it across the constructed certification path.
3. Parse and enforce keyUsage and extendedKeyUsage for TLS server authentication; reject certificates whose asserted constraints do not permit the intended use.
4. Add hostile DER test certificates with unknown critical extensions, pathLenConstraint=0 with subordinate CA, missing/incorrect keyUsage and incompatible EKU.
5. Keep unsupported critical extensions fail-closed; do not silently accept or strip them.

### Implementation tracking
- Draft PR #174: https://github.com/Masuku4129/kira_os_/pull/174
- Candidate enforces unknown-critical-extension rejection, duplicate extension rejection, BasicConstraints/pathLenConstraint (including the self-issued CA exception), KeyUsage and EKU rules.
- Exact-head commit `d8e20596ca411782cfd0318f87a787bce5bf7526`; CI run #43631654211 completed **30/30 jobs successfully**, including X.509 host tests, x86-64, ARM64 and VM matrix.
- The first host run caught an incorrect KeyUsage OID mapping; it was corrected to `2.5.29.15` and the full exact-head run passed.

F-038 remains OPEN on `main` until the candidate is reviewed/integrated.


## New finding F-039 — TCP state and listener fields are read outside the transport lock (2026-11-14)

**Severity:** HIGH concurrency correctness / connection-lifecycle race.

### Evidence (confirmed against main source SHA 7449015b5fad92b20d7bc2fc866e3a65ab296a39)
- `tcp_input()` (around lines 765–867) and `tcp_tick()` (858–868) mutate connection state, sequence numbers, retransmit deadlines and RX/TX queues under `tlk`.
- `tcp_wait()` (870–877) calls condition callbacks without `tlk`; `c_open`, `c_room`, `c_sent`, `c_data` and `c_closed` read `state`, `tx_len`, `rx_head/rx_tail` and FIN state concurrently with the network thread.
- `tcp_connect_ipa()` waits on a raw TCB pointer, then checks `t->state` and writes `T_FREE` on timeout outside `tlk`. A late SYN-ACK/timeout transition can race with this cleanup.
- `tcp_send()` reads state before locking and uses a raw slot pointer across two blocking waits. `tcp_recv()` also waits on a raw pointer before locking the RX ring. Neither operation pins the TCB against close/unlisten/free and immediate slot reuse.
- `tcp_peer()` reads the TCB state/address/port without `tlk`; `tcp_accept()` reads `listens[l].used` before taking `tlk`, then sleeps while retaining only the numeric listener slot.
- `tcp_unlisten()` frees pending TCBs under `tlk`, but there is no generation or reference contract connecting those TCBs to a listener incarnation. A listener slot can be closed and reused while an accept waiter still holds its old integer.
- `tcp_owner[]` and `listen_owner[]` are separate global arrays. `net_sys()` reads/writes them without `tlk`; `net_proc_gone()` walks and clears them while invoking blocking `tcp_close()` / `tcp_unlisten()`. Ownership validation is therefore not atomic with connection/listener lifetime.
- These are source-confirmed synchronization/lifetime defects. No packet-level exploit or runtime corruption is claimed.

### Required closure
1. Define one lock/ref/generation contract for TCBs and listener slots. Never sleep or copy user memory while holding `tlk`.
2. Evaluate all wait predicates and state snapshots under `tlk`; perform timeout cleanup and every FREE/CLOSED transition under the same lock.
3. Pin each TCB for the full duration of every blocking syscall and prevent reuse until all pins drain; use generation-tagged opaque handles to reject stale indices after reuse.
4. Give listeners equivalent generation/ref semantics so accept-vs-unlisten and listener-slot reuse cannot attach a waiter to a new listener.
5. Make owner authorization part of the synchronized lifetime transition; avoid unsynchronized side arrays or protect them with a documented lock order.
6. Add deterministic lifecycle tests plus concurrent stress for receive-vs-close, send-vs-reset, accept-vs-unlisten, connect-timeout-vs-late-SYN-ACK, process-exit-vs-I/O, and stale-handle-vs-slot-reuse.
7. Exercise both IPv4 and IPv6 paths, timeout boundaries, repeated close/unlisten, slot exhaustion and generation retirement.

### Implementation tracking
- Draft PR #173: https://github.com/Masuku4129/kira_os_/pull/173
- Candidate patch replaces raw slot indices with generation-tagged connection/listener handles, pins TCB/listener slots across blocking operations, synchronizes wait predicates and ownership under `tlk`, drains references before reuse, interrupts waits on process exit, and aborts owned sockets nonblocking during teardown.
- Host handle/generation tests are included. Exact-head commit `aa6e7832d93a35797b93faae8daa79f3ae74d029`; CI run #43626310419 completed **30/30 jobs successfully**, including host tests, x86-64, ARM64 and VM matrix.
- Concurrent SMP lifecycle stress (close/reset/receive, accept/unlisten, connect timeout/late SYN-ACK, process-exit-vs-I/O and slot reuse) is still not demonstrated.

F-039 remains OPEN until targeted lifecycle stress is added and the candidate is integrated.


## New finding F-040 — KCC compiler API is process-global and non-reentrant (2026-11-14)

**Severity:** MEDIUM API concurrency / compiler isolation.

### Evidence
- The public kcc_compile() entry point stores the active compilation in global KCtx K (kcc/util.c). Each invocation begins by memset() of that singleton context.
- The preprocessor keeps mutable file-scope macro table, include-once list, output token list and include-level state (kcc/pp.c).
- The parser keeps file-scope token cursor, global symbol/scope lists, current function/labels and local counters (kcc/parse.c).
- IR and both code generators keep mutable file-scope counters and backend state (kcc/ir.c, kcc/gen_x86.c, kcc/gen_a64.c, including assembler label/fixup contexts).
- Two concurrent kcc_compile() calls in the same process can overwrite each other's token streams, macro table, diagnostics, IR/backend state and arena pointers. One call can therefore corrupt or miscompile another; this is not a safe parallel compilation API.
- The shipped Ring 3 cc application currently invokes one compile at a time, so this is not evidence of a presently reachable userland exploit. The public API does not document or enforce a single-caller restriction.

### Required closure
1. Move compiler context, preprocessor, parser, IR and backend mutable state into a per-compilation context passed explicitly through the pipeline.
2. Until refactored, document and enforce a single-active-compilation contract (e.g. caller-owned serialization) at every API integration point; do not imply thread safety.
3. Add parallel host tests compiling distinct x86-64 and AArch64 translation units concurrently, verifying outputs and diagnostics do not cross-contaminate.
4. Run ThreadSanitizer where supported after context isolation.

### Implementation tracking
- Draft PR #176: https://github.com/Masuku4129/kira_os_/pull/176
- Adds an atomic single-active-compilation guard. Concurrent/nested calls return `KCC_BUSY` before touching global compiler state; success and longjmp error paths both release the guard.
- Exact-head commit `6ff41ceb5fe0bc6724cfaa6bc7b32e4198143523`; CI run #43631380730 completed **30/30 jobs successfully**, including a pthread host regression that proves the second invocation is rejected and the first completes without state corruption.
- This is a fail-closed serialization contract, **not** per-compilation context isolation. ThreadSanitizer and concurrent independent compilation are still outstanding.

F-040 remains OPEN until the compiler pipeline is context-isolated and parallel output/diagnostic tests pass.


## New finding F-041 — WASM binary decoder advances pointers beyond validated section bounds (2026-11-14)

**Severity:** HIGH untrusted-module parser memory safety.

### Evidence
- userland/wasm.c parses user-selected WebAssembly modules in Ring 3. The section loop validates `se = s + sz` against module end, but many nested fields then advance `s` without first proving the field fits within the current section: type parameter/result bytes, import/export names and kinds, table/memory limits, global descriptors, element entries, function-body local declarations and several instruction immediates.
- `skip_imm()` directly advances over memory-size/alignment bytes, float constants and typed-select type vectors (`*p += 1/4/8/n`) without checking remaining bytes against `e`.
- `leb_s()` returns a partial decoded value when input ends before a terminating byte; unlike `leb_u()`, it does not explicitly trap on unterminated/truncated LEB128.
- These parser paths can read outside the declared section and, for truncated final input, beyond the supplied module buffer. This is a source-level out-of-bounds-read risk when processing an untrusted .wasm file; no runtime exploit is claimed.
- Existing WASM differential/fuzz tests generate valid operations, trap cases and damaged copies, but this review did not establish coverage for every truncated section/LEB/immediate boundary or execute the harness.

### Required closure
1. Replace raw pointer increments with checked readers that receive the current section/body end and fail closed before every byte/range read.
2. Make signed and unsigned LEB readers reject unterminated, overlong and out-of-range encodings per WebAssembly binary rules.
3. Validate section order/uniqueness, vector counts, name lengths, type bytes, import/export indices, table/memory limits, element/data ranges and function-body boundaries before allocating or compiling.
4. Add systematic truncation tests at every byte boundary for each section and opcode immediate, plus oversized LEB/vector/section lengths.
5. Run the full WASM corpus under ASan/UBSan and fuzz malformed modules with bounded fuel and a strict timeout; require no out-of-bounds access, hang or cross-module state leakage.

F-041 remains OPEN.


## New finding F-042 — WASM imported function type indices are not validated before use (2026-11-14)

**Severity:** HIGH malformed-module memory-safety risk.

### Evidence
- During import-section parsing, function imports store the supplied type index `ti` directly in `funcs[nfuncs].type`; unlike defined functions, this value is not checked against `ntypes`.
- The export section may designate any function index, including an imported function, as `main`, `_start` or `kmain`. Final validation checks only `entry < nfuncs`.
- `umain()` then evaluates `&types[funcs[entry].type]` before execution. A module with an imported entry function whose type index is outside the allocated type array can therefore cause an out-of-bounds pointer dereference before a normal WASM trap is produced.
- Imports also are not checked against the required host ABI signatures (argument count/types and result count/type) for `write`, `sleep`, `ticks`, `exit`, `gfx_create`, `gfx_present` and `gfx_event`. `host_call()` assumes the import's declared type is compatible with the host operation; its local argument array has only four entries while the parser permits up to 32 parameters.
- This is a parser/ABI validation defect for untrusted WASM modules. No runtime exploit is claimed.

### Required closure
1. Reject every function import whose type index is `>= ntypes` during parsing.
2. Validate exact host import signatures against a single authoritative ABI table before accepting the module.
3. Reject imported entry points unless the runtime explicitly supports them and validates their exact signature.
4. Bound host-call parameter handling to the ABI's actual maximum; do not silently discard extra arguments.
5. Add hostile fixtures for absent type section, out-of-range type indices, wrong parameter/result types, 5+ arguments, and imported functions exported as entry points. Run under ASan/UBSan on both interpreter and JIT-enabled host builds.

F-042 remains OPEN.


## New finding F-043 — Audio stream handles are not pinned or consistently locked (2026-11-14)

**Severity:** HIGH concurrency/lifecycle correctness; possible cross-process audio stream corruption.

### Evidence
- `audio_sys()` validates `src[a1].used` and `src[a1].pid` without `lk`, then retains a raw pointer to the reusable global slot (`audio.c:242-243`).
- `audio_proc_gone()` clears matching slots under `lk` (`audio.c:333-336`); close also clears `used` under the lock (`audio.c:244`). A syscall can pass the unlocked owner check, be descheduled, then resume after teardown/close and slot reuse by another process. Its subsequent writes/resampling can affect the new owner's stream.
- The gain operation writes `s->gain` outside `lk` (`audio.c:245`), while the mixer reads it under `lk` (`audio.c:123-143`). This is an unsynchronized shared access.
- The write operation reads `s->ch`, `s->rate`, `s->tail`, `s->head` and mutates the resampler outside the lock before briefly locking only to enqueue output (`audio.c:247-262`). Concurrent close/process teardown and handle reuse are not excluded for the full operation.
- This is a source-level race/lifetime defect. No runtime corruption or privilege escalation has been demonstrated.

### Required closure
1. Validate owner and acquire a per-stream reference/pin under `lk`; keep the slot unavailable until all in-flight operations release the pin.
2. Use generation-tagged handles or equivalent stale-handle protection; do not expose a reusable array index as the entire lifetime identity.
3. Serialize resampler state and stream format/queue state consistently. Keep all accesses to `gain` under the same lock or use a documented atomic type.
4. Make close and process teardown mark the stream closing, prevent new pins, wait for in-flight operations to drain, then release it.
5. Add adversarial concurrent tests for write/gain/close/process-exit/open/reuse and run under ThreadSanitizer where supported.

F-043 remains OPEN.


## New finding F-044 — Input ring indices and diagnostics have mixed synchronization (2026-11-14)

**Severity:** MEDIUM concurrency correctness; possible dropped/corrupted input under SMP load.

### Evidence
- Keyboard producer holds `prod_lock` and reads `tail` as a plain volatile load to decide whether the ring is full (`input.c:227-229`). Consumers advance `tail` with an atomic release store while holding a different lock, `in_lock` (`input.c:241-247`). The producer and consumer therefore do not share a lock for this index, and one side uses a non-atomic access while the other uses an atomic access.
- Mouse producer `mpush()` reads `mtail` as a plain load (`input.c:132-136`); consumer advances it atomically under `min_lock` (`input.c:154-160`). This repeats the mixed atomic/non-atomic index access.
- `dropped` is incremented by producer paths under `prod_lock`, but `input_dropped()` reads it without that lock or an atomic load (`input.c:228-229,251`).
- Existing `tests/input_test.c` exercises overflow sequentially; it does not concurrently run producers and consumers, so it cannot detect these races.

### Required closure
1. Make each cross-context ring index consistently atomic, including every load/store; use acquire/release ordering appropriate to the SPSC or serialized multi-producer design.
2. Keep producer serialization for multiple input sources, but do not rely on a producer-only lock to synchronize with consumers.
3. Make `dropped` an atomic counter or read it under its writer lock.
4. Add SMP stress tests with concurrent keyboard/mouse producers and multiple consumers, wraparound and overflow assertions; run TSan on host build and targeted QEMU SMP tests.

F-044 remains OPEN.


## New finding F-045 — Graphics event queue has no multi-thread consumer serialization (2026-11-14)

**Severity:** MEDIUM event integrity/concurrency.

### Evidence
- Processes can create multiple user threads through `proc_thread_create()` (`proc.c:480-505`); all share the process PID and address space.
- `gfx_sys_event(pid,...)` obtains the process graphics slot and reads `a->qt`, copies the event, then stores `qt + 1` without a per-application consumer lock or atomic compare/exchange (`gfxsrv.c:126-145`).
- Two threads in the same process can enter concurrently, observe the same `qt`, copy the same queue entry to separate user buffers, and both publish the same increment. One logical event is therefore delivered twice and the next event remains queued. With interleaving around queue wrap/producer updates, queue accounting can also become inconsistent.
- The existing graphics test file exercises drawing/clock/log behavior; no concurrent multi-thread event-consumer test was found in the reviewed fixture.

### Required closure
1. Serialize dequeue and commit for each app slot, or use a single-consumer contract enforced by the syscall/API and tested; do not use an unprotected load/copy/store sequence.
2. Preserve the event on failed user copy while ensuring only one consumer can own the in-flight entry. A per-slot lock or reservation/commit sequence must define this.
3. Add a two-or-more-thread same-PID event-consumer stress test with unique sequence-tagged events, queue wrap, copy failure and close/process-exit cases.
4. Audit app-slot lifetime concurrently with `gfx_proc_gone()` and server reclamation; this finding does not claim that separate lifetime question is closed.

F-045 remains OPEN.


## New finding F-046 — Graphics app slots and shared framebuffer lack in-flight lifetime pins (2026-11-14)

**Severity:** HIGH concurrency/lifetime; potential use-after-free and stale-slot access.

### Evidence
- `find_pid()` and `find_win()` return raw pointers into the reusable global `apps[]` table after an atomic `used == 1` check, without taking `gfx_lock` or retaining a reference (`gfxsrv.c:42-48`).
- Graphics syscalls then use the returned pointer to access metadata and the physical double buffer. For example, `gfx_sys_draw()` copies into `a->phys` and increments `a->seq` after the lookup (`gfxsrv.c:100-120`); event dequeue similarly uses the queue after lookup.
- `gfx_proc_gone()` only sets `need_destroy` on the raw slot pointer (`gfxsrv.c:148-151`). The display server later frees the double-buffer physical allocation and publishes `used = 0` (`gfxsrv.c:163-173`).
- No in-flight syscall reference or generation token prevents the display server from freeing/reusing a slot while a syscall that already passed `find_pid()` is still copying or updating it. A concurrent lookup can also race with slot reuse and observe a new owner through an old pointer.
- The user framebuffer is shared with the process page tables. The reviewed graphics destroy path frees the backing allocation; a complete proof that all user mappings and CPU translations are revoked before reuse was not found in this service path.

### Required closure
1. Add per-slot references/pins acquired atomically with owner/generation validation; reject new pins after transition to CLOSING.
2. Process teardown must mark CLOSING, prevent new syscalls, wait for in-flight references to drain, revoke/unmap the user framebuffer mapping and complete required TLB shootdowns, then free physical pages.
3. Use generation-tagged app/window handles to prevent stale syscalls from acting on a reused slot.
4. Ensure compositor/window callbacks also pin the slot or execute under a documented serialized ownership contract.
5. Add adversarial QEMU SMP tests for draw/event/input/close/process-exit/reopen, stale handles, mapping revocation and allocator reuse; run ASan/TSan host models where applicable.

F-046 remains OPEN.


## New finding F-047 — FAT chain allocation can leak partially allocated clusters on write failure (2026-11-14)

**Severity:** MEDIUM data-space integrity / recovery.

### Evidence
- `chain_alloc()` mutates the FAT incrementally: it marks each free cluster EOC and links it from the previous cluster (`fat.c:132-147`).
- The error paths for a failed `fat_get()` and an insufficient number of free clusters attempt `chain_free(*first)`, but errors returned by either `fat_set(v, c, eoc(v))` or `fat_set(v, prev, c)` return immediately without rolling back the chain already created.
- `fat_write()` calls `chain_alloc()`; on error it invokes `fat_flush(v)` and returns, but does not know or recover the partially allocated chain (`fat.c:670-681`).
- A `fat_set()` failure is reachable when switching FAT sectors requires flushing a dirty FAT-sector cache and the block-device write fails. The already modified FAT entries can remain allocated/orphaned; the caller has no allocation journal/list to release them.

### Required closure
1. Track every cluster allocated during the operation independently of the on-disk chain, and roll back all successfully modified entries on every failure path.
2. Preserve the original I/O error if rollback also fails; mark the volume as requiring remount/recovery rather than reporting it healthy.
3. Add fault-injection tests failing each FAT-sector write boundary during allocation, including both FAT12 cross-sector entries and FAT16/32 multi-sector chains.
4. After simulated reboot/remount, verify free-space accounting and `fsck.fat` consistency; ensure no lost clusters remain.

F-047 remains OPEN.

## New finding F-048 — FAT unlink discards cluster-chain release errors (2026-11-14)

**Severity:** MEDIUM data-space integrity / error reporting.

### Evidence
- `fat_unlink()` removes directory entries, then calls `chain_free(v, d.clus)` when the file/directory has a cluster chain, but discards its return value and unconditionally returns `fat_flush(v)` (`fat.c:721-738`).
- If reading the next FAT entry or writing a FAT sector fails during `chain_free()`, unlink can still return success when the subsequent flush sees no dirty cache or succeeds. The directory entry is already removed, while some clusters may remain allocated and unavailable until repair/reformat.
- Related replacement path in `fat_write()` also ignores `chain_free(v, old.clus)` after publishing the new directory entry (`fat.c:687-694`).

### Required closure
1. Propagate chain-release errors and distinguish “name removed, reclamation incomplete” from complete success.
2. Ensure the volume is marked degraded/remount-required after partial metadata failure; do not silently continue with uncertain allocator state.
3. Fault-inject reads and writes at every chain-free step for FAT12/16/32; remount and verify consistency with `fsck.fat` and free-cluster recount.
4. Define safe recovery for already-unlinked files whose old chain is only partially released.

F-048 remains OPEN.

### KiraFS durability test coverage note
The KiraFS host suite has meaningful write-cutoff/remount tests across multiple operations, including large indirect files. However, its block-device flush callback always succeeds, so it does not validate persistence-ordering failures where writes reach the device but flush reports failure. The suite also uses the host-test lock configuration with no-op locking; it does not validate concurrent callers. These are explicit remaining acceptance gaps, not passing tests.


## New finding F-049 — ARM64 PMM cannot distinguish reserved pages and frees contiguous ranges non-atomically (2026-11-14)

**Severity:** HIGH memory-integrity invariant; MEDIUM confidence of accidental trigger through current call paths.

### Evidence
- `arch/aarch64/pmm_arm.c` has a single `bits` bitmap. `pmm_arm_init()` marks all RAM free, then `pmm_arm_reserve()` marks the kernel image, FDT and early allocations as used. No second reservation bitmap or per-page ownership state is retained.
- `pmm_free_page()` checks only alignment, range and whether the used bit is set. It therefore cannot distinguish an allocated page from a reserved kernel/FDT/early-allocation page; any stale or invalid free of a reserved page clears its bit and returns it to the allocator.
- `pmm_free_contiguous()` calls `pmm_free_page()` one page at a time and returns at the first failure. If an invalid/already-free page occurs after one or more valid allocated pages, the prefix has already been released; the operation is not all-or-nothing.
- The shared contract is explicitly specified in `docs/adr/ADR-0003-pmm-reservation-and-atomic-block-release.md`: separate reservation ownership, reject reserved frees, and validate an entire contiguous range under one lock before changing any state. That ADR describes the required ARM64 parity, but the current ARM64 implementation does not satisfy it.

### Consequence
A stale/double/incorrect free can make reserved physical memory eligible for allocation. A partial contiguous release can also leave ownership and free-page accounting inconsistent after an invalid release request. These are allocator-invariant violations; this audit did not demonstrate a live caller currently freeing a reserved page.

### Required closure
1. Add separate reserved and allocated/free state (or equivalent ownership metadata) on ARM64, and size/validate the bitmap before indexing.
2. Make reserve operations safe for overlapping ranges and keep reserved pages permanently non-freeable.
3. Validate the entire aligned contiguous range under one PMM lock; release all pages or none.
4. Add ARM64 host tests for reserved kernel/FDT/early pages, duplicate free, unaligned/out-of-range free, invalid-middle contiguous release, overlapping RAM regions and unchanged counters on failure.
5. Run ASan/UBSan, ARM64 build and QEMU memory/DMA/SMP/driver scenarios on the exact resulting SHA.

F-049 remains OPEN. Related contract: ADR-0003.


## New finding F-050 — ARM64 non-coherent DMA pool exhaustion silently falls back to cacheable RAM (2026-11-14)

**Severity:** HIGH DMA data-integrity / device-control risk on non-coherent ARM64 platforms.

### Evidence
- ARM64 detects a non-coherent platform from FDT/bootargs and attempts to carve out a non-cacheable DMA pool (`arch/aarch64/main.c:231-254`). The pool is optional: `dma_pool_choose()` can return without creating one if there is insufficient room.
- `platform_dma_pool()` returns NULL both when no pool exists and when the bump allocator has exhausted the pool (`arch/aarch64/main.c:256-268`).
- Both generic DMA allocators treat NULL as permission to fall back to ordinary PMM contiguous pages: `virtio_dma_alloc()` (`virtio.c:50-68`) and `usb_dma_alloc()` (`usb.c:308-326`). Those pages are mapped as normal cacheable RAM. The generic VirtIO/xHCI paths shown do not perform the required per-buffer clean/invalidate cache maintenance; a memory barrier alone does not make non-coherent caches coherent.
- A second fail-open state is present: `platform_dma_noncoherent()` returns `dma_lo != 0` (`arch/aarch64/main.c:255`). If the platform is detected as non-coherent but pool selection fails, it reports coherent. `kdrv.c:206` uses this result to decide whether to reject Ring 3 CAP_DMA packages, so this specific guard is bypassed when the pool could not be created.
- This is separate from the broader Ring 3 IOMMU requirement in ADR-0004 and the existing F-031 bus-master sequencing issue. ADR-0004 correctly states that a non-coherent bounce pool is not IOMMU isolation.

### Consequence
On a non-coherent ARM64 machine, exhausting or failing to create the special pool can place device-visible descriptors/data in cacheable RAM without cache maintenance. Device and CPU can observe stale or partially updated contents, causing silent corruption, failed I/O or DMA protocol instability. The Ring 3 non-coherent guard also loses its signal when the pool is absent.

### Required closure
1. Track platform coherency as an independent immutable platform property; never infer it from whether a pool was successfully allocated.
2. On a non-coherent platform, make DMA allocation fail closed if the non-cacheable pool is absent/exhausted, unless a fully implemented and tested cache-maintenance allocator is used.
3. Keep ordinary cacheable PMM fallback only for positively verified coherent devices/platforms.
4. Add fault-injection tests for no-pool, pool-exhaustion, below-4G exhaustion and partial allocation; assert no device queue/BM activation follows failure.
5. Add ARM64 QEMU/device tests for VirtIO block/net/GPU and xHCI under forced non-coherent mode, with cache visibility and data-integrity assertions.

F-050 remains OPEN.

## New finding F-051 — FDT property-name lookup reads past the bounded strings block (2026-11-14)

**Severity:** HIGH boot-input parser memory-safety risk.

### Evidence
- `fdt_valid()` verifies the strings block offset and declared size are within `totalsize`, but does not validate that every referenced property-name offset points to a NUL-terminated string within that block (`fdt.c:19-25`).
- `fdt_prop()` checks only `no < stsz`, then calls unbounded `streq(strs(fdt) + no, name)` (`fdt.c:80-93`). If the name has no NUL before the end of the strings block, `streq` continues reading beyond that block and potentially beyond the FDT blob.
- The FDT host tests cover malformed header offsets and property payload lengths, but do not cover unterminated property-name strings or name offsets at the final byte of the strings block (`tests/fdt_test.c`).

### Consequence
A malformed firmware-provided device tree can trigger an out-of-bounds read during early boot property lookup. Depending on memory placement this can read unrelated boot memory or fault before the OS has established normal recovery.

### Required closure
1. Replace unbounded property-name comparison with a length-bounded comparison whose NUL search cannot cross `size_dt_strings`.
2. Validate name offsets and NUL termination when reading each property; reject malformed names rather than searching adjacent memory.
3. Validate all FDT block ranges, alignment and non-overlap constraints required by the supported FDT versions.
4. Add malformed-blob tests for unterminated final string, out-of-range offsets, strings overlapping adjacent blocks and fuzzed truncation; run ASan/UBSan and QEMU early-boot negative cases.

F-051 remains OPEN.


## New finding F-052 — ARM64 DMA coherency is inferred from an unrelated device-tree node (2026-11-14)

**Severity:** HIGH on mixed-coherency ARM64 platforms; conditional pending board inventory.

### Evidence
- `dma_noncoherent()` first checks the root `dma-coherent` property, then checks the PCI host node if one exists. If no PCI host node exists, it walks every FDT node and returns “coherent” as soon as any node has a `dma-coherent` property (`arch/aarch64/main.c:231-241`).
- Device-tree `dma-coherent` is a property of the node/device or its applicable bus hierarchy, not proof that every DMA-capable device on the platform is coherent. A single coherent peripheral can therefore cause the global DMA allocator policy to classify unrelated non-coherent devices as coherent.
- The result controls whether the kernel creates the non-cacheable DMA pool and whether `platform_dma_noncoherent()` rejects Ring 3 DMA drivers. The DMA allocator is shared by multiple built-in PCI drivers, so a global false-positive can affect more than the node carrying the property.

### Consequence
On a mixed-coherency ARM64 platform without a PCI host node, one node's `dma-coherent` property may suppress the non-cacheable pool for other devices. Combined with F-050, generic DMA allocations can then use cacheable memory without cache maintenance.

### Required closure
1. Replace platform-wide inference with per-device DMA coherency derived from the device node and its parent bus hierarchy, or use a conservative platform-wide non-coherent policy when per-device routing is not implemented.
2. Do not allow a single peripheral property to upgrade unrelated devices to coherent.
3. Make coherency immutable per DMA device context and pass it into allocator decisions.
4. Add FDT fixtures for mixed coherent/non-coherent devices, absent PCI host, inherited bus properties, root-level property and conflicting declarations; assert each device receives the correct policy.
5. Test that non-coherent devices never use cacheable fallback after pool absence/exhaustion (F-050).

F-052 remains OPEN.


## New finding F-053 — PSCI power commands execute HVC when PSCI is absent (2026-11-14)

**Severity:** HIGH platform power-control fault; conditional on firmware without PSCI or malformed/missing method.

### Evidence
- `psci_hvc` is initialized to `-1` to represent “no PSCI” (`arch/aarch64/main.c`, PSCI section).
- `psci_call()` selects HVC with `if (psci_hvc)`; because `-1` is nonzero, the absent-PSCI state selects HVC instead of rejecting the call. Only the exact value 0 selects SMC.
- CPU bring-up guards calls with `if (psci_hvc >= 0)`, but `platform_poweroff()` and `platform_reboot()` call `psci_call()` unconditionally using SYSTEM_OFF and SYSTEM_RESET function IDs.
- The firmware method parser can leave `psci_hvc == -1` when the node or a usable method string is missing. The status string currently always advertises PSCI regardless of availability.

### Consequence
On ARM64 firmware without PSCI, selecting poweroff/reboot attempts an HVC from EL1. If HVC is not enabled/handled, the operation can raise an exception instead of returning a controlled “unsupported” result; it may panic or halt the system. This is distinct from a firmware that implements PSCI but refuses the requested power operation.

### Required closure
1. Make `psci_call()` reject calls unless `psci_hvc` is exactly 0 (SMC) or 1 (HVC); never treat -1 as HVC.
2. Guard SYSTEM_OFF/SYSTEM_RESET and report unsupported when PSCI is unavailable; retain the returned firmware error when it is present.
3. Parse only exact supported `method = "hvc"\0` or `"smc"\0` values and validate compatible/version support before enabling PSCI calls.
4. Add host policy tests for absent/invalid/HVC/SMC method states and QEMU tests for power operation with PSCI disabled and enabled. Verify no HVC/SMC instruction is issued in the absent case.

F-053 remains OPEN.


## F-025 / PR #125 vs PR #165 — new QEMU evidence (2026-11-14, exact artifacts inspected)

### Earlier failure: PR #125, run 43613583149, commit `fee8c9aed19ffc37dab7fd599c2bd63bfc496924`
- QEMU shard 0 failed `usb-uhci` after 90.7 s; boot prompt was not reached.
- Serial log ends during `[PCI TRACE] disable BM 0:1.2`; the next `BM disabled` marker never appears. This localizes the observed stall to the `pci_disable_bus_master()` call path, but does not prove whether the lock, config read/write or readback is the exact blocking instruction.
- Captured QEMU log is 4,915,946,761 bytes and contains 72,286,473 occurrences of `Invalid read at addr`, repeatedly at 0x0, 0x4, 0x8 and 0xC, plus an initial address 0x3FFDE384. The requester and exact DMA transaction were not identified; do not label these messages as proven UHCI DMA without device/register correlation.

### Later rerun: PR #165, run 43613588426, attempt 2, exact commit `7ad140cfadb51c4dacf809eca77fff60ff7fe12c`
- Exact-head workflow now reports 43/43 jobs successful. The earlier failure was attempt 1; attempt 2 is the latest and successful result.
- `vm (usb-uhci)` job 132514447432 reports `OK usb-uhci` in 11.7 s, boot 5.8 s. The serial log shows UHCI, tablet, hub, keyboard, USB storage, FAT32 mounted at `/usb0`, and successful `cat /usb0/hello.txt` returning `kira-ci: флешка стенда`.
- The passing artifact's QEMU log is 26,767,488 bytes but still contains 393,602 `Invalid read at addr` records, again including 0x0/0x4/0x8/0xC. Functional scenario PASS therefore does not close the invalid-read/DMA anomaly. The reduced count is not proof of root-cause correction; the QEMU log is still noisy and needs requester-level diagnosis.
- The two runs are different PR branches/bases and not a controlled A/B comparison. PR #125 targets `autodrv`; PR #165 targets `main`. Do not infer the precise cause from this comparison.

### Current disposition
- PR #165 exact-head CI: **43/43 PASS on attempt 2**; functional UHCI smoke scenario passes.
- F-025 remains **OPEN** because the invalid-read storm persists and no requester/register proof identifies its source.
- F-017/F-029/F-030 sequencing concerns remain open; a green boot test does not prove safe DMA quiesce, reset, queue setup, teardown or failure recovery.


### Correction and ARM64 evidence — exact PR #165 run enumeration (2026-11-14)

The workflow jobs API was paginated by the first-page wrapper. Full run enumeration shows PR #165 run 43613588426 attempt 2 completed **43/43 jobs successfully**, not merely 30 jobs. This includes host-tests, x86 build, all VM matrix jobs and the ARM64 job.

ARM64 artifact 14121710332 was downloaded and inspected:
- 16 ARM64 QEMU scenarios report OK: GICv2, GICv3+ITS, ARM64 compiler, EL2/eight CPUs, GUI, Explorer, isolation, driver manager, faults/crashes/stack overflow, ramfb, USB, power and non-coherent DMA.
- GICv2: 2/2 CPUs online; GICv3: 4/4; EL2 scenario: 8/8 using PSCI SMC; standard QEMU scenarios use PSCI HVC.
- Non-coherent scenario creates a 32 MB DMA pool and successfully exercises xHCI storage. This validates the normal pool-present path, not pool absence/exhaustion (F-050) or mixed-coherency detection (F-052).
- Power scenario verifies reboot and poweroff when PSCI HVC is available; it does not test the absent-PSCI state in F-053.
- FDT malformed-name coverage (F-051) is not part of these 16 scenarios.

The #165 run is therefore strong emulator evidence for the supported ARM64 happy paths, but does not close the specific negative-path findings above. It is not physical ARM64 hardware validation. The separate x86 UHCI artifact still has 393,602 rejected QEMU invalid-read records, so F-025 remains open.


## New finding F-054 — USB controllers regain Bus Master before being stopped/reset (2026-11-14)

**Severity:** HIGH; potentially explains the persistent QEMU invalid-DMA reads, but correlation is not yet proven.

### Evidence
- PCI enumeration in PR #165 clears firmware Bus Master for non-bridge functions, but only after `probe_bars()` has completed. The BAR probe disables IO/MEM decode, not Bus Master. Thus a firmware-enabled controller can retain Bus Master throughout BAR probing (F-017 remains relevant).
- `uhci_start()` then calls `pci_enable(... PCI_CMD_MASTER ...)` at the beginning of startup, before allocating the 13-page frame-list/QH/TD/buffer region and before writing FRBASE or issuing USBCMD stop/global reset/host reset (`uhci.c:386-418`). If firmware left UHCI RUN set with stale FRBASE, re-enabling Bus Master can let it fetch stale descriptors before the driver has quiesced it.
- On allocation failure after the enable, the function returns without first clearing Bus Master or forcing the controller halted.
- `xhci_start()` has the analogous ordering: enables MEM+MASTER at `xhci.c:529`, before mapping BAR, BIOS handoff, clearing Run/Stop, waiting for Halted, issuing HCRST, and allocating DCBAA/command/event rings (`xhci.c:531-564`). Failure paths can also return after Bus Master is enabled without a common quiesce/disable rollback.
- The PCI scan's late clear is a mitigation but does not make driver re-enable safe; it does not prove the controller is halted or that firmware-owned DMA pointers are invalidated.

### Consequence
A running controller can resume DMA against firmware-owned/stale schedule or context pointers as soon as Bus Master is re-enabled. This can cause invalid DMA reads and memory corruption before the driver's own DMA structures are ready. The QEMU UHCI artifact's invalid-read records make this a high-priority hypothesis, not a demonstrated one: no register snapshot or DMA requester trace yet ties those log entries to UHCI.

### Required closure
1. Keep Bus Master disabled while mapping registers, taking ownership, stopping controller, waiting for Halted, resetting, and allocating/initializing every DMA structure.
2. Clear/disable old controller DMA pointers and pending events before reset/ownership transition where hardware spec requires it.
3. Program validated schedule/ring addresses and barriers, then enable Bus Master as the final activation step immediately before starting the controller.
4. Add one cleanup path for every failure after ownership: stop, wait Halted, clear Bus Master, quarantine/free only after DMA is proven quiescent, release mappings.
5. Add QEMU register trace snapshots before/after PCI enumeration, immediately before/after BM enable, before/after reset and before RUN; assert no DMA before initialized schedule/rings.
6. Extend host tests to assert call ordering and injected allocation/reset/mapping failure always leaves BM disabled and controller halted.

F-054 remains OPEN. Do not close F-025 until requester/register evidence confirms the QEMU invalid-read source and the error log is clean.


## New finding F-055 — QEMU guest_errors artifact mixes firmware and OS activity without a phase boundary (2026-11-14)

**Severity:** MEDIUM test-observability gap; it blocks attribution of the current invalid-read anomaly.

### Evidence
- `ci/kira_vm.py:140-145` launches QEMU with `-d guest_errors -D <scenario>/qemu.log` from process start, before firmware/SeaBIOS execution and before KIRA's bootloader/kernel.
- The runner records serial output separately (`serial.log`) and never records a QEMU log byte offset, timestamp, or guest-phase marker at the moment the OS reaches its ready prompt.
- The PR #165 UHCI success artifact has 393,600 `Invalid read` records: 384 distinct addresses in two ranges (0x3FFDE000–0x3FFDE2A8 and 0x3FFDECAC–0x3FFDEFFC), plus exactly 98,304 reads each at 0x0, 0x4, 0x8 and 0xC. The first logged invalid read appears at the beginning of qemu.log. The artifact has no timestamps or phase labels to determine whether these reads happened in firmware, KIRA PCI enumeration, UHCI startup, or later guest activity.
- Serial shows the OS eventually detects UHCI, keyboard/tablet/hub/storage, mounts FAT32 and reads the test file. This proves functional success, but does not timestamp or attribute the QEMU error records.

### Consequence
The previous audit correctly kept F-025 open, but the invalid-read count alone cannot be used as evidence that KIRA's UHCI driver generated the errors. Firmware may be responsible for some or all of the records; the current artifact format cannot distinguish this. F-054 is a real source-ordering hazard independent of that attribution question.

### Required closure
1. Change the VM runner to capture a QEMU guest_errors log boundary at the exact serial boot-ready marker and report separate pre-ready and post-ready counts.
2. For focused USB diagnostics, add structured serial markers before PCI enumeration, before/after UHCI BM enable, before/after USBCMD stop/reset, FRBASE programming and controller RUN.
3. Preserve full raw QEMU log but summarize post-ready errors separately; don't delete firmware evidence.
4. Add a negative-control QEMU run without USB device/driver activation to estimate firmware-only noise.
5. Close F-025 only after post-ready errors are attributed to a requester/device and the targeted guest phase is clean.

F-055 remains OPEN.


## New finding F-056 — xHCI slot retry dereferences partially allocated DMA buffers (2026-11-14)

**Severity:** HIGH under DMA allocation failure during USB device enumeration.

### Evidence
- `xhci_enumerate()` allocates `xs.slot[sl].in`, `out`, and `buf` sequentially at `xhci.c:437-439`. If any allocation or EP0 ring initialization fails, it calls `usb_dev_free(d)` and returns, but does not reset the partially populated slot fields.
- The retry decision at `xhci.c:436` uses only `!xs.slot[sl].in` as the initialization flag. If `in` allocation succeeded but `out` or `buf` failed, `in` remains non-NULL.
- On a later attempt that reuses the same xHCI slot, the `else` branch at `xhci.c:445-447` writes through `xs.slot[sl].out[i]` without checking it. If the earlier `out` allocation failed, this dereferences NULL in kernel context. If `buf` or EP0 ring allocation failed, the retry can proceed with incomplete state and later use missing DMA memory.
- `usb_dev_free()` calls `xhci_release()`; that disables the slot and clears the DCBAA entry, but does not roll back or normalize the partial per-slot allocations. The shared `usb_dma_alloc()` has no public free operation, so the current path cannot transactionally unwind these buffers.

### Consequence
A transient DMA allocation failure can leave a partially initialized slot. If the controller assigns that slot again on a later device-enumeration attempt, the retry path may dereference a NULL DMA buffer and crash the kernel. This is distinct from the controller-start Bus Master ordering issue in F-054.

### Required closure
1. Replace the single-pointer initialization sentinel with explicit per-slot initialization state and validate every pointer before use.
2. Make allocation retry-safe: either add a real DMA free/rollback contract or retain each successfully allocated buffer and allocate only missing resources on retry.
3. Do not mark a slot ready until IN/OUT contexts, transfer buffer and EP0 ring are all valid.
4. Add deterministic allocation-failure injection at each of the four allocation stages, force the same slot to be reused, then assert retry succeeds or returns a controlled error without NULL access, stale DCBAA state or leaks.
5. Run the xHCI host model under ASan/UBSan and the QEMU xHCI/USB hub/storage scenarios.

F-056 remains OPEN.


### Comparative QEMU evidence from the phase-aware baseline run (2026-11-14)

- PR #168, exact head `7ad4b30cf9e7cbb2cefaecaa06359838c5971841`, run `43620850523`: **43/43 jobs PASS**. Its actual `vm-usb-uhci` and `vm-boot-virtio` artifacts were inspected. Both `qemu.log` files are 0 bytes; diagnostics report 0 invalid reads and 0 invalid writes before and after guest-ready.
- PR #165, run `43613588426`, exact head `7ad140cfadb51c4dacf809eca77fff60ff7fe12c`: prior UHCI artifact had 393,602 invalid-read records in its QEMU log while its functional scenario passed.
- This is a useful differential: the invalid-read storm is not reproduced on current-main baseline and appears in the PR #165 branch. It strengthens F-054 (PCI Bus Master is cleared during enumeration, then UHCI/xHCI re-enable it before reset and DMA setup) as a causal hypothesis.
- It is **not a controlled A/B**: the two runs use different source heads, and PR #165 contains other changes. Do not claim that F-054 is proven to generate every record. PR #169 is a stacked fix that tests the revised activation order on the PR #165 base; its exact-head CI and artifacts are still required.
- The phase-aware runner itself passed 43/43 on #168 and wrote diagnostics for both scenarios. The log was genuinely empty, not a missing diagnostic file.


### QEMU confirmation: xHCI ERSTBA can trigger DMA before controller RUN (2026-11-14)

- PR #169 first QEMU attempt, run `43621930596`, UHCI/xHCI stack on PR #165 base: xHCI controller initialization reached the console, but the USB scenario failed. Serial reported `[KIRA USB] xHCI command timeout` and `no free slot`.
- The QEMU guest log recorded `xhci_er_reset: DMA memory access failed!` and rejected reads at `0xE15000`, `0xE15004`, `0xE15008`, `0xE1500C`. In that attempt, the driver wrote ERSTBA while PCI Bus Master was still disabled; `0xE15000` was the ERST table address.
- This demonstrates that, in QEMU's xHCI model, writing ERSTBA can immediately cause a DMA fetch even before USBCMD.RUN. Therefore the safe sequence is not simply “enable BM immediately before RUN”: allocate and initialize all DMA objects with BM off, grant BM while the controller remains halted, then expose DCBAAP/CRCR/ERST pointers, and finally set RUN.
- PR #169 has been adjusted to that order and the xHCI host model now asserts that DMA objects exist while the pointer registers are still zero at the BM grant point. The corrected exact-head QEMU/host CI is pending; do not close F-054 until it passes.


## New finding F-057 — failed USB controller startup leaks DMA allocations (2026-11-14)

**Severity:** MEDIUM; resource exhaustion risk when multiple controllers or initialization failures are encountered.

### Evidence
- \x60usb_dma_alloc()\x60 allocates either from the platform DMA pool or via \x60pmm_alloc_contiguous()\x60 and exposes no matching USB DMA free/release API.
- \x60uhci_start()\x60 allocates controller state and a 13-page DMA region for the frame list, QH/TD structures and transfer buffers. If halt/reset or a later startup step fails, it returns without releasing the successful allocations. \x60nctl\x60 increments only after complete startup, so the next controller attempt reuses the same index but allocates another schedule region.
- \x60xhci_start()\x60 similarly allocates controller state, DCBAA, command ring, optional scratchpad array/pages, event ring and ERST. Failure at a later allocation, reset, register validation or startup step returns without transactional rollback. \x60started\x60 increments only after full success, so the next controller attempt reuses the same slot while prior allocations remain consumed.
- Existing xHCI host tests inject failures for per-device slot IN/OUT/buffer/EP0 allocations and retry the same device slot. They do not inject failures into controller-start allocations or verify that a second controller can start after the first fails. The UHCI host test covers successful startup, not failed halt/reset/allocation followed by another controller.

### Consequence
A failed controller can permanently consume DMA pool/physical memory. A UHCI failure can strand at least the 13-page schedule region; xHCI scratchpad allocation can strand a variable number of pages. Subsequent supported controllers may fail for lack of DMA memory even when enough memory would have existed if failed initialization had rolled back.

### Required closure
1. Introduce an explicit DMA allocation ownership/transaction object for controller startup, with a real release operation for PMM-backed pages and a reclaimable or safely quarantined platform-pool contract.
2. Do not publish \x60started\x60/\x60nctl\x60 or controller state until all required resources and hardware state are committed.
3. On failure, keep Bus Master disabled, ensure the controller is halted, then release only allocations proven unreachable by hardware; never free DMA pages while a device can still access them.
4. Add deterministic failure injection at each controller-level allocation and reset/halt stage; after each injected failure, start a second controller and assert DMA accounting is restored and no stale pointers are programmed.
5. Run host ASan/UBSan tests and the full x86-64/ARM64 USB matrix where applicable.

F-057 remains OPEN. This is a source/test-coverage finding; no runtime reproduction or production fix is claimed.


## New finding F-058 — FDT string-property consumers can read beyond property bounds (2026-11-14)

**Severity:** MEDIUM; malformed firmware-supplied device tree can cause out-of-bounds reads during platform discovery.

### Evidence
- `fdt_prop()` validates the property token and value length within the structure block, but it returns the raw value pointer and length without validating string termination for string-valued properties.
- `fdt_count_type()` obtains `device_type` and calls unbounded `streq(t, type)`; the property length is checked only for nonzero. A malformed property with no NUL byte within its declared length causes the comparison to continue into alignment padding or subsequent structure tokens.
- `fdt_node_enabled()` obtains `status` and similarly calls unbounded `streq(s, "okay")` / `streq(s, "ok")` without requiring a NUL within the property length.
- `fdt_compatible()` has a bounded string-list walk, so this is not a blanket claim about every FDT string consumer. The issue is specifically the plain string properties consumed by the two helpers above.
- Existing `tests/fdt_test.c` covers invalid header/size/structure lengths and traversal, but does not construct a non-NUL-terminated `device_type` or `status` property and run those helpers under ASan.

### Consequence
A malformed FDT can make CPU/device enumeration and node-enabled checks read outside the declared property value. Depending on adjacent bytes, this can produce false classification or an out-of-bounds read; when the value ends at the end of the structure block, the read may leave the block entirely.

### Required closure
1. Add a bounded string-property helper that verifies a NUL exists within the declared property length before comparison; use it for `device_type` and `status`.
2. Decide whether malformed string properties should make the individual property unavailable or invalidate the complete FDT; document the policy consistently.
3. Add malformed values with no NUL, empty values, exact-length valid strings, and boundary-at-end cases for both helpers.
4. Run host ASan/UBSan and ARM64 QEMU FDT/boot tests.

F-058 remains OPEN. Source/test-coverage finding only; no production fix or runtime reproduction is claimed.


### Stacked-fix QEMU results: xHCI clean, UHCI still noisy (2026-11-14)

- PR #169 intermediate exact head `6fd90d39b87766574e427991eb79d66b33eaab4b`, run `43622220409`: both targeted QEMU jobs completed successfully before the run was superseded by additional diagnostics work.
- `usb-xhci`: 12.0 s, all checks passed (xHCI, 8-port hub, keyboard, tablet, USB storage, FAT32 mount, read/write/unmount). QEMU log is 0 bytes; no `Invalid read`, `Invalid write` or `xhci_er_reset: DMA memory access failed` records. This supports the corrected xHCI order: Bus Master enabled after allocations but before ERSTBA/pointer exposure.
- `usb-uhci`: functional checks also passed (tablet, hub, keyboard, USB storage and FAT32 read), but QEMU log remains 40,081,525 bytes with 589,375 invalid-read records. 579 distinct addresses; 147,200 reads each at 0x0, 0x4, 0x8 and 0xC. The QEMU log is not clean, so F-025 and the UHCI portion of F-054 remain OPEN.
- This is evidence that the xHCI fix addresses its immediate ERSTBA DMA failure, while the UHCI anomaly is separate or occurs earlier in the PCI/firmware handoff. The latest branch adds serial/QEMU offset snapshots at PCI scan, controller start and OS-ready to localize the remaining storm. Exact-head CI for that instrumentation is pending.


## New finding F-059 — UDP socket ownership is not synchronized with slot lifetime (2026-11-14)

**Severity:** HIGH syscall ownership / concurrent teardown race.

### Evidence
- `udp_gone(pid)` iterates the global `us[]` socket table and clears `owner` without acquiring `ulk`.
- `uown(h)` reads `owner` and the socket slot without `ulk`; it is called at the start of send, receive and close operations.
- `net_udp_sys()` validates ownership once, then uses the slot across operations that may allocate memory, copy user data, wait for a datagram, resolve a neighbor or transmit. It does not pin the slot or revalidate a generation before committing.
- `NET_UDP_CLOSE` clears `owner` under `ulk`, but a concurrent operation that passed the earlier unlocked `uown()` can continue using the same slot. Another process can then open the freed slot and reset its ring/owner while the old operation is still active.
- `udp_deliver()` correctly takes `ulk` while locating a socket and appending to its ring; this does not protect a syscall that already retained the slot index outside the lock.
- Existing `tests/net_test.c` covers only checksum and IPv4 text parsing. It does not instantiate the kernel UDP table or test ownership, close, process teardown, slot reuse, concurrent send/receive, or packet delivery.

### Required closure
1. Use a consistent locked ownership check and slot-lifetime protocol; do not treat a successful owner check as a durable reference.
2. Add generation-tagged socket handles or reference/pin counts so close/process teardown cannot recycle a slot while an operation is active.
3. Make open, close, process teardown and ring reset one atomic lifecycle transition under `ulk`; define how in-flight operations drain or fail.
4. Audit lock ordering between `ulk`, process teardown, user copies, memory allocation and network resolution; never sleep or perform potentially blocking work while holding the spinlock.
5. Add host-testable UDP table/lifecycle tests plus SMP stress for close-vs-send, close-vs-recv, process-exit-vs-delivery, and immediate cross-process slot reuse. Run TSan where supported and targeted QEMU SMP tests.

**Evidence boundary:** source-confirmed race and missing test coverage. No runtime reproduction or packet-level exploit is claimed.

### Implementation tracking
- Draft PR #171: https://github.com/Masuku4129/kira_os_/pull/171
- Adds generation-tagged UDP handles, slot references/pinning across syscalls, synchronized owner teardown, and regression tests for handle encoding.
- Exact-head workflow rerun completed **30/30 jobs successfully**. The initial run had one unrelated timing-sensitive Driver Manager output check; rerun passed.
- Finding remains OPEN on `main` until the candidate is integrated and lifecycle stress coverage is expanded.


## New finding F-057 — PCI enumeration revokes UHCI Bus Master but leaves firmware controller running (2026-11-14)

**Severity:** MEDIUM; QEMU DMA-denial storm / early-device quiesce gap. No successful DMA into guest RAM is demonstrated.

### Evidence
- PCI enumeration in `pci.c:186-200` probes the UHCI BAR, clears PCI Bus Master and prints `[PCI TRACE] secured`, but does not stop the controller's USBCMD.RUN state or verify USBSTS.HALTED.
- The UHCI driver is started later, after the full PCI scan. Its `uhci_start()` first revokes BM, then writes USBCMD=0 and waits for HALTED before reset and schedule initialization.
- PR #169 phase-aware artifact `usb-uhci`, run `43622980003`, contains 393,600 QEMU invalid-read records (26,767,488 bytes). The repeated addresses are 0x0/0x4/0x8/0xC, consistent with an active controller attempting to fetch a frame list at a zero base while DMA is rejected. The no-USB `boot-virtio` control artifact on the same branch has a zero-byte QEMU log.
- Serial/QEMU offset snapshots show the log has grown by the time the PCI scan reaches later functions and is fully populated by the `[KIRA USB] UHCI at I/O` startup marker; no further records appear after the guest-ready marker. QEMU log buffering means offsets are approximate, not a cycle-accurate attribution.
- The PCI scan's `secured` marker does not prove that the controller is halted. The later driver startup explicitly stops it; the QEMU storm ceases by that point.

### Consequence
Clearing Bus Master prevents the device from successfully accessing guest memory, but a firmware-running UHCI controller can continue issuing rejected DMA requests for the interval between PCI enumeration and USB driver startup. This creates a large QEMU error log and leaves hardware active in an undefined pre-driver state.

### Required closure
1. After revoking Bus Master for a UHCI function, perform an early, bounded quiesce using BAR4: enable I/O decode only as needed, write USBCMD=0, poll USBSTS.HALTED, then restore the original I/O-decode state while keeping BM off.
2. If the controller does not halt, keep BM revoked, report the failure and let the normal driver perform controlled recovery; never re-enable BM on the stale schedule.
3. Add a host test with mocked port I/O for successful halt, timeout, BM-off precondition and restoration of I/O decode.
4. Re-run QEMU `usb-uhci` and require the raw `guest_errors` log to be clean or limited to a documented bounded transition; preserve the no-USB control.

F-057 remains OPEN. This finding is separate from F-054's late BM re-enable ordering and F-025's unresolved DMA log anomaly.


## New finding F-060 — UDP transmit reports success after link-layer enqueue failure (2026-11-14)

**Severity:** MEDIUM syscall result correctness / silent datagram loss.

### Evidence
- `netdev_t.send` documents a zero return as successful handoff to the NIC; both `e1000.c::send()` and `virtio_net.c::send()` return a negative error when their transmit ring/buffers are unavailable.
- `eth_send()` propagates the NIC driver's return value, and `ip_send()` / `ip6_send()` propagate an `eth_send()` failure.
- `udp_xmit()` calls `ipa_send(...)` but discards its return value, then unconditionally returns the payload length. Therefore UDP SEND/SENDTO can report all bytes sent when the NIC explicitly rejected the frame because its TX queue is full or the frame could not be enqueued.
- The existing `tests/net_test.c` does not test transmit propagation or UDP syscall behavior.

### Required closure
1. Propagate `ipa_send()` failures from `udp_xmit()` to the syscall caller; define clearly whether a positive byte count means local NIC-queue acceptance, not remote delivery.
2. Add a fake netdev whose send callback deterministically returns success and failure; test IPv4 and IPv6 UDP send result propagation.
3. Test e1000/virtio TX-ring-full paths and verify syscall reports failure rather than payload-length success.
4. Keep asynchronous network delivery semantics distinct from local enqueue success; do not imply remote receipt without an acknowledgement protocol.

**Evidence boundary:** source-confirmed ignored error path. No remote packet-delivery claim is made.

### Implementation tracking
- Draft PR #170: https://github.com/Masuku4129/kira_os_/pull/170
- Propagates the `ipa_send()` result through `udp_xmit()` and adds deterministic host regression tests for success, negative enqueue errors and zero-length payloads.
- Exact-head CI completed **30/30 jobs successfully**.
- Finding remains OPEN on `main` until the candidate is integrated; driver-level TX-ring-full coverage remains a follow-up.


## New finding F-061 — FDT property-name lookup is not bounded by the strings block (2026-11-15)

**Severity:** MEDIUM; malformed firmware-supplied DTB can cause an out-of-bounds read during property lookup.

### Evidence
- `fdt_valid()` checks that the strings block extent `size_dt_strings` lies within `totalsize`, but it does not validate that every referenced property-name string is NUL-terminated inside that extent.
- `fdt_prop()` reads the property-name offset `no`, checks only `no < stsz`, then calls the unbounded `streq(strs(fdt) + no, name)`.
- `streq()` walks until it encounters a NUL or a differing character. A malformed DTB can place a property-name offset at the final byte of the strings block and omit the terminator; lookup can then read alignment bytes, another DTB block, or beyond the supplied blob.
- The existing FDT tests cover block-size/header bounds and ordinary property lookup, but do not create a strings block whose referenced property name has no NUL within the declared strings extent. The existing malformed-property case changes a structure token; it does not exercise this strings-block boundary.

### Consequence
A firmware-controlled property-name offset can make the kernel read beyond the declared FDT strings block while searching for a property. This is a separate boundary from F-058, which concerns unterminated *property values* consumed as `device_type` and `status`.

### Required closure
1. Add a bounded property-name comparison that receives the remaining strings-block length and succeeds only when a NUL terminator is found within that length and the complete name matches.
2. Reject or safely ignore a property whose name offset is out of range or whose name is not terminated inside the strings block; document one consistent policy.
3. Add host regressions for an offset equal to `size_dt_strings`, an offset at the final byte with no terminator, a terminator exactly at the final valid byte, and a valid name followed by another string.
4. Run the FDT host suite under ASan/UBSan and the ARM64 boot/QEMU matrix on the exact candidate head.

**Evidence boundary:** source-confirmed unbounded read path and missing boundary coverage. No runtime reproduction or production fix is claimed.

F-061 remains OPEN.


## F-062 — WASM validation PR stack fails C builds due to undeclared fixed-width integer types (2026-11-15)

**Severity:** CRITICAL — current `main` is not build-green; also blocks the stacked PR #179/#181/#182.  
**Scope:** current `main` @ `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e` and open stacked PRs #179/#181/#182.  
**Evidence:** exact GitHub Actions logs for current-main push run `43643758138` and the recorded PR heads.

### Evidence

- PR #179 head `2fdb2c04fc3d5a938c547ad3b8285bebd1a7f736`, run `43644011093`: x86 build, ARM64 build and all QEMU scenarios succeeded, but `host-tests` failed.
- The failure is in `userland/wasm.c:145`: `int32_t` and `uint32_t` are undeclared in the WASM signed-LEB/type conversion path.
- Dependent PR #181 head `17614b29e4bbd75ae1dd14fd7db0772d14449200`, run `43643351332`: host-tests, x86 build and ARM64 build all failed. The x86/ARM64 compiler logs show the same undeclared `int32_t`/`uint32_t` identifiers in `userland/wasm.c:145`.
- Dependent PR #182 head `49e2914e620e6ac6e98adbdd49b2f04936b2a7d3`, run `43644247467`: host-tests, x86 build and ARM64 build all failed at the same `userland/wasm.c:145` undeclared type error.
- PR #181 is based on an intermediate commit in the PR #179 branch; PR #182 is based on PR #179 head. These are stacked changes, so the shared compile defect blocks verification of their independent input/GFX fixes.
- The PRs are open drafts. No merge has occurred. This finding does not imply the same failure exists in current `main`.

### Required closure

1. Inspect the freestanding typedef/header conventions used by `userland/wasm.c`; use the project's existing fixed-width types or include a compatible freestanding definition without importing host libc assumptions.
2. Run the exact host-test command locally/CI and verify signed-LEB boundary tests.
3. Re-run x86-64 build, ARM64 build and the dependent PR checks on the exact corrected heads.
4. Verify PR #181 and #182 independently after the common base is fixed; do not inherit a PASS from an earlier parent SHA.
5. Keep the stacked PR dependency graph explicit and do not merge a subset that silently drops later fixes.
6. After correction, rerun the current-main workflow-equivalent jobs and verify the full QEMU matrix is no longer skipped.

**Disposition:** OPEN — confirmed current-main build break and stacked-PR blocker. No production fix has been applied in this Stage 0 audit pass. The current-main exact-head CI result is FAIL, not PASS.


## F-063 — FDT v16 header is accepted but parsed using the v17-only structure-size field

**Severity:** MEDIUM; firmware compatibility / parser correctness.  
**Confidence:** HIGH — direct source evidence in current main.  
**Mapping:** A, B, F, L, M, N; Stage 1 (BootInfo/firmware handoff), Stage 3 (memory ownership), Stage 0 parser validation.

### Evidence on current main `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`

- `fdt_valid()` rejects versions below 16, so version 16 is explicitly accepted.
- It unconditionally reads header word 9 (`size_dt_struct`) for the structure-block length. That field is defined in the v17 header; v16 requires deriving the structure extent from the next block offset rather than trusting this field.
- The current host fixture in `tests/fdt_test.c` uses version 17 and does not include a v16 compatibility case.
- ARM64 boot calls `fdt_valid()` directly on the firmware-provided DTB, so this is on the boot-critical parser path.

### Candidate remediation already present in PR #166

- PR #166 changes the structure-size calculation: explicit `size_dt_struct` for v17+, and `off_dt_strings - off_dt_struct` for v16.
- Its body records a v16 regression fixture. The currently exposed PR head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e` completed workflow `43610515593` with 30/30 jobs successful.
- This candidate evidence does not change current main and does not close F-063 until reviewed and integrated.

### Closure

1. Retain the v16 and v17 fixtures and verify the derived v16 extent cannot cross another block.
2. Test malformed v16 offsets, truncated header, and block ordering under ASan/UBSan.
3. Re-run exact-head host tests, x86 build, ARM64 build and QEMU matrix after any subsequent PR changes.

**Disposition:** OPEN on main; candidate fix verified on draft PR #166.


## F-064 — FDT host-test corpus does not cover accepted v16 and string-table boundaries

**Severity:** HIGH — parser accepts FDT v16 headers but the only host fixture is version 17, leaving an accepted-format path untested.  
**Scope:** current main `fdt.c` and `tests/fdt_test.c`.  
**Evidence:** current-main source SHA `2e8541a28517cb4b6a5382566e99211c8350200a`; test SHA `3b00bda0822288fcf89b2042d4106180007bd3c0`. The fixture writes version 17 / last-compatible 16. No v16 fixture or tests for strings-block final-byte NUL, unterminated property names, `nameoff == size_dt_strings`, or malformed string offsets were found. `fdt_prop()` uses `streq(strs(fdt) + no, name)` after only `no < stsz`, so it relies on a NUL terminator inside the declared strings block that `fdt_valid()` does not validate. This is a boundedness defect candidate requiring regression reproduction before code changes.
**Disposition:** OPEN — test gap and source risk recorded; no production fix applied. Related existing findings F-058/F-061/F-063 remain separate.
