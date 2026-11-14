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
| F-016 | FDT header `mem_rsvmap` and ARM64 PMM init | Draft PR #166 validates and consumes firmware reservation entries before allocator exposure | Exact-head CI pending; host FDT sanitizers + ARM64/QEMU required | FIRMWARE MEMORY RESERVATION FIX UNDER VERIFICATION |
| F-015 | FDT reg index/address-size overflow | Draft PR #159 exact head `1a61bd5bdc11b3dd0338c3142113c963308598bf` | Workflow `43592382535`: 43/43 PASS incl. ASan/UBSan FDT tests, ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-014 | ARM64 PMM contiguous release is non-atomic | Draft PR #158 validates whole range under one lock | Workflow `43592051859`: 43/43 PASS incl. host, ARM64 and QEMU | VERIFIED ON DRAFT PR; main unchanged |
| F-012 | Driver-start log line split at 192-byte buffer boundary | QEMU serial artifact `43591102543` confirms driver success but interleaved log | Draft PR #157 exact head `59b391696baabfd49f2ff48acf5aba2b9db538de`; run `43591626068` 43/43 PASS including ARM64 and QEMU `drv-edu` | FIX VERIFIED ON DRAFT BRANCH; main unchanged, review/merge pending |
| F-009 | PMM contiguous block release | Source review and caller inventory | PR #153 exact-head CI 43/43 PASS; combined PR #154 integration pending | FIX VERIFIED ON DRAFT BRANCH; combined integration pending |

No code was changed on `main` by this audit. All implementation changes remain in separate unmerged Draft PRs. No test was executed locally. Exact-head CI evidence from other PRs is branch-only and is not transferred to `main`.


## Follow-up — F-002 / F-016 exact-head evidence and PMM dependency (2026-11-14)

- **F-002 firmware handoff:** PR #165 exact head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, workflow `43603442098`, completed successfully; all 30 returned jobs succeeded, including `vm (usb-uhci)`. This supersedes the earlier entry that described the UHCI regression as still open on the previous head. Temporary diagnostic traces were removed afterward; cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb` has no exact-head CI result yet. Keep F-002 branch-only until that cleanup head passes. Real per-device VT-d/SMMUv3 domains remain absent; PR #161's fail-closed Ring 3 DMA/MSI gate does not implement IOMMU.
- **F-016:** PR #166 exact head `9d6443ff2db7195e8abf0c6d63a6257c1c38f229`, workflow `43602565437`, completed successfully; all 30 returned jobs succeeded. This verifies the FDT header reservation-map parser and ARM64 boot integration on that Draft PR head.
- **F-016 integration caveat:** PR #166 is based on main and uses the current main `pmm_arm_reserve()`. Main's ARM64 PMM has no separate reserved-page bitmap, so a later `pmm_free_page()` can clear a page reserved by the new call. PR #158 implements reserved-page ownership and atomic contiguous release, exact head `70d0e89a65414818c22d61532f7ca4288c464d2c`, workflow `43592051859`, 43/43 PASS. Treat #158 as a prerequisite or integrate #158+#166 as one reviewed change set. Add an integration test that proves a page described only by FDT `mem_rsvmap` remains unavailable after PMM initialization and cannot be freed.
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
- PR #166's earlier exact head `539285048cdd97f5d71ba5aefc9afda4058b7512` passed 30/30 jobs, but the corrected head is now `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`; its own CI is still required. The prior green result does not verify this new fix.


## F-022 — scheduler stack fallback weakens guard and ownership guarantees (2026-11-14)

**Severity:** HIGH. Integrated fix is under exact-head verification.

- PR #154 adds generation-token ownership for guarded VMM stack slots and atomic PMM contiguous release. PR #164 independently adds fail-closed allocation and stack-memory clearing, but overlaps `kstack.h`, `sched.c`, `sched.h`, and scheduler tests.
- PR #154's initial implementation still fell back to `kmalloc(SCHED_STACK_SIZE)` when guarded VMM allocation failed. That made stack-window exhaustion silently weaken the guard guarantee.
- Integrated the non-fallback policy and stack clearing into PR #154's generation-aware branch. New production threads now fail creation if guarded stack allocation fails; allocated stack memory is zeroed before publishing the thread and then receives the stack canary.
- Added host coverage that checks the canary and verifies the stack body is zero while excluding the architecture's initial context frame.
- Current integration candidate: PR #154 head `8f4b9be58f63d93911817184dcbbce0e4eca36c1`. Exact-head CI is not yet available. Prior PR #154 success (43/43) predates this integration and cannot be used as its validation.
- PR #153 duplicates the atomic PMM change already included in #154; PR #164 overlaps scheduler changes. Do not merge the duplicates independently. Keep #154 as the candidate pending fresh CI and review.

F-022 remains OPEN pending exact-head CI and source-level review of all architecture-specific stack allocator paths.


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

- PR #166 corrected head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`, workflow `43610515593`: **30/30 jobs succeeded**.
- This exact run includes host-tests (including FDT v16 and one-byte overlap regression), x86 build, ARM64 build/scenarios, and all VM matrix scenarios including `usb-uhci`.
- This verifies the FDT validator correction and the FDT reservation-map/ARM64 PMM integration on the PR branch. It does not integrate the separate branch-only IOMMU backends or close F-021.

