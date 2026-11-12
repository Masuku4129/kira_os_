# ADR-0001: KIRA-native application VM

- Status: **Proposed — owner decision required**
- Date: 2026-11-12
- Scope: KIRA Virtual Runtime (KVR), KIRA VM (C1), WASM compatibility runtime

## Context

The project already has a portable WebAssembly runtime with an established module
format, an x86-64 JIT path using RW-NX to R-X sealing, and an ARM64 interpreter.
The master plan therefore names WebAssembly as the baseline portable KVR format
and prohibits adding a second private bytecode unless a separate architecture
decision demonstrates measurable value and defines migration.

KIRA VM is proposed as a smaller KIRA-native application runtime with typed
functions, bounded per-instance memory and call stacks, a named import registry,
and capabilities checked at the host boundary. It is not a hypervisor and does
not replace WASM or native processes.

Two incompatible KIRA VM container designs appeared during development:
- **KVM1 draft:** 64-byte sectioned header, typed function descriptors, typed
  register declarations and named imports, specified in `docs/KIRA_VM_SPEC.md`.
- **KVP0 prototype:** 32-byte header and a fixed 16-register interpreter model.
  It exists only to test interpreter mechanics and cross-architecture build
  integration. It is not a public ABI and must never be labelled KVM1.

## Alternatives

### A. Extend WebAssembly only

Advantages:
- avoids a second bytecode, verifier, toolchain and package format;
- benefits from existing module ecosystem and current runtime/JIT work;
- lowest long-term compatibility and maintenance burden.

Costs:
- KIRA-specific typed imports and capability policy must be expressed through
  WASM imports and the existing host ABI;
- any KIRA-native semantic restrictions need additional validation around WASM.

### B. Add a KIRA-native typed VM alongside WASM

Advantages:
- can define a deliberately small KIRA ABI and named imports;
- can keep verification, instruction budget and memory semantics tailored to
  KIRA's process and resource model;
- one module can target x86-64 and ARM64 without CPU-specific code.

Costs:
- duplicates loader, verifier, interpreter, SDK, package lifecycle, fuzzing and
  cross-architecture maintenance;
- creates a second application ecosystem and ABI migration burden;
- a small interpreter is not automatically safer than the existing WASM runtime.

### C. Ship the current 32-byte register-file prototype as KVM1

Rejected. It conflicts with the typed 64-byte KVM1 draft and lacks function
signatures, typed register declarations and named imports. Reusing the KVM1
magic for it would make the file format ambiguous and create an incompatible
public ABI before the design is settled.

## Proposed direction

1. Keep WebAssembly as the production portable KVR baseline.
2. Keep the 32-byte implementation under the distinct experimental magic
   **KVP0**, version 0. Do not install it as a general application format or
   include it in release images.
3. Reserve **KVM1** for the 64-byte typed sectioned format in
   `docs/KIRA_VM_SPEC.md`. The draft is not yet frozen or implemented.
4. Do not merge KIRA VM into the release baseline until the project owner
   accepts this ADR and the evidence below is complete.
5. If the evidence does not show a measurable advantage over extending WASM,
   close the KIRA VM experiment and invest in the existing WASM runtime.

## Evidence required before acceptance

- A side-by-side benchmark against the current WASM interpreter and x86-64 JIT:
  startup time, code size, peak memory, steady-state throughput and worst-case
  verifier cost on representative KIRA applications.
- A threat model and independently reviewed typed verifier, parser and import
  registry, with ASan/UBSan fuzzing and negative fixtures for every section,
  type, branch, call, import, capability and memory boundary.
- A complete instance lifecycle: quotas, cancellation, pause/resume, watchdog,
  teardown and safe failure handling.
- Identical conformance vectors on x86-64 and ARM64; no architecture-specific
  semantic drift.
- Signed package integration, ABI version negotiation and atomic update/rollback
  policy before general distribution.
- A working SDK/compiler path; hand-authored bytecode alone is not an application
  platform.
- An explicit owner decision accepting or rejecting the second-runtime cost.

## Consequences while proposed

- KVP0 remains a prototype and is not a stable app ABI.
- KVM1 remains the typed draft specification, not a claimed implementation.
- WASM remains the production portable runtime.
- Full hardware virtualization (VT-x/AMD-V/ARM EL2) remains a separate subsystem.
- No KIRA VM capability may grant kernel pointers, physical memory, MMIO, DMA or
  raw syscalls. Host imports must remain narrow, named and capability checked.
