# KIRA OS — Stage execution tracker

**Нормативный план этапов:** [целевая архитектура и Stage 0–13](architecture/KIRA_TARGET_ARCHITECTURE_AND_STAGE_PLAN_2026-11-15.md)  
**Текущий этап:** Stage 0 — аудит и архитектурная подготовка.  
**Статус:** IN PROGRESS. Следующий Stage запрещено начинать до формального закрытия gate текущего этапа.  
**Правило статуса:** `DONE` только при наличии проверяемых артефактов и evidence; процент не выставлять без измеримой шкалы.

## Общая карта

| Stage | Название | Статус | Зависимость | Условие перехода |
|---:|---|---|---|---|
| 0 | Полный аудит, целевая архитектура и план миграции | **IN PROGRESS** | — | Все критерии Stage 0 ниже закрыты, остаточные риски явно приняты, архитектурные решения утверждены |
| 1 | Boot chain, firmware и BootInfo | BLOCKED — ждёт Stage 0 | 0 | Firmware/BootInfo контракты, malformed-input fuzzing, producer/consumer tests, x86-64 + AArch64 QEMU, Limbo regression |
| 2 | HAL и CPU foundations | BLOCKED — ждёт Stage 1 | 1 | Cross-ISA contracts, interrupt/timer/SMP stress, affinity/teardown race tests |
| 3 | Memory manager и безопасность адресных пространств | BLOCKED — ждёт Stage 2 | 2 | Sanitizer host tests, adversarial allocator, page-fault isolation, stress без leaks/double-free/aliasing |
| 4 | Kernel core, processes и syscall ABI | BLOCKED — ждёт Stage 3 | 3 | Lifecycle fuzzing, starvation/deadlock, ABI version, SMP stress, crash containment |
| 5 | Typed IPC и системная модель объектов | BLOCKED — ждёт Stage 4 | 4 | Schema compatibility/fuzzing, rights-escalation negatives, lifetime/race tests, измерения IPC |
| 6 | Device model, драйверы и IOMMU/DMA | BLOCKED — ждёт Stage 5 | 5 | DMA isolation, hostile firmware, reset/timeout/restart, device QEMU + hardware matrix |
| 7 | Storage, filesystems и integrity layer | BLOCKED — ждёт Stage 6 | 6 | Power-loss injection, corruption fuzzing, recovery/idempotency, rollback и benchmarks |
| 8 | Network stack и service runtime | BLOCKED — ждёт Stage 7 | 7 | Protocol conformance/fuzzing, resource exhaustion, service restart/dependency failures |
| 9 | Compiler, KIRA IR и build system | BLOCKED — ждёт Stage 8 | 8 | Differential tests, IR verifier, reproducible builds, backend parity, bootstrap/signatures |
| 10 | KIRA Virtual Runtime и приложения | BLOCKED — ждёт Stage 9 | 9 | WASM conformance, parser/verifier fuzzing, sandbox negatives, quotas, ISA parity |
| 11 | KIRA Mind и data/AI services | BLOCKED — ждёт Stage 10 | 10 | Deterministic policy, provenance, rollback, privacy/resource budgets, no unauthorized authority |
| 12 | Graphics, Spatial Minimal и user experience | BLOCKED — ждёт Stage 11 | 11 | Interaction/performance/accessibility, multi-resolution, input latency, QEMU/Limbo visual regression |
| 13 | Hypervisor, install/update/recovery и release engineering | BLOCKED — ждёт Stage 12 | 12 | Clean-room builds, complete image manifest/hash/size, install/update/recovery, VM isolation, exact-head CI |

> Зависимости обозначают архитектурные ворота. Внутри Stage можно вести независимые исследования и подготовку следующих направлений, но нельзя объявлять следующий Stage начатым или интегрировать его production-миграцию до закрытия текущего gate.

## Stage 0 — критерии закрытия

- [ ] Полная инвентаризация всех tracked-файлов, сборочных целей, образов, ресурсов и генерируемых артефактов; размеры/manifest полной сборки зафиксированы. Current-main tree comparison completed; actual built image and disk manifests still open.
- [ ] File → subsystem → build target → test → runtime matrix без неучтённых файлов и orphaned production modules.
- [x] Все требования мастер-плана A–N и прежние 12 потоков перенесены в source-linked traceability ledger и сопоставлены с Stage 0–13 (routing ещё требует verification по компонентам).
- [ ] Проверить каждую запись traceability ledger по source/build/test matrix; подтвердить, что нет потерянных требований или ошибочного Stage mapping.
- [ ] Полный PR/branch graph: каждый открытый PR классифицирован по ancestry, base/head SHA, diff, CI exact head, зависимости и риску потери уникального изменения.
- [ ] Source-level аудит критических путей по всем подсистемам, не только найденных CI failures; findings имеют severity, точные файлы/строки, доказательство, remediation и тест закрытия.
- [ ] CI/host/QEMU baseline привязан к SHA; Limbo и физическое железо указаны отдельно, неподтверждённые среды не называются проверенными.
- [ ] Архитектурные контракты: privilege boundaries, capability ownership/lifetime/revocation, address/memory domains, ABI/versioning, IPC, DMA/IOMMU, storage integrity, driver trust, Mind authority, build provenance.
- [ ] Языки и форматы распределены по задачам; все новые технологии имеют ADR/benchmark gate, а не объявлены внедрёнными по умолчанию.
- [ ] Migration map показывает порядок изменений, зависимости, совместимость, rollback и сохранение старых функций/артефактов.
- [ ] Тестовая стратегия: unit/host, sanitizers, fuzzing, integration, QEMU, Limbo, hardware; команды и критерии PASS/FAIL воспроизводимы.
- [ ] Все открытые архитектурные вопросы либо решены ADR, либо явно помечены как блокирующие/допустимые с владельцем решения.
- [ ] Независимый review Stage 0 документов и проверка актуальности всех ссылок/commit SHA.
- [ ] Владелец проекта получил краткий итог и явно разрешил переход к Stage 1.

## Текущие Stage 0 рабочие пакеты

| ID | Пакет | Статус | Артефакт / следующий шаг |
|---|---|---|---|
| S0-01 | Целевая архитектура и карта Stage | PARTIAL | `KIRA_TARGET_ARCHITECTURE_AND_STAGE_PLAN_2026-11-15.md`; draft, требуется полный cross-check |
| S0-02 | Crosswalk требований A–N | PARTIAL — 233 source-linked records | `STAGE0_REQUIREMENT_TRACEABILITY_2026-11-15.md` содержит 135 вложенных требований A–N, 12 потоков, исторические gates, release и §8 policy. Осталось проверить каждый пункт по source/build/test matrix и уточнить Stage routing |
| S0-03 | Инвентаризация репозитория/build/test | IN PROGRESS — current-main tree captured | `STAGE0_CURRENT_MAIN_INVENTORY_2026-11-15.md`: 830 blobs, no removed paths vs pinned baseline; 3 source files changed. ISO + KiraFS disk pair, x86 release filter and ARM64 profile asymmetry documented. Exact UELF/disk manifests and image hashes still missing |
| S0-04 | PR/branch graph | PARTIAL | `STAGE0_OPEN_PR_GRAPH_2026-11-15.md`; 92 PR на момент снимка, ещё нет полного diff/ancestry/CI review каждого PR |
| S0-05 | Source-level findings | IN PROGRESS — F-062 current-main CI break confirmed | `STAGE0_SOURCE_FINDINGS_2026-11-14.md`; current-main FDT/WASM findings and subsystem source audit remain open |
| S0-06 | CI/test baseline | IN PROGRESS — CURRENT MAIN FAIL; coverage matrix added | Main SHA `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`, run `43643758138`: ARM64/host-tests/x86 build FAIL on F-062; QEMU skipped. Fix and exact-head rerun are required before baseline can be accepted |
| S0-07 | ADR и technology decisions | IN PROGRESS | Сформировать ADR backlog и закрыть блокирующие контракты/решения |
| S0-08 | Migration/dependency plan | NOT STARTED | По каждому Stage определить последовательность и rollback без потери функций |
| S0-09 | Independent Stage 0 review | NOT STARTED | Независимая проверка completeness и exit gate |

## Правила обновления трекера

1. Менять статус только вместе с доказательством в GitHub.
2. Для PASS указывать commit SHA, workflow/run, job и среду; для локальных тестов — точную команду и результат.
3. Не закрывать finding только потому, что fix существует в draft PR.
4. Не считать branch-only реализацию частью main или релизного ISO.
5. Любое изменение stage gate согласовывать через документированное решение; не удалять критерии ради ускорения.


### Stage 0 evidence links added 2026-11-15

- Requirement traceability: `architecture/STAGE0_REQUIREMENT_TRACEABILITY_2026-11-15.md` — 233 source-linked rows.
- Current main tree/build profile: `architecture/STAGE0_CURRENT_MAIN_INVENTORY_2026-11-15.md` — recursive tree comparison, x86/ARM64 build-profile comparison, CI exact-head failure evidence.
- Current main is not CI-green: F-062 is a confirmed build blocker on `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`.


- Source/build/test coverage matrix: `architecture/STAGE0_SOURCE_BUILD_TEST_MATRIX_2026-11-15.md`. This maps critical subsystems to source, build profiles and wired host tests, while explicitly identifying VMM, FDT, ELF, update/recovery and architecture parity gaps.
- New finding F-064: FDT parser accepts v16 but host fixture is v17 only; missing strings-block boundary and NUL-termination tests, with a bounded string-compare risk in `fdt_prop()`.
