# Stage 0 — crosswalk требований A–N к новой архитектуре

**Дата:** 2026-11-15  
**Статус:** рабочая карта Stage 0; не является отчётом о готовности подсистем.  
**База исходного аудита:** `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`.

Цель — сохранить весь объём мастер-плана и назначить каждому направлению место в новой архитектуре Stage 0–13. Если требование затрагивает несколько этапов, оно не считается закрытым после выполнения только одного из них.

| Направление | Архитектурная зона | Основные Stage | Что обязательно сохранить/проверить |
|---|---|---:|---|
| A. Архитектура и базовая платформа | Hybrid capability microkernel, BootInfo, HAL, object model, ABI/IPC | 0–5 | Контракты, границы привилегий, versioning, cross-ISA invariants, dependency graph |
| B. Ядро KIRA | Privileged kernel, scheduler, memory, syscall | 2–4 | Lifecycle, SMP, guarded stacks, TLB, fault handling, no hidden fallback |
| C. KIRA Virtual Runtime | WASM/WASI capability runtime, native ABI | 9–10 | Existing WASM subset, verifier, JIT W^X, quotas, cancellation, host imports, ARM64 parity |
| D. KIRA Mind | Isolated policy/analytics services | 11 | No implicit kernel authority, provenance, bounded telemetry, deterministic policy and rollback |
| E. KCC / IR / toolchain | KCC, typed KIRA IR/SSA, optional MLIR bridge | 9 | Compiler correctness, type/ownership model, backend parity, deterministic bootstrap |
| F. Memory safety and execution security | Capabilities, W^X, IOMMU, signatures, sandbox | 3, 5–7, 10–13 | Pointer/handle validation, DMA isolation, trust chain, parser fuzzing, revocation |
| G. Drivers and Driver Manager | Device model, isolated drivers, DMA/IOMMU, source adaptation | 6 | Resource lifecycle, IRQ/MMIO/DMA capabilities, provenance/license, sandbox/build/sign/rollback |
| H. KiraFS and storage | Block service, VFS/filesystems, CAS/Merkle/CoW | 7 | Flush/barriers, crash recovery, integrity, atomic root updates, corruption fuzzing |
| I. Network and communications | NIC drivers, network stack, TLS and service IPC | 6, 8 | Device isolation, protocol conformance, socket lifetime, backpressure, fuzzing |
| J. Graphics and Spatial Minimal | Display/compositor/input/audio services and shell | 6, 12 | Service isolation, existing approved visual direction, GUI regression and interaction evidence |
| K. System services and application platform | Service manager, typed IPC, package/runtime APIs | 5, 8, 10, 12 | Lifecycle, dependency management, signed packages, SDK/API stability |
| L. Boot/install/update/recovery | Boot chain, installer, A/B or atomic update, recovery | 1, 7, 13 | BootInfo, image integrity, no-loss manifest, rollback, exact release artifacts |
| M. Architectures and real hardware | x86-64/AArch64 HAL and device matrix | 1–6, 13 | Separate evidence per ISA, QEMU/Limbo vs physical hardware distinction |
| N. Tooling and quality | CI, reproducible build, tests, fuzzing, benchmarks, release engineering | 0–13 | Exact SHA evidence, layered tests, supply-chain audit, artifacts and independent verification |

## 1. Cross-cutting architecture contracts

These contracts are shared and must not be implemented as incompatible per-subsystem variants:

1. **Address and memory domain:** PhysAddr, VirtAddr and DmaAddr are distinct; DMA mappings are created/revoked through IOMMU-aware APIs. Raw integers/pointers cannot silently cross domains.
2. **Capability/object lifetime:** typed object handles carry rights and generation/lifetime semantics; transfer, duplication, revocation and teardown are explicit.
3. **ABI discipline:** Native ABI, syscall ABI, IPC schema, driver ABI and WASM host ABI are separately versioned and tested. Struct layout, alignment, endian, errors and compatibility policy are specified.
4. **IPC discipline:** all untrusted messages have bounded length/depth/count; schemas are versioned; queue capacity, ownership, cancellation, timeout and backpressure are explicit.
5. **Driver isolation:** no network-fetched binary is executed as a driver; driver build and test happen in a sandbox; MMIO/IRQ/DMA are least-privilege resources.
6. **Mind authority:** AI/policy suggestions are not privileged commands; each state-changing action passes through explicit policy and audit.
7. **Storage integrity:** signed metadata uses canonical encodings; crash consistency and rollback are tested, not inferred from hashes alone.
8. **Build provenance:** toolchain versions, dependencies, generated files, source inputs and artifact hashes are recorded; CI evidence binds to exact commit SHA.

## 2. Language-to-component assignment

| Language/stack | Primary owner | Non-goals / restrictions |
|---|---|---|
| C + KCC | Current kernel, boot-adjacent code, existing drivers and performance-critical core | No unreviewed ABI break or wholesale rewrite |
| Assembly | Boot, exception entry, context switching, narrowly scoped ISA primitives | No business logic; every entry has documented ABI |
| Rust `no_std` | Candidate isolated services and user-mode drivers | Not integrated until toolchain, allocator/panic, FFI, IPC and both ISA builds are verified |
| WASM/WASI | Portable apps and capability-limited extensions | Not a replacement for kernel code; compatibility profile must be explicit |
| Python | Host-side generators, analysis, fuzz orchestration, benchmark/report tools | Not a kernel/runtime dependency |
| Bash | Thin build/CI/Termux orchestration | Avoid untestable complex logic embedded in scripts |
| SPARK Ada / Zig | Research only | No production dependency without separate ADR and proven advantage |

## 3. Format-to-workload assignment

| Format/technology | Workload | Required decision evidence |
|---|---|---|
| FlatBuffers vs Cap’n Proto | Typed IPC candidates | Decode/verify cost, code size, memory, compatibility, malformed-input fuzzing |
| CBOR | Config, manifests, compact metadata | Canonical form, strict parser limits, signature stability |
| Arrow + Zstd + event log | KIRA Mind telemetry/analytics | Batch size, throughput, memory ceiling, bounded retention and recovery |
| Parquet | Long-term analytics/export | Storage ratio, query workload, implementation cost |
| CAS + Merkle DAG + CoW + signatures | Immutable objects, package/update integrity | Atomic commit, GC, crash recovery, signature canonicalization |
| LSM-tree vs B+Tree | Metadata/index databases | Read/write profile, write amplification, memory, recovery |
| ELF and WASM binary | Existing native and portable executable paths | Preserve compatibility; introduce new container only through versioned migration ADR |

## 4. Stage 0 exit checks added by this crosswalk

- [ ] Every A–N requirement has one or more Stage assignments and is represented in the component matrix.
- [ ] Every current component maps to an owner, ABI/contract, language, build target, architecture and test suite.
- [ ] Existing branch-only implementations and PR stacks are classified as integrated, candidate, superseded or blocked.
- [ ] No requirement from the legacy stage plan disappears; conflicts are resolved in ADRs.
- [ ] Each proposed language/format/technology is classified as integrated, experimental, candidate or rejected, with evidence.
- [ ] At least one representative benchmark workload is defined for every competing technology pair before selection.
- [ ] Stage 0 has reproducible baseline evidence and a prioritized remediation/migration backlog.

## 5. Current limitations of this map

This is a requirement allocation document, not proof that all components have been audited or tested. The source audit and A–N reconciliation remain open. No new language or serialization library is claimed to be integrated. Physical hardware support remains unverified unless a specific device has independent evidence.
