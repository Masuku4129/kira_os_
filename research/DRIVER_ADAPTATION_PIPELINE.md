# KIRA Driver Adaptation Pipeline

Status: research architecture + host-side source-intake prototype. No Linux driver is yet claimed to compile or run on KIRA.

## Non-negotiable product rule

**Importing a source package, compiling it, and allowing it to control hardware are three different operations.** Failure at a later stage must not discard the earlier result.

A driver that cannot currently be adapted is still retained as a source package with provenance, license evidence, source hashes, dependency inventory, compatibility findings, and a machine-readable work queue entry. It is never silently represented as a runnable driver.

## Artifact lifecycle

1. **Acquire** — retrieve a pinned Git commit/release archive; record repository URL, commit, archive hash, retrieval date and upstream authorship notices.
2. **Preserve** — create a read-only `.ksrc` source package. Do not execute build scripts during intake. Reject path traversal, archive symlinks, encrypted entries, duplicate paths, oversized files and decompression bombs.
3. **License review** — preserve SPDX identifiers and all license/NOTICE files. Unknown or conflicting licensing is marked for human review. GPL source is not discarded merely because KIRA's own license policy is undecided. Inclusion, modification and redistribution are separate legal decisions.
4. **Analyze** — parse build files, includes, symbols, architecture-specific assembly, Linux subsystem dependencies, DMA/IRQ/MMIO usage, firmware requirements and device IDs. Produce an explicit dependency graph and compatibility score with evidence.
5. **Adapt** — choose one route:
   - direct build against a stable KIRA Driver ABI;
   - Linux compatibility shim for a supported Linux API subset;
   - source-to-source transformation with compiler/AI suggestions and reviewable patches;
   - partial adaptation with unresolved dependencies retained;
   - source-only archival if adaptation is currently impossible.
6. **Build** — run only in a disposable, resource-limited build sandbox. Pin compiler, headers, target, flags and dependencies. Never let an imported build script run with kernel privileges.
7. **Verify** — static analysis, compiler diagnostics, API-contract tests, fuzzing, sanitizers on host models, ELF/package validation, capability review and reproducible-build comparison.
8. **Hardware simulation** — test against QEMU devices and fault-injection models; physical hardware remains a separate release gate.
9. **Quarantine** — load only in an isolated driver process with least-privilege capabilities, bounded memory, watchdog and device reset path. DMA requires IOMMU isolation where available; without it, risky DMA drivers remain blocked or require a clearly documented restricted mode.
10. **Promote** — only a signed, reviewed, tested artifact becomes installable. Record the exact source hash, patch set, toolchain digest, test results and license decision.

## Compatibility outcomes

- `SOURCE_PRESERVED`: source saved, no API claim.
- `ANALYSIS_PARTIAL`: dependencies found but not fully understood.
- `ADAPTER_REQUIRED`: known missing KIRA interfaces.
- `ADAPTED_BUILD_FAILED`: source transformation exists but compiler failed.
- `BUILT_UNVERIFIED`: object exists, tests incomplete; cannot load.
- `TEST_FAILED`: tests found defects; preserve logs and source.
- `APPROVED_QUARANTINED`: passes defined gates and can run only in isolation.
- `APPROVED`: meets the release policy for the exact hardware/architecture tuple.

Never collapse these into a single boolean "supported".

## Technology combinations to compare

| Area | Candidate combination | Why test it |
|---|---|---|
| Driver ABI | C ABI + versioned fixed-width structs + capability handles | Low friction for existing C drivers; explicit boundary |
| Linux porting | Linux API compatibility shim + KIRA-native bus/device model | Reuse source while keeping KIRA internals independent |
| Compiler | KCC baseline vs Clang/LLVM cross-compile vs Rust/LLVM for new components | Existing KCC is valuable, but Linux kernel build assumptions may exceed it |
| AI adaptation | retrieval over API docs + AST-aware patch proposals + compiler diagnostics loop | Avoid blind text rewriting; every patch is reviewable |
| Isolation | Ring 3 driver process + typed IPC + shared-memory rings + IOMMU-gated DMA | Limit driver fault impact without forcing every packet through copies |
| Verification | host models + fuzzing + sanitizers + QEMU device matrix + hardware gate | Catch parser, state-machine, memory and device failures |

No candidate wins until the same representative drivers and workloads have been tested.

## First representative corpus

Select a small, legally reviewed corpus spanning different subsystems:
- simple PCI network driver (RTL8139 existing KIRA port as baseline);
- Linux PCI network driver with modern DMA API;
- USB HID class driver;
- storage driver with asynchronous queues;
- a driver requiring firmware loading;
- one deliberately incompatible or architecture-specific driver.

For every case record: import success, license classification, dependency graph coverage, percentage of API calls mapped, compile outcome, test outcome, runtime stability, throughput/latency and memory use.

## Immediate gaps in current KIRA

- Existing `.kdrv` format describes an executable ELF and user confirmation; it is not a source archive format.
- Existing `drvget` recipe logic rejects GPL-family identifiers and tells users Linux `.ko` modules are not accepted. This conflicts with the new source-ingestion objective.
- The current source hash and package record do not provide a complete source provenance/patch/toolchain manifest.
- SHA-256 detects changes but is not an authenticity signature. Release promotion needs a signed manifest and trusted key policy.
- Ring 3 does not by itself isolate DMA. IOMMU support and device reset/ownership rules need separate proof.
- KCC must be tested against actual Linux driver language/build requirements; it must not be assumed to be a drop-in Linux kernel compiler.

## Decision rule

Keep the current working driver path intact. Prototype source intake separately. Add compatibility APIs incrementally based on real driver dependency graphs. Do not replace the current driver subsystem until the new pipeline demonstrates more devices supported, no weakened security gates, and acceptable performance.
