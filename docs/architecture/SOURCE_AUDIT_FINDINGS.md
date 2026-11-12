# KIRA OS — подтверждённые первичные source-audit findings

**Кодовая база:** `main` @ `bbc7f7ab948e72511aed113b69043add10ec8350`  
**Дата:** 2026-11-12  
**Метод:** чтение исходников через GitHub; тесты в этой сессии не запускались.  
**Статус:** findings требуют независимого воспроизведения и/или проверки текущего PR; не являются окончательным security verdict.

## F-001 — kernel stack slot allocator не защищён от повторного освобождения

- **Severity:** High — memory corruption / duplicate allocation risk.
- **Files:** `vmm.c`, функции `vmm_kstack_alloc`, `vmm_kstack_free`.
- **Evidence:** allocator возвращает слот из `ks_free[--ks_nfree]`; free проверяет только диапазон адреса, затем без проверки ownership/active state делает `ks_free[ks_nfree++] = slot`. Нет проверки точного base, активного владения и capacity перед append. При double-free один slot может попасть в free list дважды и быть выдан двум потокам; повторные освобождения могут переполнить free list.
- **Related work:** открытый draft PR #132 `fix/vmm-kstack-double-free` заявляет active-slot bitmap, exact-base/ownership checks и bounded free-list append. Его head SHA на снимке: `2b8569ad1baf8596e48c81ebebcdcefe8fae859d`; base — `v46/unified-release-integration`, не main.
- **Action:** не создавать конкурирующий fix поверх main. Проверить #132 diff и интеграционную ancestry; добавить unit/host allocator lifecycle test (allocate/free, double-free, interior pointer, foreign pointer, exhaustion, partial map failure, concurrent alloc/free); затем x86-64/ARM64 builds и QEMU lifecycle stress. До этого помечать stack-slot lifecycle как unverified/high-risk.

## F-002 — KDRV publisher authenticity не подтверждена кодом main

- **Severity:** High — supply-chain trust boundary.
- **Files:** `kdrv_format.h`, `kdrv_fmt.c`, `kdrv.c`, `fsrv.c`, `docs/DRIVERS.md`.
- **Evidence:** текущий `kdrv_rec_t` содержит SHA-256 package/ELF hashes, source string, confirmation marker and granted capabilities. `kdrv.c:pkg_load` сверяет digest package и ELF с локальной записью `.rec`; `kdrv_fmt.c:kdrv_rec_check` проверяет структуру/confirmation/cap mask. В просмотренном main path не обнаружена криптографическая подпись издателя, проверка доверенного signer key или anti-rollback publisher version. SHA-256 обнаруживает несовпадение с локальной записью, но сам по себе не аутентифицирует издателя, если атакующий может заменить и пакет, и запись.
- **Documentation mismatch:** `docs/DRIVERS.md` описывает проверку подписи, но main implementation path, просмотренный в этом аудите, доказывает только hash/confirmation record. Это следует считать документально-кодовым расхождением до интеграции/проверки PR.
- **Related work:** signature chain PR #35/#36/#38/#39/#40/#41/#42 and integration #44/#46/#12. На снимке #42 head `b08513f387d9ae9cc779f9faaa7e1eee26d02c39`, #41 head `5aefeb8cf1d0d13f8e9781c3cf5fe85bfedb2b8e`; они образуют зависимую веточную цепочку и не входят в main.
- **Action:** review the full PR ancestry and exact cumulative diff; establish trust anchor/key rotation/revocation, signer identity, signature domain (manifest + ELF + permissions + device match), anti-rollback and offline recovery. Add tamper tests that replace both package and .rec; require invalid/unknown signer rejection.

## F-003 — Linux/Windows compatibility runtimes absent from main source map

- **Severity:** Architecture blocker, not an immediate defect.
- **Evidence:** native `elf.c` path is KIRA's own ELF64 executable format/personality (machine selected by build target); no dedicated Linux syscall personality, Linux dynamic-linker profile, PE/COFF loader or Win32/NT user-mode dispatch identified in the inspected main tree.
- **Action:** implement as separate user-space runtimes against the shared capability-mediated host API. Track actual application-level vertical slices, not parser-only milestones. Specs are now in `docs/compat/`.

## F-004 — Hardware hypervisor not established by the existing VMM

- **Severity:** Architecture blocker, not an immediate defect.
- **Evidence:** `vmm.c` implements host virtual address mapping/page tables and kernel stack slots. It is not evidence of VMX/SVM/EL2 guest execution, vCPU lifecycle, VM exits, EPT/NPT/Stage-2, virtual devices or guest isolation.
- **Action:** separate hypervisor control plane and architecture backends; threat model and acceptance criteria in `docs/virtualization/HYPERVISOR_ARCHITECTURE.md`.

## F-005 — Capability authority is currently static/name-based

- **Severity:** Medium/High — depends on threat model and trust of launch paths.
- **Files:** `caps.c`, `proc.c`, `syscall.c`.
- **Evidence:** `caps_for_program(name)` maps a fixed application name to a bitmask; `proc.c:spawn` assigns that mask at launch. `syscall.c:cap_needed` checks syscall capability bits centrally. No general object-bound capability handle, delegation/revocation API or dynamic policy authority was identified in the reviewed files.
- **Action:** preserve the existing deny-by-default allowlist as baseline while specifying object capabilities, endpoint rights, delegation/revocation and audit. Do not weaken current syscall checks during migration.

## F-006 — Mind action surface is direct and needs a formal policy gate

- **Severity:** Medium/High — privileged policy boundary.
- **Files:** `mind_sys.c`, `mind.c`, scheduler/process/resource APIs.
- **Evidence:** action callbacks include scheduler quantum/weight/limit, process heap restriction/kill, USB HID verdict, display effects and CPU reservation. This may be intentionally trusted in-kernel code, but the target architecture requires explicit bounds, ownership, audit and rollback. A bounded request→policy→apply path was not established by this source slice.
- **Action:** enumerate all action callbacks, identify policy validation and rate/bounds checks at the authoritative owner, record action provenance and rollback. Move noncritical policy out of kernel where feasible; keep enforcement at resource owners.

## F-007 — VFS/KiraFS concurrency evidence differs between host tests and production

- **Severity:** Medium — test validity concern.
- **Files:** `vfs.c`, `kfs.c`.
- **Evidence:** `vfs.c` deliberately compiles lock operations as no-ops under `KIRA_HOST_TEST`; production path uses kmutex. `kfs.c` similarly omits lock in `KIRA_HOST_TEST`/`KFS_NO_THREADS`. Host tests therefore cannot prove lock correctness, races or production concurrency behavior.
- **Action:** keep host tests for format/logic correctness, but add kernel/QEMU concurrency tests with real mutexes and fault injection. Explicitly document what each host test does not cover.

## F-008 — WASM production runtime has a deliberate time-slice, but fuel accounting is host-test-only

- **Severity:** Needs validation; not classified as a defect from this evidence alone.
- **Files:** `userland/wasm.c`.
- **Evidence:** `WASM_FUEL` enables deterministic fuel exhaustion accounting in host tests; production build compiles `FUEL_TICK` to no-op and uses periodic `wasm_slice()` yielding based on elapsed time. This is a valid alternative only if process cancellation, scheduler fairness and non-yielding paths are proven.
- **Action:** add/verify a production runtime test for infinite loops, tight loops, host-call loops, cancellation and scheduler responsiveness on x86-64 and ARM64. Do not equate host-test fuel exhaustion with production instruction quota.

## F-009 — broad syscall dispatcher is a cross-layer coupling hotspot

- **Severity:** Architecture debt.
- **Files:** `syscall.c`, `fsrv.c`, `gfxsrv.c`, `net.c`, `audio.c`, `kdrv.c`.
- **Evidence:** `syscall.c` includes and dispatches directly to many service implementations; syscall numbers cover graphics, files, IPC, network, audio, drivers and JIT. Some are legitimate kernel primitives; others appear to be service calls.
- **Action:** preserve stable syscall ABI while moving service-level behavior behind capability-checked IPC/service endpoints. Do not break user ABI or existing apps during staged migration.

## Validation state

No tests were executed during this source-review pass. Findings above are code observations and risk statements, not test failures. The next validation sequence is:
1. review PR #132 and reproduce allocator ownership invariants;
2. review KDRV signature PR chain and test replacement of both package and record;
3. run existing host tests and production x86-64/ARM64 builds at a recorded SHA;
4. add production/QEMU tests for VFS/KFS concurrency, WASM scheduling, Mind control and driver lifecycle;
5. update each finding with fixed commit, test command, exact result and residual risk.
