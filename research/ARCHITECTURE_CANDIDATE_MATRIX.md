# KIRA architecture candidate matrix — experiment baseline

Status: research matrix only. No candidate is selected or approved.

## Candidate families grounded in the architecture decision program

| ID | Candidate | What changes | Required comparison |
|---|---|---|---|
| A | Current modular C/ASM kernel | Existing kernel and service boundaries | Baseline for every workload |
| B | Larger hybrid kernel | Move selected performance-critical services into kernel; keep optional/fault-prone services isolated | Dispatch/IPC latency vs trusted code size, crash blast radius and restartability |
| C | Small microkernel | Isolated drivers, filesystem and network services; message IPC | Service-failure containment, IPC cost, RAM, tail latency, recovery |
| D | Capability-oriented design | Explicit resource handles, least authority, ownership checks | Unauthorized access rejection, revocation, overhead, leak/deadlock behavior |

## Language and compiler candidates

| Area | Candidates | Mandatory measurements |
|---|---|---|
| Low-level boot/hardware | C + assembly baseline | boot correctness, binary size, portability |
| Memory-sensitive services | C vs Rust; Zig as optional measured candidate | fuzz defects, unsafe boundary, code size, RAM, build time, x86-64/AArch64 |
| Compiler IR | current KCC IR vs typed SSA vs LLVM IR reference | compile time, incremental time, code size, runtime, ABI/output equivalence |
| Optimization | current backend vs optimized backend; LTO/PGO only where supported | clean/incremental build, p50/p95/p99 runtime, size, cache correctness |
| Portable untrusted extensions | WASM/WASI-like sandbox vs native process | isolation, startup, throughput, memory, denied syscalls/capabilities |

## Data, IPC, storage and intelligence candidates

| Area | Candidates | Mandatory measurements |
|---|---|---|
| IPC | copy-based typed messages; shared-memory ring; capability-checked shared buffer; versioned schema (FlatBuffers/Cap'n Proto/custom) | separate-process correctness, MPSC/MPMC contention, copies, p50/p95/p99, throughput, RAM, unauthorized access |
| Hot data | fixed binary records vs JSON/CBOR/typed schema | encode/decode, random access, malformed input, version compatibility |
| Cold telemetry | row records vs columnar batches; zlib vs LZ4/Zstd | bytes, encode/decode, selective reads, CPU, peak RAM |
| Filesystem/update | KiraFS baseline vs journal vs CoW/CAS+Merkle | write/flush crash points, old-or-new recovery, integrity, rollback, write amplification |
| Compiler cache | no cache vs canonical IR cache with compiler/target/flags/dependency digests | cold/warm build, invalidation, reproducibility, false hits |
| System intelligence | deterministic rules vs bounded Mind controller with rollback vs isolated generative AI service behind capability broker | decision quality, resource budget, adversarial input, policy violations, rollback; AI cannot approve itself |

## Cross-domain combinations (test after individual candidates)

| Combination | Components | Question |
|---|---|---|
| X1 | capability handles + isolated services + typed IPC | Does stronger isolation justify IPC overhead? |
| X2 | shared-memory ring + capability validation + bounded queues | Does real cross-process IPC beat copy messages under contention without leaking authority? |
| X3 | CoW/CAS + Merkle integrity + signed package + A/B rollback | Can updates survive interruption and reject tampering/downgrade? |
| X4 | canonical typed SSA/IR + dependency-aware cache + x86-64/AArch64 lowering | Does the cache accelerate real KCC builds without stale artifacts or output changes? |
| X5 | WASM sandbox + capability broker + resource quotas | Can untrusted applications run portably without direct device authority? |
| X6 | Mind bounded controller + telemetry pipeline + independent policy gate | Does adaptation improve responsiveness without runaway resource use or self-authorized changes? |
| X7 | isolated driver service + source/license intake + ABI mapper + compiler feedback + QEMU device tests | Can legal source drivers be ported reproducibly, and can failed ports be contained? |
| X8 | full pipeline: AI proposal → compiler → static/fuzz tests → sandbox/QEMU → signed promotion/rollback | Is there a complete, independently gated software factory? Generative AI remains NOT TESTED until a real AI integration exists. |

## Test protocol

- Freeze one commit and one workload per comparison. Run candidates against identical data and parameters.
- Correctness/security are hard gates; speed cannot compensate for corruption or privilege escalation.
- At least 5 warmups and 20 measured repetitions where practical; report median, p95/p99, throughput, CPU, peak RAM, bytes written and binary size.
- Run x86-64 and AArch64 where implementation support exists. Keep QEMU and physical-hardware results separate.
- Keep raw JSON/CSV, exact commit SHA, toolchain and runner metadata.
- Test each candidate alone before combinations; then test pairwise combinations and combinations-of-combinations. Record negative interactions.
- Reject synthetic Python/host-model results as proof of kernel performance. Label every evidence tier: static/source, host prototype, QEMU kernel, physical hardware.

## Current evidence snapshot (2026-11-09)

- Existing CI contains 50 host-test groups, 39 x86-64 QEMU scenarios and 16 ARM64 QEMU scenarios as counted by the coverage audit. These are coverage counts, not all completed results for the current run.
- Round-1 host prototypes: CAS+Merkle+CoW improved one synthetic snapshot workload; columnar+zlib reduced size; capability/ring IPC was slower than copy; canonical IR cache only proved key invalidation and synthetic warm lookup.
- Missing/weak evidence: true process-shared IPC; complete DMA/IOMMU threat tests; complete update interruption/rollback; real KCC differential optimization benchmarks; real AI-to-software end-to-end pipeline; physical devices.
- Current architecture must remain undecided until the matrix is tested. Do not turn these preliminary findings into architecture claims.
