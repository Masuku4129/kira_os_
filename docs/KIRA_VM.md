# KIRA VM v1 — portable application bytecode

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

## KVM1 container format

All integer fields and instruction immediates are little-endian. The header is exactly
32 bytes; code immediately follows it, then the initial data image. The file must end
exactly after the declared data bytes; trailing bytes are rejected.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `KVM1` |
| 4 | 2 | format version, currently 1 |
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

The v1 decoder validates magic, version, exact header size, code alignment, entry,
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
- Memory: private linear memory, fixed at module creation in v1. Loads/stores are
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

- KVM1 container decoder with little-endian fields, strict length checks, and hard 16 MiB module / 16 MiB linear-memory ceilings.
- Structural verifier for the complete code stream and all direct control-flow targets.
- Instance-owned immutable-by-copy instruction stream and private initialized memory.
- Interpreter for the v1 opcode set, bounded call stack, deterministic fuel, memory
  bounds, arithmetic traps, and capability-tagged host calls.
- Host regression cases for arithmetic, loops, branches, calls, recursion overflow,
  memory load/store, module/data copy isolation, KVM1 decoding, malformed containers,
  invalid opcodes/targets, missing capabilities, division by zero, out-of-bounds
  memory, and fuel exhaustion.

## First OS runner integration

The `/bin/kvm` userland runner is built for x86-64 Ring 3 and ARM64 EL0 and
accepts one module path argument or a file grant from the file manager. It loads
the KVM1 blob, applies the runner policy before allocation, verifies it, creates
a private instance, runs with a fixed fuel budget, and destroys the instance.

The first runner profile is intentionally narrower than the portable core:
- module container: at most 1 MiB;
- guest linear memory: at most 128 pages (512 KiB);
- fuel: 10,000,000 instructions per invocation;
- host capabilities: console write (maximum 4 KiB per call), monotonic ticks,
  and cooperative yield only;
- no filesystem, network, device, process, sound, or graphics capabilities;
- modules are not yet authenticated by the package manager. Only run modules
  whose origin you trust; package signature and anti-rollback are still required
  before general distribution.

The runner uses the same KVM1 interpreter source on both architectures. This
establishes build integration, not yet cross-architecture execution equivalence:
identical module fixtures must still be run under x86-64 and ARM64 QEMU/VM CI.

## ABI decision gate

This branch currently implements an experimental 32-byte KVM1 header with a fixed
register-file instruction model. A separate draft proposal on
`feat/kira-vm-runtime-foundation` describes a different 64-byte sectioned container
with typed function descriptors and named imports. These formats are not compatible.
Neither proposal is a frozen public ABI. Before either is merged as the canonical KIRA
VM format, reconcile the two designs, select one versioned format, and add migration
and conformance fixtures. Do not silently reinterpret one format as the other.

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
   verify the same KVM1 fixtures on both architectures.
7. Add a compiler/backend or SDK that emits KVM1 modules; until then modules are
   hand-authored test fixtures, not a production app toolchain.
8. Consider a JIT only after interpreter semantics and cross-architecture tests are
   stable. JIT pages must be W^X, capability-equivalent, and fall back to interpretation.

## Migration and compatibility policy

- Existing WASM applications continue to run through the existing WASM runtime; no
  forced conversion or duplicate WASM implementation is introduced.
- New KIRA-native applications may target KVM1 once the package, host ABI, and build
  integration steps above are completed.
- Native x86-64/AArch64 applications remain supported through the native runtime.
- Compatibility Runtime and full hardware VMs remain separate projects with their own
  isolation boundaries.
- A KVM1 version change requires a new format version or an explicitly backward-
  compatible extension; unknown versions/opcodes are rejected, never guessed.
