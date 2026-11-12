# KIRA VM — runtime specification (draft 0.1)

**Purpose:** define the separate native KIRA application VM required by `docs/MASTER_DEVELOPMENT_PLAN.md`.  
**Relationship to WASM:** KIRA VM is not a replacement for the existing WebAssembly runtime. WASM remains the portable standards-based runtime; KIRA VM is a KIRA-native bytecode/runtime with its own module ABI. Compatibility and hardware virtualization remain separate tracks.

## 1. Security and execution model

- One KIRA VM instance runs inside one ordinary Ring 3/EL0 KIRA process. The process address space is the outer isolation boundary; the VM adds an inner boundary for module memory, registers, call stack, imports and resource quotas.
- A VM instance never receives kernel pointers, physical addresses, PCI/MMIO access or raw syscall numbers.
- Host operations are named imports checked against a per-instance capability set. Every import validates arguments and buffer ranges before calling the narrow KIRA process API.
- Each instance owns a separate linear memory, value/register state, call stack, import table and execution budget.
- CPU instruction budget, maximum call depth, memory pages, open handles and output sizes are bounded. Exhaustion traps the instance; it must not hang the scheduler.
- Shared memory is not implicit. Any future shared-memory facility requires explicit object handles, rights, lifetime and synchronization.
- Native JIT is not part of the first interpreter milestone. If later added, it must use process-owned RW-NX pages and one-way R-X sealing; never W+X.

## 2. Module container: KVM1

All integer fields are little-endian. The loader must use checked arithmetic for every offset, count, multiplication and range. No pointer is formed from an unvalidated file offset.

### 2.1 Fixed header (64 bytes)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `KVM1` |
| 4 | 2 | major | exactly 1 |
| 6 | 2 | minor | supported minor, otherwise reject |
| 8 | 4 | header_size | exactly 64 in v1 |
| 12 | 4 | flags | unknown bits reject |
| 16 | 4 | total_size | equals file size |
| 20 | 4 | entry_function | valid function index |
| 24 | 4 | function_count | 1..1024 |
| 28 | 4 | functions_offset | aligned to 8 |
| 32 | 4 | code_offset | aligned to 8 |
| 36 | 4 | code_size | non-zero, multiple of 8 |
| 40 | 4 | data_offset | aligned to 8 |
| 44 | 4 | data_size | may be zero |
| 48 | 4 | imports_offset | aligned to 8 |
| 52 | 4 | import_count | bounded |
| 56 | 4 | memory_min_pages | 4 KiB pages |
| 60 | 4 | memory_max_pages | >= min, bounded by policy |

The header and every section must lie entirely within `total_size`. Sections may not overlap. Reserved padding bytes must be zero. The v1 maximum file size is 16 MiB; default linear-memory maximum is 16 MiB (4096 pages) and may be lowered by system policy.

### 2.2 Function descriptor (32 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | first_instruction (index in code section) |
| 4 | 4 | instruction_count |
| 8 | 2 | register_count (1..256) |
| 10 | 1 | parameter_count (0..32) |
| 11 | 1 | return_type |
| 12 | 4 | register_types_offset (file offset; one byte per register) |
| 16 | 4 | flags (v1 must be zero) |
| 20 | 12 | reserved, all zero |

Type IDs: `0=void` (return only), `1=i32`, `2=u32`, `3=i64`, `4=u64`. Register declarations may not be void. Parameter registers are registers `0..parameter_count-1`; their types are the first entries in the register type vector. A function has one scalar return value or void.

Function instruction ranges may not overlap and must fit inside the code section. Register type vectors must be in-bounds, non-overlapping with other sections and long enough for `register_count`.

### 2.3 Import descriptor (24 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | name_offset (absolute file offset) |
| 4 | 2 | name_length (1..63 bytes) |
| 6 | 2 | flags (must be zero) |
| 8 | 4 | signature (registry-defined ABI signature ID) |
| 12 | 4 | capability (required capability bit; zero only for explicitly public, side-effect-free imports) |
| 16 | 8 | reserved, all zero |

The name is a bounded canonical ASCII identifier stored in the module. It must not contain NUL, control characters or non-ASCII bytes. The runtime compares the full name against its compile-time registry; no hash-only dispatch is permitted. The module does not supply function pointers or kernel syscall numbers. Unknown names, signature mismatches and missing capabilities reject the module.

## 3. Instruction encoding

Each instruction is exactly 8 bytes:

- byte 0: opcode;
- byte 1: destination register `a`;
- byte 2: source register `b`;
- byte 3: source register / argument count `c`;
- bytes 4..7: unsigned or signed little-endian immediate `imm32`.

The decoder must reject unknown opcodes, non-zero reserved operands, invalid register indices and invalid type combinations. Branch immediates are signed instruction-relative offsets from the instruction following the branch. Branches may target only instructions within the current function.

### 3.1 Initial opcode set

| Opcode | Operation | Semantics |
|---:|---|---|
| 0x00 | NOP | no state change |
| 0x01 | HALT | terminate instance with exit code in register `a` |
| 0x02 | CONST_I32 | `a = (i32)imm32` |
| 0x03 | CONST_I64 | `a = sign_extend(imm32)`; larger constants use a paired high/low immediate form in the next format revision |
| 0x04 | MOV | `a = b`, types identical |
| 0x10 | ADD | typed integer add; wrap modulo declared width |
| 0x11 | SUB | typed integer subtract; wrap modulo declared width |
| 0x12 | MUL | typed integer multiply; wrap modulo declared width |
| 0x13 | DIV_S | signed division; zero and min/-1 trap |
| 0x14 | DIV_U | unsigned division; zero traps |
| 0x18 | EQ | compare `b` and `c`, write i32 boolean to `a` |
| 0x19 | LT_S | signed comparison, write i32 boolean |
| 0x1A | LT_U | unsigned comparison, write i32 boolean |
| 0x20 | LOAD8_U | load byte from linear memory at `b + imm32`, zero extend to `a` |
| 0x21 | LOAD32 | load 32-bit word from linear memory at `b + imm32` |
| 0x22 | LOAD64 | load 64-bit word from linear memory at `b + imm32` |
| 0x23 | STORE8 | store low byte of `a` to `b + imm32` |
| 0x24 | STORE32 | store low 32 bits of `a` to `b + imm32` |
| 0x25 | STORE64 | store 64 bits of `a` to `b + imm32` |
| 0x30 | JMP | unconditional branch by signed `imm32` instructions |
| 0x31 | JZ | branch if register `a` is zero |
| 0x32 | JNZ | branch if register `a` is non-zero |
| 0x40 | CALL | call function `imm32`; copy `c` arguments from caller registers starting at `b` to callee registers 0..c-1; result to `a` |
| 0x41 | RET | return register `a` according to function signature |
| 0x50 | IMPORT | call import `imm32`; `b` is first argument register, `c` argument count, result to `a` (void imports require `a=0xFF`) |
| 0x60 | MEM_SIZE | write current linear memory size in bytes to `a` |
| 0x61 | MEM_GROW | grow by `b` pages; return old page count or `0xFFFFFFFF` on quota/allocation failure |
| 0x70 | YIELD | voluntarily yield to the KIRA scheduler |

The opcode table is intentionally small. New operations require a versioned specification and positive plus negative verifier tests. Floating point, SIMD, exceptions, threads and atomics are out of scope for v1.

## 4. Verification before execution

The verifier must reject a module before executing any instruction unless all checks pass:

1. Header/version/total size, section bounds, alignment, overlap and reserved bytes.
2. Function count, entry index, function ranges, register type vectors and signatures.
3. Every instruction decodes fully; opcode operands and register types are valid.
4. All branch targets remain within the same function.
5. Every reachable path has a valid terminal instruction; no fall-through past function end.
6. CALL target exists, argument count/types match callee parameters, return type matches destination.
7. IMPORT index exists; signature and capability are known and the caller's grant includes the required capability.
8. Memory access base is an integer register; effective address arithmetic cannot overflow; runtime bounds are checked for every access.
9. Static checks must not replace runtime checks for memory addresses or resource consumption.
10. The verifier itself uses bounded work and memory; malformed modules must not cause unbounded recursion or allocation.

The v1 verifier uses declared register types and a control-flow graph per function. Since registers have fixed declared types, joins do not require dynamic type inference; each opcode must match those declarations.

## 5. Runtime semantics

- Interpreter state is per instance; no mutable global VM state is shared between instances.
- Each call frame contains function index, instruction pointer, return destination and a bounded register file. Maximum call depth is a system policy (default 128).
- Linear memory is zero-initialized, grows only up to both module maximum and system policy, and is never executable.
- Data section initializes the start of linear memory; data size must not exceed initial memory.
- Every LOAD/STORE checks the full access width using subtraction-based bounds checks to avoid overflow.
- Every instruction consumes budget. Backward branches, calls and imports also check cancellation and scheduler deadlines.
- Traps terminate only the VM instance/process and are logged with module identity, function and instruction index.
- Imports are deterministic only where explicitly documented. Blocking imports must yield and have timeouts.
- A VM package must be authenticated by the system package trust policy before launch; bytecode verification is still mandatory after signature verification.

## 6. First implementation milestones

1. Host-portable format parser and verifier, no OS syscalls, with fuzzable byte buffers.
2. Reference interpreter with deterministic tests for arithmetic, branches, calls, memory, imports, traps and budget exhaustion.
3. Negative fixtures for every bounds/type/signature/capability failure and fuzz harness under ASan/UBSan.
4. KIRA process wrapper in Ring 3/EL0 using existing process isolation and capability APIs.
5. KiraFS package installation/manifest trust and runtime launch integration.
6. x86-64 and ARM64 interpreter builds and identical cross-architecture test vectors.
7. Resource manager, pause/resume, process cancellation and telemetry.
8. Optional JIT only after interpreter, verifier, lifecycle and cross-architecture semantics are stable.

## 7. Explicit non-goals for v1

- Do not implement a hypervisor or run whole guest operating systems in KIRA VM.
- Do not execute arbitrary native code inside the VM.
- Do not make VM imports direct kernel entry points.
- Do not silently fall back from a failed verifier to native execution.
- Do not claim full WASM compatibility; WASM remains its own runtime.
- Do not implement unbounded threads, shared memory or dynamic library loading in v1.
