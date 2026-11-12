# KIRA WebAssembly runtime contract

Status: production-path hardening in draft PR #131. The main release baseline
continues to use this runtime as the primary KVR, subject to successful CI and
release integration.

## Execution model

- `/bin/wasm` is a userland process. The kernel does not parse or execute WASM.
- The same module format is accepted on x86-64 and ARM64.
- x86-64 may use the W^X-sealed JIT; ARM64 uses the interpreter.
- Supported instructions and sections are the WebAssembly 1.0 subset documented
  in `userland/wasm.c`. Unsupported proposals (threads/atomics, SIMD,
  reference types, multi-memory, component model) are rejected.
- Module container size is capped at 8 MiB; linear memory is capped at 2 MiB.
  The loader validates section ordering, lengths, counts, indices, code bodies,
  imports, exports, data and element ranges before execution.

## Runtime quotas

- Default execution budget: 50,000,000 WASM instructions.
- Interpreter decrements fuel for each executed instruction.
- x86-64 JIT inserts periodic checkpoints every 256 source instructions and at
  function/loop boundaries. Its fuel charging is conservative and may stop a
  tight short loop earlier than the interpreter; it must never run without a
  finite budget.
- Host calls: at most 10,000 per invocation.
- `write(ptr,len)`: at most 4 KiB per call and 64 KiB total output.
- `sleep(ticks)`: at most 60,000 ticks per call and 600,000 ticks total.
- `gfx_create(w,h)`: at most one successful framebuffer per process; total
  framebuffer bytes cannot exceed the 2 MiB guest-memory cap.
- `gfx_present(ptr)`: at most 10,000 frames per invocation.
- Backward branches and host calls cooperate with the scheduler. The process
  remains subject to normal process scheduling and can be terminated by the OS.

## Host imports

Only the fixed `kira`/`env` import allowlist is accepted:
`write`, `sleep`, `ticks`, `exit`, `gfx_create`, `gfx_present`, and
`gfx_event`. All imported calls share the host-call budget. Memory pointers and
lengths are checked against the module's current linear-memory size before use.

This is a bounded allowlist, not yet a signed per-package capability manifest.
A future package manager must bind requested imports to publisher trust and
user/system policy before general third-party distribution.

## Regression coverage

The host suite includes:
- malformed envelope and section-boundary fixtures;
- a valid infinite-loop module that must exhaust fuel under both JIT and
  interpreter modes;
- output, aggregate host-call, and sleep quota trap modules;
- randomized damaged-module execution and native/interpreter output comparison.

CI evidence must be recorded against the exact PR head. A queued workflow or an
older successful run is not evidence for a newer commit.

## Remaining work

1. Complete independent parser/verifier audit and structured fuzzing of every
   section, LEB encoding, type, import, control-flow edge and memory operation.
2. Verify JIT fuel checkpoint coverage for all backward edges, indirect calls,
   recursive calls and generated basic blocks; measure interpreter/JIT budget
   variance.
3. Add explicit process cancellation/timeout controls and cleanup for graphics
   resources.
4. Replace the fixed import allowlist with signed package capability requests
   and a user/system grant policy.
5. Run the same conformance fixtures on x86-64 and ARM64 QEMU; test the final ISO
   in Limbo separately from CI.
