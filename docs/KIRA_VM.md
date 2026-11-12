# KIRA VM — portable application runtime (experimental prototype)

Status: experimental executable core with a first Ring 3/EL0 runner built into x86-64 and ARM64 images; package manager and signed-app integration are not complete.

## Purpose and relationship to KVR

KIRA VM is the KIRA-native lightweight application runtime described by section C1 of
`docs/MASTER_DEVELOPMENT_PLAN.md`. It is distinct from:

- the existing WebAssembly runtime, which remains the portable compatibility path for
  existing `/wasm` applications and external WASM modules;
- native x86-64/AArch64 process execution;
- full hardware virtualization (VT-x/AMD-V/ARM virtualization extensions).

KIRA VM is intended for applications built against a versioned KIRA host ABI. It does
not replace WASM and does not promise to run arbitrary foreign-OS binaries.

The initial ISA is deliberately small: 16 integer registers, fixed-width 8-byte
instructions, a private linear memory, bounded calls, explicit capability-gated host
calls, and deterministic instruction fuel. The design prioritizes a compact verifier,
low implementation complexity, predictable bounds checks, and identical semantics on
x86-64 and ARM64. A future JIT must preserve the interpreter as the reference and
fallback implementation.

## KVP0 container format

All integer fields and instruction immediates are little-endian. The header is exactly
32 bytes; code immediately follows it, then the initial data image. The file must end
exactly after the declared data bytes; trailing bytes are rejected.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `KVP0` |
| 4 | 2 | format version, currently 0 (prototype only) |
| 6 | 2 | header size, exactly 32 |
| 8 | 4 | code byte length, nonzero multiple of 8 |
| 12 | 4 | initial data byte length |
| 16 | 4 | linear memory size in 4 KiB pages |
| 20 | 4 | entry instruction index |
| 24 | 8 | required capability bitset |
| 32 | variable | code records, then initial data |

Each 8-byte instruction is:

| Byte | Meaning |
|---:|---|
| 0 | opcode |
| 1 | destination register |
| 2 | source/base register A |
| 3 | source register B or syscall capability-bit index |
| 4–7 | signed 32-bit immediate, little-endian |

The KVP0 decoder validates magic, version, exact header size, code alignment, entry,
memory/data bounds, and exact container length before exposing the module view. The first
implementation caps the complete module at 16 MiB and linear memory at 4096 pages (16 MiB)
to prevent unbounded allocations before the OS quota manager is integrated.
The instance copies verified code into private storage and copies initial data into
zero-initialized private linear memory; mutating the source blob after instance creation
does not change the running program.

## Execution model

- Registers: `r0..r15`, each 64-bit unsigned. `r0` is the process exit value and
  syscall result; `r0..r3` carry syscall arguments.
- Constants: signed 32-bit immediates sign-extend to 64 bits.
- Memory: private linear memory, fixed at module creation in KVP0. Loads/stores are
  little-endian and support 8/16/32/64-bit widths. Effective addresses use register A
  plus a signed immediate; overflow and every out-of-range access trap.
- Branches and calls: absolute instruction indices. Calls use a private 256-entry
  return stack; overflow and RET at depth zero trap.
- Arithmetic: 64-bit modular ADD/SUB/MUL, unsigned division/remainder, bitwise
  operations, and logical shifts. Division by zero and shifts of 64 or more trap.
- Fuel: one unit per executed instruction, including branches and host calls. Exhaustion
  terminates the invocation with `KVM_E_FUEL`.
- Host calls: syscall number is the signed immediate (must be nonnegative); byte 3 is
  the required capability-bit index (0–63). That bit must appear in the module's
  required-capability mask and in the grants supplied by the OS. The host callback
  receives both syscall number and capability index and must independently map the
  pair to an allowed OS service. Unknown or denied calls fail closed.

### Initial opcode set

| Opcode | Operation |
|---|---|
| HALT | Return `r0` |
| CONST, MOV | Load immediate, copy register |
| ADD, SUB, MUL, UDIV, UMOD | Integer arithmetic |
| AND, OR, XOR, SHL, SHR | Integer/bit operations |
| LOAD8/16/32/64 | Load from private linear memory |
| STORE8/16/32/64 | Store to private linear memory |
| JMP, JZ, JNZ | Direct and zero/nonzero branches |
| CALL, RET | Bounded internal calls |
| SYSCALL | Capability-gated host ABI call |

Unused instruction fields must be zero. The verifier rejects unknown opcodes, invalid
register indices, noncanonical unused fields, invalid direct targets, malformed
syscalls, missing capability declarations, and malformed module bounds.

## Implemented and tested in this first core

- KVP0 container decoder with little-endian fields, strict length checks, and hard 16 MiB module / 16 MiB linear-memory ceilings.
- Structural verifier for the complete code stream and all direct control-flow targets.
- Instance-owned immutable-by-copy instruction stream and private initialized memory.
- Interpreter for the KVP0 prototype opcode set, bounded call stack, deterministic fuel, memory
  bounds, arithmetic traps, and capability-tagged host calls.
- Host regression cases for arithmetic, loops, branches, calls, recursion overflow,
  memory load/store, module/data copy isolation, KVP0 decoding, malformed containers,
  invalid opcodes/targets, missing capabilities, division by zero, out-of-bounds
  memory, and fuel exhaustion.

## First OS runner integration

> Experimental PR #134 only. This runner is not part of the release baseline and must not be merged into release images unless ADR-0001 is superseded by an accepted comparative review. The production KVR baseline remains the existing WebAssembly runtime.


The `/bin/kvm` userland runner is built for x86-64 Ring 3 and ARM64 EL0 and
accepts one sandbox-local filename argument or a file grant from the file manager; double-clicking a `.kvm` file launches it through that grant. Arbitrary absolute paths are never accepted. It loads
the KVP0 blob, applies the runner policy before allocation, verifies it, creates
a private instance, runs with a fixed fuel budget, and destroys the instance.

The first runner profile is intentionally narrower than the portable core:
- module container: at most 1 MiB;
- guest linear memory: at most 128 pages (512 KiB);
- fuel: 10,000,000 instructions per invocation;
- host capabilities: console write (maximum 4 KiB per call and 64 KiB total), monotonic ticks,
  and cooperative yield only; all host calls together are capped at 10,000 per invocation;
- no filesystem, network, device, process, sound, or graphics capabilities;
- modules are not yet authenticated by the package manager. Only run modules
  whose origin you trust; package signature and anti-rollback are still required
  before general distribution.

Both architecture build scripts generate and install `/apps/hello.kvm` from
`examples/kvm/hello.kvasm`. Open `/apps/hello.kvm` in the file manager to run
the same portable smoke module through the file-grant path.

The runner uses the same KVP0 interpreter source on both architectures. This
establishes build integration, not yet cross-architecture execution equivalence:
identical module fixtures must still be run under x86-64 and ARM64 QEMU/VM CI.

## Experimental assembler

`tools/kvm_asm.py` assembles the current KVP0 instruction syntax into a
portable container. It supports labels, entry points, integer/register
instructions, branches/calls, capability declarations, and initial data
(`.ascii`, `.byte`, `.zero`). Example:

```asm
.memory 1
.caps console
.entry start
start:
    const r0, 0
    const r1, 6
    syscall 0, console
    halt
.data
.ascii "hello!"
```

Build with `python3 tools/kvm_asm.py hello.kvasm hello.kvm`. The assembler
automatically includes capabilities used by `syscall` instructions and rejects
unknown labels, invalid registers, unsupported capability names, and out-of-range
immediates. It is an experimental SDK seed, not a frozen source language or a
replacement for the future C/KCC KVM backend. Regression tests run in the host
suite via `tests/kvm_asm_test.py`.

## ABI decision gate

This branch implements only an experimental **KVP0** prototype: a 32-byte header and
fixed register-file instruction model. It is deliberately not called KVM1 and is not a
public ABI. The proposed canonical **KVM1** format is the separate 64-byte sectioned
container in `docs/KIRA_VM_SPEC.md`, with typed function descriptors and named imports.
The two formats are incompatible and must never be auto-detected or reinterpreted as
one another. KVP0 exists only to test interpreter mechanics and the two-architecture
build path; it must not be used for general application packages or release images.

`docs/ADR-0001-KIRA-VM.md` records the comparison against the existing WebAssembly
runtime and the decision gate. The KVM1 specification remains draft until its loader,
typed verifier, imports, conformance suite and package integration are implemented.

## Explicit remaining work (not claimed complete)

1. Replace the first runner's path/file-grant loader with a signed KIRA package loader integrated with the package manager and KiraFS `/apps` path.
2. Define and freeze the KIRA host ABI, syscall registry, capability policy, and ABI
   version negotiation. The current callback is a narrow testable boundary, not a
   complete OS service implementation.
3. Add module signature/trust/anti-rollback checks before instantiation.
4. Add allocator-backed memory quotas, optional bounded memory growth, cancellation,
   pause/resume, watchdog scheduling, per-instance resource accounting, and diagnostics.
5. Add static control-flow/type analysis and fuzzing of the container decoder/verifier.
   The current verifier is structural; runtime checks remain mandatory.
6. Integrate the portable C runtime into both x86-64 and ARM64 userland builds and
   verify the same KVP0 fixtures on both architectures.
7. Add a compiler/backend or SDK that emits KVP0 modules; until then modules are
   hand-authored test fixtures, not a production app toolchain.
8. Consider a JIT only after interpreter semantics and cross-architecture tests are
   stable. JIT pages must be W^X, capability-equivalent, and fall back to interpretation.

## Migration and compatibility policy

- Existing WASM applications continue to run through the existing WASM runtime; no
  forced conversion or duplicate WASM implementation is introduced.
- New KIRA-native applications may target the typed KVM1 format only after the
  canonical loader, host ABI, package trust and conformance gates are completed.
  KVP0 remains a non-production prototype and is never a supported package target.
- Native x86-64/AArch64 applications remain supported through the native runtime.
- Compatibility Runtime and full hardware VMs remain separate projects with their own
  isolation boundaries.
- A future KVM1 version change requires a new format version or an explicitly
  backward-compatible extension; unknown versions/opcodes are rejected, never guessed.
