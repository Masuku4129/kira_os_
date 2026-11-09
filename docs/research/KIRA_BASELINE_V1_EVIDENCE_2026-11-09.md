# KIRA Technology Baseline v1.0 — evidence ledger (2026-11-09)

This ledger preserves the original experiment: test individual technologies, then combinations, then combinations-of-combinations. It does not select a final architecture.

## Fixed catalogue

- K1–K3: kernel / architecture
- M1–M4: memory / security
- C1–C4: compiler
- S1–S4: data / formats
- D1–D5: storage
- E1–E4: execution / IPC
- DR1–DR5: driver subsystem
- AI1–AI8: AI / adaptivity
- CC1–CC7: cross-domain combinations

The catalogue IDs and groupings above are the accepted baseline labels. The complete wording of every sub-candidate is not independently present in this repository snapshot; do not invent missing definitions. The approved first priority is:

**Capability Microkernel + Shared Memory + Ring Buffer + Zero-Copy.**

Target architecture remains a hypothesis: **Adaptive Capability Microkernel + Universal AI + Adaptive Compiler + Autonomous Driver Engine**.

## Evidence collected from completed CI

Source run: [42557276182](https://github.com/Masuku4129/kira_os_/actions/runs/42557276182), branch `kira-lab/robust-futex-2026-11-09`, commit `61b1e53e57c4bc30594b43ed91ea1cdf4b315171`. All 43 GitHub Actions jobs succeeded: 39 x86-64 QEMU scenarios plus host, data, build and ARM64 jobs. The ARM64 job contains 16 scenarios; all 16 result.json files report ok=true.

### K — kernel / architecture

- x86-64 QEMU SMP scenario: 8/8 processors online; 8 worker threads completed; result PASS; guest-reported wall time 62 ms.
- ARM64 QEMU: GICv2 and GICv3 boot paths, 2–8 cores, EL1 and EL2 entry paths exercised; 8/8 cores online in the EL2/8-core scenario.
- Evidence level: KIRA kernel in QEMU. Not physical hardware.

### M — memory / security

- Host tests: heap ownership/alignment, 200,000 randomized heap operations, OOM recovery, PMM contiguous allocation, RAM above 1 GiB, and memory mapping checks passed.
- KIRA QEMU isolation: separate address spaces survived repeated sleeps; kernel pointer passed to write() rejected; 15 unauthorized CAP_GFX calls denied; FPU/vector state remained private across concurrent processes.
- Evidence level: host tests and KIRA QEMU. Not a formal proof of capability security.

### C — compiler

- KCC host suite: 449 test files, 0 failures.
- KCC fuzz: 3,000 inputs; 211 accepted and 2,789 rejected.
- ASan/UBSan used by the compiler test suite.
- A direct KCC-vs-Clang executable benchmark was added to the round-2 data job, but its new CI run is still queued. No performance conclusion yet.

### S — data / formats

- Previous combined-tech lab: columnar + zlib was 1.81x smaller and 4.3% faster to encode than its tested baseline on that synthetic workload.
- This is a host-side synthetic benchmark, not a KIRA kernel workload.

### D — storage

- KiraFS host tests: 191 simulated crash cases; corruption fuzz mounted 334 inputs and rejected 66; v1 upgrade and large-file/directory growth passed.
- Persistent store: CRC vector, A/B generations, torn-write handling and corruption fallback passed.
- These tests validate the tested code paths; they do not prove power-loss durability on physical storage hardware.

### E — execution / IPC

- KIRA QEMU isolation scenario: 200 IPC round trips in 12 ms (about 60 microseconds per round trip in that guest scenario); shared-memory marker observed by the server.
- Host kernel-object tests cover ports, shared memory, handles and futex behavior.
- Host real context-switch test: 102,965 switches, three workers, zero FPU mismatches; control without FPU save detected 120 mismatches.
- Important gap: no fair head-to-head KIRA-kernel benchmark yet for copy IPC vs shared-memory ring vs zero-copy.

### DR — drivers

- Host driver intake rejects GPL-marked source, binary-only payloads, missing licenses, HTTP, invalid hashes and untrusted inputs.
- RTL8139 source build was exercised for x86-64 and AArch64 in the host lab.
- QEMU driver-manager scenario checked package rights and driver lifecycle.
- ARM64 driver-manager log shows the EDU driver's DMA request returned rc=-7 and 0 DMA pages were allocated. The scenario passing does not prove working DMA for this device. Treat ARM64 DMA-driver support as unproven.
- ARM64 non-coherent DMA scenario reported a 32 MiB uncached DMA pool and completed a 58,020-byte audio DMA transfer. This is QEMU evidence, not IOMMU/SMMU isolation.

### AI — adaptivity

- Mind host tests cover norms, anomalies, memory pressure, focus, starvation, persistence, device errors, input immunity and policy limits.
- QEMU mind-levers scenario demonstrated trial, measurement, keep and rollback paths. Examples: a 60 ms audio lead was kept after a measured improvement; other changes were rolled back when no improvement was found.
- Mind foresight host model: precision 63%, recall 48% on the recorded synthetic test; a noise-only world had 0% precision/recall and trust was disabled. Do not present the model as a general-purpose AI.
- Generative AI-to-compiler-to-tested-software pipeline remains unproven; no real end-to-end generative AI integration is established by these tests.

### CC — combinations

Prior combined-tech host prototype results (run [42551324373](https://github.com/Masuku4129/kira_os_/actions/runs/42551324373)):

- Capability-checked IPC ring: validation passed, but the tested implementation was 2.54x slower than the copy baseline. Do not select it for speed on this evidence.
- CAS + Merkle + CoW: 18% faster and 40.1x fewer retained snapshot bytes in the synthetic workload.
- Columnar + zlib: 1.81x smaller and 4.3% faster encoding.
- Canonical IR cache: about 2x faster warm lookup in a small synthetic test; real compiler end-to-end gain remains unproven.

## First priority experiment: exact comparison still required

The round-2 branch adds `research/combined_lab/ipc_process_shared.c` and a CI step to compare:

1. pipe copy between two processes;
2. shared-memory ring with explicit copies into/out of slots;
3. shared-memory ring with producer writing and consumer reading the shared slot in place;
4. descriptor-possession probe (FD present vs absent).

The descriptor test is only a Linux host analogue. It is not KIRA's capability enforcement and does not prove microkernel security. The ring tests are separate-process Linux prototypes, not KIRA kernel IPC.

The corresponding CI run is [42558948592](https://github.com/Masuku4129/kira_os_/actions/runs/42558948592). At the time of this ledger it is queued, so there are no results yet.

## Decision rules

- Correctness, isolation and integrity are hard gates; speed cannot compensate for a security or data-integrity failure.
- Compare the same payload, process count, workload, compiler and runner. Report median and tail latency, throughput, CPU and peak memory where available.
- Keep x86-64 and ARM64 separate. Keep host prototypes, QEMU kernel evidence and physical hardware evidence separate.
- Status vocabulary: CONFIRMED (only for the tested scope), CONDITIONAL, INEFFECTIVE, UNPROVEN, REJECTED.
- No architecture winner is selected until isolated candidates and CC combinations are tested with comparable workloads.
