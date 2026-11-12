# RFC-TOOL-001 — KCC compiler context, reentrancy and deterministic multi-target builds

**Статус:** DRAFT — current implementation facts and target contract.  
**База main:** `bbc7f7ab948e72511aed113b69043add10ec8350`.

## 1. Current implementation facts

- Public API: `kcc_compile(const KCfg *cfg, void *mem, size_t memsz, const char *path, int emit_ir, KResult *res)`.
- Caller provides a bump arena; KCC avoids malloc in the compiler core. `KCfg` supplies source reader, include directories, macro definitions, target and optional arena-growth callback.
- `kcc/util.c` defines one process-global `KCtx K`. `kcc_compile()` begins with `memset(&K, 0, sizeof K)`, sets arena/config, and uses one global `jmp_buf` for error unwinding.
- Main baseline has separate x86-64 and AArch64 code generation paths and a large host/self-host test suite.
- This API is sequentially reusable only if every file-scope mutable state is reset or moved into `K`; it is not safe for concurrent calls because calls share `K`, arena bookkeeping, diagnostics and jump buffer.
- Open PR #93 and #95 move some PP/IR/backend/optimizer state toward per-compilation storage, but do not replace the process-global `K` with caller-owned contexts. PR #95's comment explicitly says it does not improve existing concurrency guarantees.
- KCC CI for #93 (`43201182562`) and #95 (`43211438660`) failed only the `kcc-tests` job at self-hosted stage1 compilation. #93 failed parsing `#define IRS ((IRState *)K.ir_state)` (`ir.c:14`, expected ')' before K); #95 failed parsing the replacement member alias (`ir.c:24`, expected ')' before ->). Main build, host tests and other jobs in those runs succeeded.

## 2. Target contract

Each compilation must have a distinct `KCtx` owned by the caller or an explicit context object created by the library. No mutable compiler state may be shared implicitly between independent compilations.

Context owns:
- arena base/capacity/used and grow callback;
- error jump target, error text and warning buffer;
- configuration, source path, macro token budget and include depth;
- preprocessor state and include cache;
- parser/type/AST state and built-in type instances;
- IR lowering, optimizer scratch pools and inline-analysis tables;
- target-specific backend state and temporary relocation/output buffers;
- statistics, diagnostics and dependency manifest.

Public API should be shaped as `kcc_compile(KCtx *ctx, const KCfg *cfg, ...)` or equivalent opaque-context lifecycle. A process-global convenience wrapper may exist only as a single-threaded compatibility API and must be explicitly named/documented as non-reentrant.

## 3. Required invariants

1. Two contexts never share mutable pointers, counters, token lists, AST/IR state, optimizer pools or backend state.
2. Errors unwind only the owning context; a failure in compile A cannot corrupt or longjmp through compile B.
3. Reusing a context requires explicit reset semantics; no stale preprocessor definitions, include guards, target flags, warnings or output survive unless explicitly configured.
4. Arena allocations and all references remain within the owning arena; growth preserves pointer validity or uses offset-based references.
5. x86-64 and AArch64 target selection is local to a context; interleaved builds cannot leak ABI/backend state.
6. Compiler output is deterministic for the same source bytes, include closure, options, compiler version and target.
7. Self-hosted KCC must compile its own unity source and runtime/tooling for both targets; host-only success is insufficient.

## 4. Migration strategy

1. Preserve current `kcc_compile` signature temporarily as a wrapper over a single static context, clearly mark non-reentrant.
2. Move one mutable-state family at a time into explicit context: PP → parser/types → IR → optimizer → common backend → x86/AArch64 backend.
3. Remove file-scope mutable globals and macros that hide context lookup; prefer explicit context parameters or a narrowly scoped context accessor that the self-host parser can compile.
4. Make diagnostics and `setjmp/longjmp` context-local; ensure nested compilation or callbacks cannot corrupt the active error frame.
5. Add host context tests, then self-host stage1/2 and in-KIRA build tests.
6. Only claim parallel compilation after an actual concurrent/interleaved test uses two independent arenas and targets.

## 5. Required test matrix

- sequential same-target compilation with changing headers/macros/options;
- x86-64 → AArch64 → x86-64 interleaving;
- two independent contexts interleaved at PP/parse/IR/backend boundaries;
- true concurrent calls if the runtime supports threads;
- error in one context while another continues successfully;
- arena exhaustion and grow callback per context;
- deterministic byte-for-byte output across repeated clean builds;
- self-host stage1 and stage2 builds with exact compiler SHA;
- malformed input and cancellation/resource-limit behavior.

## 6. Open decisions

- Opaque public context versus public struct layout.
- Whether a single context can be reused after failure or must be destroyed/reinitialized.
- Whether nested compilation is supported.
- Whether KCC on-device builds use one job at a time initially or support concurrent compiler services.
- Which allocator/growth semantics preserve pointer stability.

No KCC context RFC is accepted until #93/#95 self-host failures are fixed and the API is tested with truly independent contexts.
