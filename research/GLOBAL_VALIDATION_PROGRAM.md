# KIRA Global Validation Program

## Purpose
Validate the whole product, not just whether the kernel boots: boot chain, kernel, memory, scheduling, IPC, storage, drivers, networking, graphics, audio, applications, compiler, built-in intelligence, AI-assisted development, adaptation pipeline, security, updates, performance and developer experience.

The existing CI already provides broad evidence: host tests, x86-64 build, ARM64 build/QEMU and a large QEMU scenario matrix. This plan separates what is already tested from what remains unproven. A green subset must never be described as complete OS readiness.

## Release gates
A release candidate requires all mandatory correctness and security gates to pass; x86-64 and ARM64 builds; all required QEMU scenarios; host sanitizers/fuzzers and crash recovery; no unresolved critical/high security issue; measured performance against a fixed baseline; verified provenance, licensing, signatures and rollback. Physical-hardware limitations must be stated. A failed security gate cannot be offset by good performance elsewhere.

## Global coverage map

| Area | Existing repository evidence | Additional tests required |
|---|---|---|
| Boot/architecture | x86 QEMU boot matrix; ARM64 EL1/EL2, GIC, 1-8 CPU scenarios; boot map tests | repeated cold/warm boot, malformed boot info, firmware/UEFI matrix, physical hardware |
| Memory/protection | heap ASan/UBSan, PMM, page-map tests, isolation and crash-isolation scenarios | fault injection for mapping/unmapping, use-after-free, W^X, guard pages, DMA/IOMMU |
| Scheduler/concurrency | policy, FPU context switch, ticket lock/TSan, scaling to 256 CPUs, SMP scenarios | long contention soak, priority inversion, randomized interleavings, p50/p95/p99 latency |
| IPC/kernel objects | handles, ports, shared memory, futex and isolation tests | copy vs shared-memory/ring-buffer benchmark; malformed-message fuzzing; leak/deadlock tests |
| Storage/filesystems | KiraFS crash-at-write tests, corruption fuzz, persistent-store torn-write tests, FAT/VFS tests | power-loss at every flush boundary on QEMU block devices; recovery invariants; throughput/latency |
| Drivers/hardware | PCI/virtio, USB UHCI/xHCI, RTL8139 model, ACPI/IOAPIC, QEMU matrix | broader device corpus, adversarial DMA/IOMMU, real hardware |
| Networking | e1000/virtio, IPv6, UDP fragmentation and host-service QEMU scenarios | protocol fuzzing, loss/reordering, TLS trust and certificate-failure matrix |
| Graphics/input/UX | graphics/UI/window-manager tests; x86/ARM GUI screenshots and app scenarios | interaction latency, multiple display modes, accessibility, long-session leak checks |
| Audio | SB16, AC97, HDA scenarios and host tests | underrun/overrun, malformed device responses, repeated restart and latency |
| Applications | file manager/editor/viewer/dock/shell host tests; QEMU edit/view/explorer/WASM | complete user journeys, error recovery, keyboard-only operation, usability checks |
| Compiler/toolchain | KCC positive/negative/constant/GNU/ELF fixtures, native execution, x86/ARM builds | differential testing vs Clang, randomized C corpus, miscompile minimization, optimization equivalence, cache correctness |
| Built-in Mind | model, foresight, levers, persistence, seed and QEMU scenarios | adversarial inputs, model poisoning, explainability, resource limits and rollback |
| Generative AI software creation | architecture proposal only; no end-to-end proof yet | controlled AI-to-patch-to-compiler-to-tests-to-sandbox workflow; independent approval |
| Source adaptation | source archive intake prototype and archive-safety tests | representative legal source corpus, license/provenance classifier evaluation, API mapping and port success |
| Security/trust | KDRV signing/crypto/capabilities/isolation tests | update-chain signatures, key rotation/revocation, downgrade/replay, SBOM/license audit, independent review |
| Updates/recovery | persistent store and package crash-safety tests | interrupted full-OS update, A/B rollback, recovery boot, preservation of user data |
| Performance | data-format benchmark and scenario timing | common cross-architecture harness: p50/p95/p99, throughput, CPU, peak RAM, binary size and boot time |
| Developer experience | KCC fixtures and architecture docs | every SDK example compiles in CI; ABI conformance suite; end-to-end beginner tutorial |

## Technology combinations to compare

A. Current KIRA baseline: current C/assembly kernel, scheduler, memory, storage, IPC, KCC and KDRV path.

B. Safety-first hybrid: small privileged core, isolated services/drivers, versioned C ABI, typed capability IPC, shared-memory rings, Rust only for selected services if footprint and target support pass, Clang/LLVM as reference compiler.

C. Compatibility-first hybrid: KIRA-native core/device model, documented Linux/BSD API compatibility subsets, AST-aware porting, compiler-diagnostics feedback loop, reproducible packages and isolated drivers.

D. Compiler stack: current KCC IR/backend vs typed SSA improvements vs Clang/LLVM; Rust for selected safe services; WASM sandbox for portable untrusted extensions, not direct hardware drivers.

E. Storage/data stack: current KiraFS vs journaling vs copy-on-write prototypes; fixed binary vs columnar vs compressed event storage; integrity trees only where threat model and measured overhead justify them.

F. Intelligence/automation stack: deterministic rules baseline, bounded Mind adaptive controller with rollback, optional generative AI in an isolated service behind a capability broker. AI never approves its own output.

A complete stack wins only if it passes correctness/security gates and improves the target workload without unacceptable RAM, code size, boot time or maintenance cost.

## Standard measurement protocol
For performance comparisons: same commit, compiler, flags, target and workload; at least 5 warm-ups and 20 measured runs where practical; report median/p95/p99, min/max, throughput, CPU time, peak RAM and binary size. Keep raw machine-readable artifacts. Separate QEMU measurements from physical hardware. Timing noise alone should not fail a build; correctness regressions must.

For reliability: deterministic seeds, reproducible failures, fault injection at state/commit boundaries, saved fuzz corpus, soak tests, leak checks, serial/QEMU logs, screenshots and exact commit SHA.

## AI and automation safety
Pipeline: specification → plan → patch → compiler → static analysis → unit/property/fuzz tests → QEMU sandbox → security policy → human/release approval. AI proposes and implements; independent gates decide. Automation receives only explicit capabilities. Kernel changes, DMA, firmware, key management and update promotion require separate policy gates and rollback.

Mind's current adaptive subsystem and a future generative AI are different products and need separate evidence. Passing Mind tests does not prove a generative AI software engineer works.

## How to interpret current CI
Existing tests are substantial but not complete product proof. Host models and QEMU are not physical hardware. ARM64 QEMU success is not physical ARM64 certification. Mind tests do not prove generative AI. Driver package tests do not prove broad Linux/BSD driver compatibility.

## Next research phase
1. Preserve mainline and record a reproducible baseline.
2. Generate a coverage inventory from the actual host test script and QEMU scenario registry.
3. Add missing IPC, memory-protection, storage-flush crash, compiler differential, update rollback and AI-safety labs.
4. Run the full current CI on the research branch.
5. Publish one consolidated PASS / FAIL / NOT TESTED report with raw artifact links.
6. Recommend a technology stack only after those results. Do not merge experimental architecture changes before review.
