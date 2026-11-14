# KIRA OS — целевая архитектура и новый поэтапный план

**Статус:** DRAFT FOR STAGE 0 — архитектурное направление сформировано; решения требуют проверки по исходникам, контрактам и измерениям.  
**Дата:** 2026-11-15  
**Исходная база аудита:** `main` @ `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Рабочая ветка:** `architecture/stage0-repository-audit-2026-11-14`

Этот документ вводит новую целевую архитектуру и поэтапную карту работ. Он не объявляет предложенные технологии уже внедрёнными, не отменяет требования `docs/MASTER_DEVELOPMENT_PLAN.md` и не разрешает переписывать/удалять существующие подсистемы. На Stage 0 все направления мастер-плана A–N и параллельные потоки должны быть сопоставлены с этой картой; пропуски фиксируются, а не отбрасываются.

## 1. Целевая архитектура

### 1.1 Архитектурная модель

Целевое направление — **гибридное capability-микроядро**:

1. Минимальное привилегированное ядро отвечает за CPU/IRQ, адресные пространства, планирование, syscall entry, базовые capability-объекты и безопасное управление памятью.
2. Системные службы работают в отдельных адресных пространствах: device manager, storage, filesystem, network, display/compositor, audio, input, package/trust, telemetry и KIRA Mind.
3. Драйверы по возможности работают в user mode как изолированные сервисы. Прямой доступ к MMIO/IRQ/DMA выдаётся только через проверяемые capabilities и ограниченные device resources.
4. HAL скрывает различия x86-64 и AArch64. Архитектурный код остаётся в отдельных backends; общие контракты проверяются на обеих архитектурах.
5. IPC — типизированный бинарный протокол с явными версиями, правами, размерами, временем жизни и правилами владения. Для крупных потоков данных используется shared memory/ring buffers/zero-copy только при доказанной корректности владения.
6. KIRA Virtual Runtime использует существующий WASM runtime как основной переносимый application runtime до отдельного решения на основании сравнительных измерений. Native ABI остаётся отдельным контролируемым путём для доверенных системных компонентов.
7. KIRA Mind — непривилегированный policy/analysis service. Он не получает произвольного права менять память, планировщик, драйверы или безопасность; действия проходят через ограниченные системные API, политики, журнал и откат.
8. KIRA Driver Manager может находить, получать, проверять лицензию и происхождение драйверов, адаптировать исходный код и пересобирать его, но не загружает неизвестные сетевые бинарники. Поддержка устройства подтверждается отдельными тестами и матрицей оборудования.
9. KIRA Hypervisor — отдельный слой/служба после стабилизации базовой изоляции, а не обязательная часть раннего ядра.
10. Spatial Minimal — графическая оболочка поверх display/compositor services; интерфейс не должен диктовать архитектуру ядра.

**Почему гибрид, а не чистое микроядро или монолит:** существующий проект уже содержит крупный C kernel, драйверы и сценарии Limbo/QEMU. Полный одномоментный перенос создал бы чрезмерный риск и не дал бы доказуемого выигрыша. Гибрид позволяет постепенно выносить наиболее рискованные подсистемы за границу привилегий, сохраняя работающие пути и совместимость. Каждое вынесение должно иметь измеримый выигрыш по изоляции, отказоустойчивости, сопровождению или производительности.

### 1.2 Основные контракты, которые должны быть зафиксированы до миграции

- BootInfo: версионированное описание загрузчика, памяти, firmware tables, framebuffer, initrd/modules и архитектурных возможностей.
- HAL: CPU, interrupt controller, timer, MMU/TLB, SMP, cache maintenance, DMA/IOMMU, firmware, power.
- Kernel object/capability model: тип объекта, generation, rights, transfer rules, revocation, lifetime, audit.
- Syscall ABI и Native ABI: calling convention, errors, structure layout, alignment, endianness, version negotiation, pointer validation.
- IPC ABI: message header, schema/version, bounded payload, handles/capabilities, queue backpressure, cancellation/timeouts.
- Driver ABI: device identity, resources, MMIO mapping, IRQ binding, DMA mapping, lifecycle, crash/restart, compatibility declarations.
- Storage ABI: block device, flush/barrier semantics, filesystem service, transactions, recovery and integrity.
- Runtime ABI: WASM host imports/capabilities, module manifest, quotas, cancellation, package signatures.
- Service lifecycle: discovery, start/stop, dependencies, health, restart policy, logging and recovery.
- Security policy: fail-closed defaults, least privilege, secure boot/signing, rollback, IOMMU domains, W^X, audit trail.

## 2. Языки программирования по зонам ответственности

Язык выбирается по требованиям к safety, ABI, runtime, toolchain maturity и возможности тестировать на обеих ISA. Смешанный стек допустим; смешение ABI без явного контракта — нет.

| Язык | Целевая область | Решение/ограничение |
|---|---|---|
| C + KCC | Существующее ядро, критический low-level код и постепенная миграция | Сохраняем текущую базу. Расширяем KCC и его IR по измеримому плану. Не переписываем всё на Rust автоматически. |
| Assembly | Вход/выход из исключений, context switch, boot, atomics/barriers и узкие CPU-specific участки | Только там, где ABI/ISA требуют точного контроля; оборачивать контрактами HAL. |
| Rust `no_std` | Новые изолированные системные службы и user-mode драйверы | Целевое расширение, не текущая интеграция. Сначала доказать target/toolchain, panic/alloc strategy, FFI/IPC ABI и reproducible build на x86-64 + AArch64. |
| WASM / WASI + KIRA host API | Переносимые приложения и расширения KIRA Mind | Продолжать существующий WASM runtime; WASI-профиль и host imports ограничивать capabilities. Не объявлять полной WASM/WASI совместимости до conformance. |
| Python | Хостовые инструменты разработки, генерация, анализ, тест orchestration и ML research | Не использовать как обязательную часть boot/runtime. Скрипты должны иметь pinned dependencies и воспроизводимые входы. |
| Bash | Сборка, упаковка, CI и Termux workflow | Тонкий orchestration; сложную логику выносить в тестируемые утилиты. |
| SPARK Ada / Zig | Исследовательские кандидаты | Не вводить в production, пока конкретный модуль не покажет измеримый выигрыш и не появится поддерживаемый cross-toolchain. |

### 2.1 KIRA IR и компиляторная цепочка

Целевой pipeline: **frontend(s) → MLIR (где полезно) → KIRA IR/SSA → verification + optimization → backend/codegen → signed artifact**.

- KIRA IR должен иметь строгую типовую модель: фиксированные целочисленные типы i8–i128/u8–u128, usize/isize, f32/f64, bool, struct/enum/variant, Option/Result, массивы/slices, явные атомики и endian-типы.
- Адреса разных доменов не смешивать: PhysAddr, VirtAddr, DmaAddr; capability handles — отдельный тип, не сырой указатель.
- Ownership/borrowing и lifetime metadata должны быть представлены в IR там, где фронтенд языка это поддерживает.
- MLIR использовать как инфраструктуру компиляторных диалектов/оптимизаций, а не как требование тащить весь LLVM/MLIR runtime в ядро.
- KCC остаётся самостоятельным критическим проектом; поддержка нового языка не должна ломать bootstrap, компиляцию ядра и стабильный Native ABI.
- Content-addressed build cache и PGO включать только после детерминированных сборок и измерений качества/скорости.

## 3. Форматы данных, хранения и обмена

Формат выбирается по нагрузке. Не следует сериализовать всё одним форматом или хранить всё в одном универсальном контейнере.

| Формат/технология | Предпочтительное назначение | Что проверить до принятия |
|---|---|---|
| FlatBuffers | Типизированные сообщения и структуры IPC, где важны быстрый доступ без полной распаковки и forward/backward compatibility | Размер кода/схем, verifier cost, alignment, zero-copy safety, скорость на x86-64/AArch64 |
| Cap’n Proto | Альтернативный IPC/schema candidate с zero-copy моделью | Сложность runtime, untrusted input validation, alignment/endian, capability transfer semantics |
| CBOR | Компактные конфиги, manifests, metadata и диагностические сообщения | Canonical encoding, strict limits, parser fuzzing, signature canonicalization |
| Arrow | Колончатый обмен аналитическими данными KIRA Mind и telemetry pipeline | Memory footprint, alignment, schema evolution, streaming batches; не использовать для обычных syscall сообщений без доказанного смысла |
| Parquet | Долговременное аналитическое хранение/экспорт больших наборов данных | Стоимость реализации/зависимостей, compression ratio, recovery, suitability для embedded workload |
| Zstandard (Zstd) | Сжатие пакетов, архивов и больших data batches | Memory/CPU limits, streaming, dictionary trust, decompression bombs |
| ELF | Текущий исполняемый формат и совместимость с toolchain | Сохранить до решения о versioned KIRA executable container; проверить relocations, segments, signatures |
| WASM binary | Переносимые приложения и sandbox modules | Версионированные профили, parser/verifier, imports, resource quotas |
| CAS + Merkle DAG + CoW | Объекты/пакеты/снимки с дедупликацией и проверяемой целостностью | Crash consistency, GC, pinning, reference accounting, atomic root updates |
| LSM-tree / B+Tree | Индексы и хранилища по разным профилям записи/чтения | Write amplification, recovery, compaction latency, random read/write benchmarks |
| Event log | История событий, telemetry и аудит действий | Bounded retention, privacy, integrity, replay semantics, backpressure |

### 3.1 Приоритетные сочетания для экспериментов

1. CAS + Merkle DAG + Copy-on-Write + цифровые подписи — целостность и атомарные обновления пакетов/системных объектов.
2. Typed IPC + capabilities + shared memory/ring buffers — контроль доступа и эффективная передача больших буферов.
3. Arrow + Zstd + append-only event log — аналитический контур KIRA Mind, не критический путь ядра.
4. KIRA IR/SSA + content cache + PGO — компиляторная оптимизация и воспроизводимая сборка.
5. Async I/O + DMA + IOMMU — высокопроизводительные устройства при обязательной изоляции DMA.
6. LSM/B+Tree — выбрать по workload-тестам, возможно разные структуры для metadata и append-heavy telemetry.

Ни одна из этих комбинаций не считается принятой реализацией до ADR с benchmark, security review и тестами восстановления/негативных сценариев.

## 4. Новый план Stage

Этапы — последовательные архитектурные ворота. Внутри завершённого и утверждённого Stage допускается параллелить независимые задачи, но интеграция идёт через единые контракты. Никакая строка старого мастер-плана A–N не удаляется: каждому требованию назначается Stage и проверяемый критерий.

### Stage 0 — Полный аудит, целевая архитектура и план миграции
**Текущий этап.** Инвентаризация исходников/сборки/CI/PR, line-by-line аудит критических путей, аудит старых ADR и веточных расхождений, языковой и форматный инвентарь, dependency graph, список подтверждённых дефектов, рисков и тестовых пробелов. Зафиксировать целевую архитектуру, интерфейсы, ADR backlog, migration map и stage gates.  
**Выход:** утверждённая архитектурная спецификация; component/file/build/test matrix; baseline CI/host/QEMU evidence; полный реестр findings с severity, доказательствами и статусом; никаких неучтённых branch-only изменений.

### Stage 1 — Boot chain, firmware и BootInfo
BIOS/UEFI, Multiboot2/EFI handoff, нормализованный BootInfo, memory map, ACPI/FDT, framebuffer, initrd/modules, secure boot path.  
**Gate:** malformed firmware input fuzzing, producer/consumer tests, x86-64 + AArch64 QEMU boot matrix, regression Limbo x86.

### Stage 2 — HAL и CPU foundations
HAL contracts; CPU discovery/features; exceptions/IRQ; LAPIC/IOAPIC и GIC; timers; per-CPU state; SMP bring-up; context switch; cache/TLB primitives.  
**Gate:** cross-ISA contract tests, interrupt storm, timer drift, SMP stress, affinity and teardown races.

### Stage 3 — Memory manager и безопасность адресных пространств
PMM/VMM, ownership, guarded stacks, W^X/NX, user access, ASID/PCID where supported, TLB shootdown, COW, allocator hardening.  
**Gate:** ASan/UBSan host tests, adversarial allocator tests, QEMU page-fault isolation, no leaks/double-free/aliasing under stress.

### Stage 4 — Kernel core, processes и syscall ABI
Threads/process lifecycle, scheduler, synchronization, syscall ABI, signals/events, cancellation, resource accounting, kernel objects and capabilities.  
**Gate:** lifecycle fuzzing, starvation/deadlock tests, ABI version tests, SMP stress, crash containment.

### Stage 5 — Typed IPC и системная модель объектов
Capability transfer/revocation, typed message schema, bounded queues, shared memory, ring buffers, async request/reply, backpressure and cancellation. Benchmark FlatBuffers vs Cap’n Proto vs CBOR (and custom fixed ABI where justified).  
**Gate:** schema compatibility tests, malformed-input fuzzing, rights-escalation negatives, race/lifetime tests, throughput/latency measurements.

### Stage 6 — Device model, драйверы и IOMMU/DMA
PCI/PCIe, ACPI/FDT enumeration, MMIO resource manager, IRQ routing, DMA API, IOMMU (VT-d/SMMUv3), USB, storage, network, audio and GPU drivers. Gradual user-mode driver extraction. Driver Manager with source/license/provenance, sandbox, build, signing, rollback.  
**Gate:** per-device DMA isolation, hostile firmware state, reset/timeout/restart, device-specific QEMU tests and real hardware matrix. No DMA-capable untrusted driver before IOMMU policy is enforced.

### Stage 7 — Storage, filesystems и integrity layer
Block async I/O, flush/barrier semantics, KiraFS/FAT/VFS, journaling or CoW transactions, CAS/Merkle object store, signatures, package DB. Evaluate LSM vs B+Tree per workload; Parquet for analytics only if needed.  
**Gate:** power-loss injection, corruption fuzzing, recovery/idempotency, signed update rollback, read/write benchmarks.

### Stage 8 — Network stack и service runtime
Network drivers, IPv4/IPv6, TCP/UDP, TLS, service discovery, service lifecycle manager, logging/event bus, update/package services.  
**Gate:** protocol conformance, fuzzing, resource exhaustion, service restart and dependency failure tests.

### Stage 9 — Compiler, KIRA IR и build system
KCC frontend and stable ABI; KIRA IR/SSA; MLIR bridge where useful; x86-64/AArch64 backends; Rust no_std integration; WASM toolchain; deterministic build, content cache and PGO.  
**Gate:** compiler differential tests, IR verifier, reproducible builds, backend parity, bootstrap and artifact signature verification.

### Stage 10 — KIRA Virtual Runtime и приложения
Harden and extend existing WASM runtime; WASI-like capability profile; module manifests, quotas, cancellation, app lifecycle, native trusted services. Evaluate any KIRA-specific VM only by ADR and benchmarks.  
**Gate:** WASM conformance subset, parser/verifier fuzzing, sandbox escape negatives, resource limits, x86-64/AArch64 parity.

### Stage 11 — KIRA Mind и data/AI services
Separate Mind services, policy engine, telemetry/event pipeline, Arrow/Zstd analytics, model/plugin isolation, learning and rollback. Mind never gains implicit kernel authority.  
**Gate:** deterministic policy tests, provenance/audit trail, rollback, privacy and resource budget, no unauthorized privileged action.

### Stage 12 — Graphics, Spatial Minimal и user experience
Display/compositor/input/audio services, GPU acceleration, windows, shell, spatial desktop, contextual system indicators, dock and unobtrusive KIRA AI. Driver Manager GUI.  
**Gate:** interaction tests, rendering/performance budgets, accessibility, multi-resolution, input latency, QEMU/Limbo visual regression and hardware caveats.

### Stage 13 — Hypervisor, install/update/recovery и release engineering
KIRA Hypervisor, VM/device virtualization, installer, partitioning, signed atomic updates, rollback, recovery environment, reproducible x86-64 and AArch64 release images.  
**Gate:** full clean-room build, image manifest/hash/size comparison, install/upgrade/downgrade/recovery, VM isolation and exact-head full CI. No release claim without complete artifacts and exact test evidence.

## 5. Stage 0 work packages and completion criteria

Stage 0 is not complete until all of the following are true:

- [ ] Verify branch/commit graph, open PRs, stacked dependencies, branch-only implementations and current main drift.
- [ ] Complete repository inventory: files, languages, build inputs, generated files, binary assets, tests, workflows, image packaging.
- [ ] Map every source module to component, build target, architecture, test coverage, owner/dependencies and production-vs-test status.
- [ ] Audit all critical kernel boundaries line by line: boot/firmware, PMM/VMM, process/stack lifecycle, syscall/uaccess, capabilities/IPC, PCI/DMA/USB, block/filesystem, network, WASM/JIT, KCC, graphics, Mind.
- [ ] Reconcile all existing findings and branch PR fixes against pinned baseline; do not treat branch-only fixes as integrated.
- [ ] Reconcile every A–N requirement and all existing master-plan workstreams with the new stages.
- [ ] Write ADRs for hybrid kernel boundary, capability/object model, typed IPC, language policy, executable/runtime ABI, serialization/storage formats, driver model, KIRA IR/toolchain and KIRA Mind authority.
- [ ] Establish benchmark harnesses and representative workloads before choosing between competing formats/engines.
- [ ] Establish exact baseline commands/results for host tests, x86-64 build, AArch64 build, QEMU matrix and Limbo smoke test. Keep physical-hardware items explicitly unverified.
- [ ] Define no-loss checks for source tree, assets, package contents and ISO output before any future release build.

**Stage 0 exit:** no unresolved architecture conflict that would force rework of later stages; all current components and workstreams have a destination; every chosen technology has a reason, acceptance criteria and fallback; all known defects are recorded; baseline evidence is reproducible; next Stage has bounded tasks and dependencies.

## 6. Technology decision rules

1. Prefer the smallest runtime and dependency surface compatible with the required capability.
2. Measure both x86-64 and AArch64; a win on one ISA is not enough for a portable system.
3. Benchmark cold start, steady-state latency, throughput, peak memory, code size, verifier cost, failure recovery and power/CPU cost where measurable.
4. Security and lifecycle correctness are hard gates; performance cannot compensate for an isolation flaw.
5. Keep wire/storage formats versioned, bounded and fuzzable. Never sign ambiguous/non-canonical encodings.
6. Every external component/library needs license, provenance, maintenance and supply-chain review.
7. Experimental technologies stay isolated until ADR acceptance; no speculative mass migration.
8. Preserve current working paths during migration; switch only after parity and rollback tests.
9. Keep production code, host models, test fixtures and experimental prototypes clearly separated.
10. Never mark a feature supported solely because it compiles or appears in a file tree.

## 7. Initial repository facts (pinned baseline)

The recursive tree at the pinned baseline contains 861 entries. Among tracked blobs, the extension inventory is: 593 `.c`, 165 `.h`, 15 assembly `.s`, 13 `.sh`, 11 `.py`, 16 `.md`, 3 `.yml`, plus one each of `.bin`, `.rec`, `.cfg`, `.txt`, `.out`, `.inc`. Case-insensitive inventory shows no tracked `.rs`, `Cargo.toml`, `.toml`, `.proto`, `.fbs`, `.capnp`, `.cddl`, `.wasm`, or `.wat` files at this baseline. This is a tree inventory only; it does not imply no generated/untracked artifacts in developer environments.

Current build/CI evidence in source:
- x86-64 build uses Clang + LLD + NASM; freestanding C and assembly.
- AArch64 has a separate build script and architecture-specific implementation.
- CI has host tests, x86-64 image build, AArch64 build/QEMU scenarios, and a broad x86 QEMU scenario matrix.
- Existing WASM runtime and JIT are in C; x86-64 JIT is not evidence of ARM64 JIT.
- Python/Bash are already used for host-side tooling and CI.
- Rust and the proposed schema/data formats are target candidates, not current integrated production components.

These facts are pinned to the stated baseline. Later branch/main changes must be re-inventoried before using them as current state.
