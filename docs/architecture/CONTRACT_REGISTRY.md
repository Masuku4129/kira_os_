# KIRA OS — реестр архитектурных контрактов

**Базовый снимок кода:** main @ bbc7f7ab948e72511aed113b69043add10ec8350  
**Целевая архитектура:** `docs/architecture/KIRA_OS_TARGET_ARCHITECTURE.md`  
**Статус:** реестр требований и проверки; не все контракты уже реализованы.

## Статусы
- **Требуется спецификация** — нормативный контракт ещё не утверждён.
- **Есть частичная реализация** — обнаружен код, но полнота и соответствие не доказаны.
- **Нужно проверить** — реализация/интерфейс существует, требуется проследить полный путь и тесты.
- **Не обнаружено в main** — отдельная реализация не найдена в проверенном снимке.
- **Утверждён** — спецификация принята, имеет владельца, версии и conformance tests.

Ни один контракт в этом первоначальном реестре пока не помечен «Утверждён» только на основании наличия исходников.

## Реестр

| ID | Контракт | Владелец (предлагаемый) | Текущая точка в main | Статус | Критерий утверждения |
|---|---|---|---|---|---|
| ABI-001 | Kernel syscall ABI | Kernel | kernel/syscall.c, syscall.h, arch syscall entry | Нужно проверить | номера/версии, calling conventions x86-64/AArch64, аргументы, ошибки, pointer validation, compat policy, conformance tests |
| ABI-002 | Native Application ABI | Runtime + Kernel | elf.c, proc.c, headers | Требуется спецификация | ELF contract, entry/stack, TLS, sysroot, libraries, syscalls, handles, app manifest, ABI versioning |
| PROC-001 | Process/thread lifecycle | Kernel | proc.c, sched.c | Частичная реализация | state machine, ownership, exit/reap, signals/events, thread teardown, resource limits, race tests |
| MEM-001 | VM/address-space ownership | Kernel + HAL | pmm.c, vmm.c, arch/* | Нужно проверить | page ownership, map/unmap, permissions, TLB shootdown, COW policy, OOM, SMP and ISA tests |
| IPC-001 | IPC endpoints and message transport | Kernel | syscall.c, kobj.c, futex.c, proc.c | Частичная реализация | endpoint capabilities, peer identity, message schema, bounds, timeout/cancel, peer death, DoS/backpressure tests |
| CAP-001 | Capability authority | Security + Kernel | caps.c, proc.c, syscall.c, kdrv.c | Частичная реализация | object-bound caps, least privilege, delegation, revocation, inheritance, audit, fail-closed tests |
| DEV-001 | Device Model | HAL + Driver Manager | PCI/USB and driver sources | Требуется спецификация | stable device identity, discovery, resource ownership, hotplug, bind/unbind and power lifecycle |
| DRV-001 | Driver ABI and lifecycle | HAL + Driver Manager | kdrv.c, kdrv.h, userland/kdrv.h | Нужно проверить | manifest, signed provenance, probe/bind, capabilities, IRQ/DMA, unbind, crash/restart, versioning |
| DMA-001 | DMA/IOMMU ownership | Memory + HAL + Security | IOMMU/DMAR sources and open PRs | Частичная реализация | domain lifecycle, map/unmap, invalidation, fault isolation, device teardown, adversarial tests |
| SVC-001 | Service manifest/lifecycle | Service Manager | no dedicated manager confirmed in main | Не обнаружено в main | dependency graph, startup order, readiness, health, restart, permissions, upgrade/rollback |
| EVT-001 | System Event Bus | IPC + Service Manager | IPC and local event paths | Требуется спецификация | typed/versioned events, access policy, delivery semantics, ordering, backpressure, subscriber crash |
| RES-001 | Resource accounting/control | Kernel + Resource Manager | sched.c, pmm.c, scale.c, Mind interfaces | Частичная/распределённая | quotas, reservations, accounting, limits, priority and policy ownership |
| FS-001 | VFS contract | Storage | vfs.c, vfs.h, fat.c, blk.c | Частичная реализация | path/namespace semantics, mount lifecycle, handles, permissions, concurrency, error mapping |
| FS-002 | KiraFS on-disk format and durability | Storage | kfs.c, kfs.h | Нужно проверить | format versioning, metadata invariants, ordering, crash consistency, corruption recovery, power-cut tests |
| PKG-001 | Package manifest/trust | Package Manager + Security | package-related code; exact coverage pending | Требуется спецификация | content hashes, signer identity, dependency solver, provenance, rollback/anti-rollback |
| UPD-001 | Atomic system update/recovery | Update + Boot | release/boot sources; full path pending | Требуется спецификация | staged update, verified boot selection, rollback, interrupted update, user-data preservation |
| HOST-001 | Common Runtime Host API | Runtime + Security | WASM imports and native syscalls | Требуется спецификация | common capability-mediated host services, ABI versioning, quotas, audit, denied-operation tests |
| WASM-001 | WASM module ABI/runtime | WASM Runtime | userland/wasm.c, JIT/interpreter tests | Частичная реализация | supported proposal/features, verifier, memory safety, fuel, imports, traps, W^X, ARM64 parity |
| KVM-001 | KIRA VM bytecode/runtime | KIRA VM | ADR-0001, experimental PR #134 | Эксперимент вне main | frozen bytecode spec, verifier, deterministic semantics, compiler, package format, conformance suite |
| LNX-001 | Linux ELF/syscall compatibility | Linux Compatibility | no dedicated runtime confirmed in main | Не обнаружено в main | ELF interpreter/dynamic linker, Linux syscall coverage, signals/futex/epoll, procfs, libc and real-app matrix |
| WIN-001 | Windows PE/Win32/NT compatibility | Windows Compatibility | no dedicated runtime confirmed in main | Не обнаружено в main | PE/DLL loader, Win32/NT semantics, registry/files/process/thread/network/graphics/audio and app matrix |
| VM-001 | Hypervisor/guest ABI | Hypervisor + Kernel + Security | no hardware guest hypervisor confirmed in main | Не обнаружено в main | vCPU lifecycle, guest memory, exits, virtual devices, IOMMU, guest isolation and escape tests |
| MIND-001 | Mind telemetry API | KIRA Mind + Observability | mind.c, mind_sys.c, mindnet.c | Нужно проверить | typed read-only metrics, provenance, privacy, rate limits, schema versions |
| MIND-002 | Mind control request API | KIRA Mind + resource owners + Security | mind_sys.c control functions | Архитектурный риск / нужно проверить | request→policy→capability→bounds→apply→audit; TTL, rollback, denial and abuse tests |
| OBS-001 | Logs/metrics/tracing/crash reports | Observability | UART/logging/watchdog/life/panic paths | Частичная реализация | structured schemas, correlation IDs, retention, redaction, crash dump integrity |
| BOOT-001 | Boot handoff contract | Boot + HAL + Kernel | bootmap.c, boot/ and arch paths | Нужно проверить | versioned BootInfo, memory map, firmware data, entropy, framebuffer, modules and malformed-input tests |
| ARCH-001 | Architecture backend/HAL contract | HAL | arch/x86_64, arch/aarch64 | Нужно проверить | no ISA leakage above HAL, explicit feature detection, ABI/atomic/memory-order parity |
| TOOL-001 | KCC/toolchain object ABI | Toolchain + Runtime | kcc/*, tools/* | Нужно проверить | object format, relocations, calling conventions, debug info, deterministic build and cross-target tests |

## Правила контракта

Каждый контракт перед реализацией должен содержать:
1. Scope, non-goals, terminology and authoritative owner.
2. Data structures and wire/on-disk/in-memory formats.
3. Call sequence/state machine and lifecycle.
4. Error model, cancellation, timeout and resource exhaustion behavior.
5. Security boundary, identity, capability checks and audit requirements.
6. Versioning, backward compatibility and migration/deprecation policy.
7. x86-64 and ARM64 behavior, including architecture-specific exceptions.
8. Positive, negative, concurrency, fuzz and fault-injection tests.
9. Performance budgets and measurable acceptance criteria.
10. Implementation mapping: source paths, symbols, tests, CI jobs and known gaps.

## Порядок утверждения

1. ABI-001, BOOT-001, ARCH-001.
2. MEM-001, PROC-001, CAP-001, IPC-001.
3. DEV-001, DRV-001, DMA-001.
4. FS-001, FS-002, PKG-001, UPD-001.
5. SVC-001, EVT-001, RES-001, OBS-001.
6. HOST-001, WASM-001, KVM-001, LNX-001, WIN-001.
7. VM-001 и MIND-001/MIND-002.
8. TOOL-001 и GUI/application contracts.

Порядок утверждения не запрещает параллельную подготовку RFC; он задаёт зависимости интеграции.

## Изменение контракта

Изменения ABI, IPC, on-disk format, capability semantics, driver ABI, runtime host API или guest boundary требуют ADR/RFC, migration plan, compatibility tests и явного review владельцев затронутых подсистем. Нельзя менять контракт скрыто в рамках несвязанного feature PR.


## RFC mapping (draft only)
- BOOT-001 → `docs/architecture/rfc/RFC-BOOT-001.md`
- ABI-001 / ABI-002 → `docs/architecture/rfc/RFC-ABI-001.md`
- CAP-001 → `docs/architecture/rfc/RFC-SEC-001.md`
- IPC-001 / EVT-001 → `docs/architecture/rfc/RFC-IPC-001.md`

RFC index: `docs/architecture/rfc/README.md`. No draft RFC is approved or ABI-frozen yet.


- TOOL-001 → `docs/architecture/rfc/RFC-TOOL-001.md` (KCC context/reentrancy/deterministic build; draft).

- MIND-001 → `docs/architecture/rfc/RFC-MIND-001.md` (telemetry, model integrity, bounded control and rollback; draft).
- SVC-001 / HOST-001 → `docs/architecture/rfc/RFC-SVC-001.md` (syscall core vs service adapters; draft, transitional in-kernel extraction).
- DEV-001 → `docs/architecture/rfc/RFC-DEV-001.md` (device identity, resource ownership, driver binding and lifecycle; draft, source-backed migration contract).

- SVC-001 implementation boundary: syscall router and per-subsystem adapters are separated in source; current execution remains in kernel context pending IPC and Service Manager contracts.
