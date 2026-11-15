# ADR-0001: Primary application runtime for KIRA OS

- Status: Accepted for the current release baseline
- Date: 2026-11-12
- Scope: KIRA Virtual Runtime (KVR), x86-64 and ARM64

## Context

KIRA OS already contains a userland WebAssembly runtime with a portable module
format, a restricted host API, an interpreter on both architectures, and an
x86-64 JIT with W^X sealing. The master development plan names KIRA VM as a
future subsystem but explicitly requires an architecture decision before
introducing a second bytecode ISA.

A separate KVM1 prototype has been developed on draft PR #134. It currently
uses a 32-byte container header, fixed-width 8-byte instructions, a portable C
interpreter, a bounded Ring 3/EL0 runner prototype, and an experimental
assembler. Its ABI conflicts with a separate 64-byte sectioned proposal. It
does not yet have a stable compiler, package-signature integration, lifecycle
controls, complete cross-architecture execution evidence, or a mature app
ecosystem.

## Decision

1. WebAssembly remains the primary supported application VM and portable
   bytecode format for the current release baseline.
2. KVM1 remains an isolated research prototype on draft PR #134. Its current
   ABI is not frozen and its runner/build/file-manager integration must not be
   merged into a release branch until a separate review accepts a measurable
   advantage and resolves the ABI conflict.
3. KVR work in the release baseline prioritizes the existing WASM runtime:
   parser/verifier audit, malformed-module fuzzing, import/capability policy,
   memory/resource quotas, interruption/cancellation, host-call limits,
   JIT W^X correctness, and x86-64/ARM64 conformance.
4. A future KIRA-specific VM may be reconsidered only with comparative
   measurements against WASM for startup cost, memory footprint, verifier
   complexity, deterministic execution, sandbox strength, host ABI, and
   compiler/toolchain cost. The proposal must include a migration plan and a
   single versioned ABI before implementation is promoted.

## Consequences

- Do not describe KVM1 as a supported production runtime.
- Do not merge PR #134's runner integration into release images while this ADR
  is in force.
- Keep KVM1 host tests and assembler as experimental research artifacts, clearly
  marked as such.
- Existing WASM modules remain the compatibility and portability baseline;
  x86-64 JIT is optional and ARM64 remains interpreter-only.
