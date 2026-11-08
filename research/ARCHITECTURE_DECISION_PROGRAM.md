# KIRA Architecture Decision Program

Status: research plan. No architecture replacement is approved by this document.

## Product objective

KIRA is evaluated as an integrated desktop operating system with six goals: user convenience, responsiveness/performance, security, built-in AI, adaptive automation, and maintainability/portability. Kernel size and implementation language are variables, not goals. A larger kernel is acceptable only when measured benefits justify its additional trusted code and failure impact.

## Current baseline facts (from main branch at research start)

- README describes a C/assembly microkernel, x86-64 first with ARM64 work, custom scheduler, KiraFS/FAT, USB and storage drivers, graphical shell, and the "Mind" subsystem.
- Architecture notes describe PMM/VMM, per-process PML4 and Ring 3, syscall validation, scheduler, framebuffer/window manager, and device interfaces.
- KiraFS v2 uses 4 KiB blocks, 128-byte inodes, direct/indirect addressing, CRC-protected metadata, and ordered data-before-inode updates intended to preserve old-or-new file state after interruption.
- The current CI already runs host tests, x86-64 build, QEMU scenario matrix, and ARM64 build/scenarios.
- A host-side Python data-format experiment has been added in this research branch. It is not a kernel benchmark and must not be used to select the final on-disk or IPC format by itself.

These are observations, not a full code audit. Verify each against current source before changing it.

## Architecture candidates to compare

A. Current C/assembly kernel with modular service boundaries.
B. Larger hybrid kernel: performance-critical services in kernel, fault-prone or optional services out of kernel.
C. Small microkernel with isolated drivers/filesystem/network services and message IPC.
D. Capability-oriented kernel/service design with explicit resource handles and strict memory ownership.

Do not decide from ideology. Compare the same workloads and features. Include a fault-injection test measuring whether a service/driver failure takes down the kernel, can be restarted, and preserves data.

## Language candidates

- Keep C + assembly as the baseline for early boot, architecture code, and low-level hardware bring-up.
- Compare Rust for memory-safe services, drivers, parsers, and selected kernel components where toolchain and target support permit.
- Consider Zig only as an additional measured candidate for low-level components/build integration, not as an assumed improvement.
- Consider a KIRA-specific DSL only for a narrow domain (hardware descriptions, syscall/interface schemas, or verified policy rules) if it measurably reduces defects or maintenance cost. Do not create a general-purpose language before a demonstrated need.

Measure binary size, boot time, runtime latency/throughput, memory footprint, build time, defect rate in fuzz/property tests, and cross-target portability.

## Data and storage candidates

- Human-editable configuration: TOML or strict JSON, parsed outside privileged hot paths.
- IPC: fixed-layout typed messages versus FlatBuffers/Cap'n Proto or a custom versioned schema.
- High-frequency queues: bounded ring buffers and shared memory with explicit ownership/capability checks.
- Telemetry/AI history: append-only event segments, columnar batches for analysis, compression for cold segments.
- Long-term storage: current KiraFS versus an experimental content-addressed immutable object layer with hashes/Merkle manifests and copy-on-write metadata.
- Compression: no compression for hot paths; compare LZ4 and Zstd for logs, package payloads, and cold data.
- Compiler artifacts: canonical IR serialization and cache keys including compiler version, target, flags, and dependency digests.

## Mandatory benchmark protocol

1. Preserve a reproducible baseline on the same runner and target.
2. Compare one variable at a time before testing combinations.
3. Use identical input datasets and workloads; publish source, parameters, raw CSV/JSON, and environment metadata.
4. Run correctness tests before timing. Reject any candidate with data mismatch, corruption, race, or failed safety invariant.
5. Report median and tail latency (p95/p99 where meaningful), throughput, CPU time, peak memory, bytes written, binary size, and build time.
6. Run multiple independent repetitions; include warm and cold cache cases.
7. Add randomized/fuzzed and crash/power-loss tests for parsers, storage, and update paths.
8. Test x86-64 in QEMU and on the existing Limbo configuration where possible; test ARM64 in QEMU. Clearly distinguish emulation from physical hardware.
9. Keep every candidate behind a feature branch/configuration until evidence and review support adoption.
10. Never use synthetic benchmark wins as proof of whole-system superiority.

## Initial experiment sequence

1. Finish and preserve current GitHub Actions baseline: host tests, x86-64 build, ARM64 build, and QEMU matrix.
2. Extend data-format tests in C: fixed binary, versioned typed messages, FlatBuffers/Cap'n Proto if practical, CBOR, JSON, and compression variants. Test malformed input and schema-version compatibility.
3. Compare IPC paths: copy-based message queue, shared-memory ring buffer, and capability-checked shared buffer. Measure copies, latency, throughput, CPU, and memory under contention.
4. Compare storage/update designs using a block-device test harness: current KiraFS write path, journal/CoW prototype, content-addressed immutable objects. Inject failures at every write/flush boundary and remount.
5. Compare compiler build times: clean build, incremental source rebuild, normalized-IR cache hit/miss, and PGO where supported. Verify output equivalence and cache invalidation.
6. Compare language implementations on one bounded component with identical behavior and tests (C baseline, Rust candidate, optionally Zig).
7. Only then compare integrated combinations and user-visible workloads: boot-to-shell, input-to-frame latency, application launch, file operations, background load responsiveness, recovery after service failure, and AI task resource budgets.

## Decision gates

- Security/correctness are hard gates, not weighted away by speed.
- Adopt only when gains exceed measurement noise and overhead across relevant workloads.
- Reject a candidate if it regresses critical user experience, portability, recoverability, or maintainability without a clearly accepted trade-off.
- A larger kernel requires evidence that lower IPC/dispatch overhead or simpler resource coordination outweighs larger trusted computing base and fault blast radius.
- A new language requires measurable benefits over existing choices on real KIRA components.
- Keep an Architecture Decision Record for every accepted/rejected candidate, including benchmark links and known limitations.

## Current evidence boundary

The first Python format experiment compared six encodings of deterministic synthetic records on GitHub Actions. It validates round-trip correctness and gives host-side Python serialization measurements only. It does not prove kernel, filesystem, C, Rust, IPC, or hardware performance. Do not use those numbers as final architecture evidence.
