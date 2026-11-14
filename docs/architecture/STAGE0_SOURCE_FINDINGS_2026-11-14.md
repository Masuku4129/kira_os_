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

**Required closure:** first establish a fail-closed DMA/MSI policy; then implement and test real per-device IOMMU domain lifecycle for VT-d and SMMUv3 before allowing DMA/MSI. MMIO-only drivers remain a separate policy case.

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

**Required closure:** range-aware API with explicit host bridge bounds; bridge subordinate-range enforcement; invalid/stale handle behavior; host tests for zero/nonzero starts, nested bridges, malformed ranges and cycles; x86-64/ARM64 builds and QEMU tests on exact final SHA.

## F-004 — Boot handoff contract is not unified in current main

**Severity:** Architecture gap  
**Confidence:** Medium — source/tree inventory; detailed boot-path review remains open  
**Master-plan mapping:** A, B, L, M, N; streams 1, 10, 11, 12.

The x86-64 entry path receives Multiboot2 `magic` and `addr` in `kernel_main()`; ARM64 platform bring-up receives the FDT directly in `arch/aarch64/main.c`. No common `BootInfo` contract/module is present in the current main tree. This does not prove that the current boot paths fail; it means the cross-firmware/platform handoff contract is not yet unified and versioned in main.

Sources:
- [kernel.c at baseline SHA, kernel_main entry](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/kernel.c#L87-L105)
- [ARM64 main entry](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c)

**Required closure:** define versioned BootInfo/platform handoff, memory map ownership, modules/initrd, framebuffer, command line, firmware tables, DMA/IOMMU topology and lifetime; add BIOS/UEFI/ARM64 producer-consumer tests.

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

**Required closure:** explicitly define stack-exhaustion policy (fail thread creation or use a separately guarded fallback); zero stack contents before publishing a new thread; add tests for allocation failure, fallback classification, canary initialization and zeroing; ensure all supported architectures enforce the same policy. Do not silently treat heap fallback as equivalent to guarded stacks.


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
- Exact-head CI is pending; not yet accepted.

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

## Verification ledger

| Finding | Source reviewed | Runtime reproduction / branch evidence | Targeted regression evidence | Status |
|---|---|---|---|---|
| F-001 | x86-64 and ARM64 stack allocator | Combined PR #154 exact head `4dd1454093af90f7d569df03b4425650da8da8bc`; CI run `43590094470` | 43/43 checks PASS, including apps-drvman, host, ARM64 and QEMU matrix | FIX VERIFIED ON DRAFT BRANCH; main unchanged, merge/review pending |
| F-002 | KDRV, syscall capability gate, ARM64 DMA/MSI paths | Not run in this session | PR #136/137 tests are branch-only | CONFIRMED SOURCE SECURITY GAP |
| F-003 | PCI ECAM backend and scan root | Not run in this session | PR #139 tests are branch-only | CONFIRMED SOURCE DEFECT |
| F-004 | x86-64 and ARM64 entry paths/tree | Not run in this session | No unified BootInfo test in main | ARCHITECTURE GAP |
| F-005 | PCI/KDRV structure and PR #141 dependency | Not run in this session | No generic registry test in main | ARCHITECTURE GAP |
| F-006 | ARCHITECTURE.md vs current sources/build/CI | Source/document comparison only | No docs consistency guard in main | CONFIRMED DOCUMENTATION DRIFT |
| F-007 | build_clang.sh release filtering | Static build-script review | No full-image manifest comparison in main | CONFIRMED PROFILE COMPLETENESS RISK |
| F-008 | sched.c stack fallback and initialization | Source review | No stack-zeroing/fallback policy regression in main | CONFIRMED HARDENING GAP |
| F-010 | x86-64 TLB shootdown timeout and late IPI handling | Draft PR #155 exact head `f2df3472d78241e6287d5cc7601b5d3a632b3e52`; CI run `43590730265` | 43/43 checks PASS, including host, ARM64 and QEMU SMP matrix | FIX VERIFIED ON DRAFT BRANCH; main unchanged, review/merge pending |
| F-011 | MMIO page-range overflow during page align-up | Source arithmetic review | Draft PR #156 exact head `cbad3a2198747867e72a02e811efa38133d1dd08`; CI run `43591102543` has one unrelated `drv-edu` log assertion failure; host/ARM64 still running | FIX IMPLEMENTED; CI ACCEPTANCE BLOCKED BY F-012, RERUN REQUIRED |
| F-012 | Driver-start log line split at 192-byte buffer boundary | QEMU serial artifact from run `43591102543` confirms driver success but interleaved log | Draft PR #157: atomic complete-line output; exact-head CI pending | FIX IMPLEMENTED ON DRAFT BRANCH; NOT YET ACCEPTED |
| F-009 | PMM contiguous block release | Source review and caller inventory | PR #153 exact-head CI 43/43 PASS; combined PR #154 integration pending | FIX VERIFIED ON DRAFT BRANCH; combined integration pending |

No code was changed on `main` by this audit. All implementation changes remain in separate unmerged Draft PRs. No test was executed locally. Exact-head CI evidence from other PRs is branch-only and is not transferred to `main`.
