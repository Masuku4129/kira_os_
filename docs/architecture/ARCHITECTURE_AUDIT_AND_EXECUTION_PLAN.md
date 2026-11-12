# KIRA OS — архитектурный аудит и исполняемый план

**Дата фиксации:** 2026-11-12  
**Базовая ветка аудита:** `main`  
**Приоритет:** Architecture  
**Цель:** привести всю систему к целевой архитектуре без исключения существующих направлений и без преждевременного переписывания работающих подсистем.

> Этот документ задаёт ближайший порядок работ. Он дополняет, а не заменяет `docs/MASTER_DEVELOPMENT_PLAN.md` и 12 параллельных потоков. Не начинать крупную реализацию до завершения соответствующего архитектурного gate.

## 1. Текущее состояние аудита — исходная точка

Сверка выполнена по GitHub `main` и обязательным документам `AGENTS.md`, `README.md`, `ARCHITECTURE.md`, `docs/MASTER_DEVELOPMENT_PLAN.md`, `docs/WORKLOG.md`, а также по списку открытых PR на 2026-11-12. Это предварительная документальная/репозиторная инвентаризация, а не завершённый построчный аудит всего кода.

### Подтверждено в документации репозитория

- Есть общая база ядра и отдельные x86-64/ARM64 архитектурные пути; заявлены BIOS/UEFI, SMP, memory management, user processes, syscalls, IPC, KiraFS, storage, network, GUI, audio, Mind и KCC. Каждый статус требует сверки с кодом и актуальным CI.
- Есть WASM user runtime, x86-64 JIT с W^X sealing и ARM64 interpreter path; master plan определяет WASM текущей production-базой KVR.
- Есть собственный KVM1 эксперимент в открытом PR #134. WORKLOG прямо указывает, что ABI 32-byte экспериментальный, конкурирует с 64-byte proposal и не имеет закрытых integration/conformance gates. Не считать его production runtime.
- Есть Driver Manager / drvget и KDRV package pipeline; WORKLOG фиксирует нерешённые trust-anchor, подписи/anti-rollback и IOMMU/реальное железо ограничения на активных ветках.
- Есть крупная активность по VT-d/SMMUv3, BIOS/UEFI, KCC, Mind, IPC, scheduler и изоляции в множестве открытых PR. Открытый PR и queued workflow не равны интеграции в main или PASS.
- Текущий `ARCHITECTURE.md` — накопительная история версий/пакетов, а не компактная нормативная спецификация целевой архитектуры. Его нельзя переписывать без сохранения фактической истории и ограничений.

### Явные архитектурные разрывы, требующие аудита

1. Нет единой нормативной схемы восьми уровней, отдельного Hypervisor-контурa и cross-cutting services, связанной с конкретными исходными модулями.
2. Совместимость Windows/Linux user applications ещё не оформлена как два самостоятельных ABI/runtime проекта с loader, syscall/API surface, libraries, profiles и conformance matrix.
3. Целевая KIRA VM и текущая production-база WASM требуют ясного разделения: конечная цель не должна незаметно подменять текущую стабильную базу, а экспериментальный KVM1 не должен стать production по факту наличия runner.
4. Системные контракты распределены между кодом и документами; нужен реестр интерфейсов, версий, владельцев, зависимостей и тестов.
5. Service Manager, Event Bus, Resource Manager, Power Manager, Crash Recovery и Observability должны быть инвентаризированы как реальные механизмы, частичные реализации или отсутствующие компоненты — без приписывания статуса по одному имени файла.
6. Device Model, Driver Framework, DMA/IOMMU и security/capability boundaries требуют end-to-end проверки и общей модели lifecycle.
7. Архитектурная документация и текущие открытые ветки/PR должны быть согласованы без автоматического слияния и без перезаписи чужой работы.

## 2. Порядок работ — ближайший исполняемый маршрут

Все 12 потоков мастер-плана сохраняются активными. Порядок ниже определяет архитектурные зависимости и интеграционные gates, а не запрещает независимую параллельную работу.

### Фаза 0 — фиксация источников истины (сейчас)

- Утвердить целевую архитектуру: `KIRA_OS_TARGET_ARCHITECTURE.md`.
- Этот документ становится нормативным целевым описанием; мастер-план сохраняет полный охват; `ARCHITECTURE.md` остаётся историей и фактической инвентаризацией до его отдельного рефакторинга.
- Все новые изменения должны указывать архитектурный слой, контракт и раздел мастер-плана.
- Никаких изменений `main` напрямую; работа через отдельные ветки и PR. Не сливать PR без явного разрешения владельца.

**Выход:** документы доступны агенту из `AGENTS.md` и README; конфликтующие решения перечислены, а не скрыты.

### Фаза 1 — полная инвентаризация и архитектурная карта

Провести аудит по каждому потоку и каждому значимому модулю:
- исходный путь/модуль и публичные символы;
- слой целевой архитектуры и роль;
- привилегии и граница процесса;
- входящие/исходящие зависимости;
- ABI/API и версия (или отметка «не специфицирован»);
- x86-64 статус / ARM64 статус;
- production / prototype / test-only / legacy / dead code;
- существующие host tests, QEMU сценарии, Limbo и hardware evidence;
- открытые дефекты, заглушки, дублирование, обходы security boundary;
- точные PR/branch зависимости и риск конфликта.

Создать и поддерживать матрицу `docs/architecture/COMPONENT_INVENTORY.md`, граф зависимостей `docs/architecture/DEPENDENCY_MAP.md`, findings `docs/architecture/SOURCE_AUDIT_FINDINGS.md` и tracker `docs/architecture/OPEN_PR_ARCHITECTURE_TRACKER.md`. Не маркировать модуль «реализовано» без исходного кода и конкретного успешного теста.

**Выход:** полная карта фактической системы и список противоречий с целевой моделью.

### Фаза 2 — реестр контрактов и ADR

Сначала зафиксировать/сверить:
1. BootInfo и bootloader handoff;
2. Native ELF ABI, syscall numbers/calling conventions и user/kernel boundary;
3. IPC endpoint/message/shared-memory/event semantics;
4. capabilities, identity, trust, revocation и audit;
5. memory mappings, W^X, shared pages, DMA/IOMMU;
6. Device Model, driver lifecycle, KDRV package/signature;
7. Service Manager, Event Bus, Resource Manager и error/restart model;
8. VFS/KiraFS/block layer/crash consistency;
9. runtime host APIs и package format;
10. Windows compatibility ABI/API boundary;
11. Linux ELF/syscall/POSIX compatibility boundary;
12. Hypervisor/VM ABI, guest memory, vCPU, interrupts and IOMMU;
13. Mind telemetry/request/policy boundary;
14. update/recovery/rollback contract.

Создать `docs/architecture/CONTRACT_REGISTRY.md` с owner, version, source headers, consumers, tests, failure semantics и migration policy. Не выдумывать номера syscall или ABI, которых нет в коде.

Каждое несовместимое решение оформлять ADR с вариантами, последствиями, безопасностью, производительностью, x86/ARM влиянием и планом миграции.

**Выход:** все критические границы имеют документированные и тестируемые контракты.

### Фаза 3 — архитектурное выравнивание всех 12 потоков

Проводить параллельно, но интегрировать через утверждённые контракты:
- Kernel/process/IPC/SMP: единый lifecycle, syscall/IPC, ошибки и cleanup.
- KVR/Native/compatibility: чётко разделить native, WASM, целевую KIRA VM, Windows Runtime, Linux Runtime.
- KCC: закрепить IR, object/ELF, ABI, x86-64/AArch64 backends и безопасный build pipeline.
- Mind: telemetry-only observation + bounded requests; policy checks и audit.
- Security: capabilities, isolation, signatures, secure boot, IOMMU, rollback.
- Drivers: Device Model, KDRV, lifecycle, source provenance, DMA constraints, sandbox.
- Storage: VFS/KiraFS/block API, atomicity, recovery и update integration.
- Network: единый socket/service contract и адаптация runtime API.
- GUI: оконный сервер/compositor, toolkit, input/audio/display services.
- Boot/install/update: единый boot contract, installer, update transaction, recovery.
- HAL: общий API и архитектурные backend-ы x86-64/ARM64.
- CI/QA: contract tests, negative tests, fuzzing, architecture matrix, artifact evidence.

Нельзя «подгонять» подсистему фиктивным адаптером, который лишь маскирует отсутствие реальной функции.

**Выход:** архитектурные зависимости направлены вниз через контракты; нет скрытых обходов и циклических зависимостей.

### Фаза 4 — Windows/Linux compatibility design (обязательное целевое направление)

Создать отдельные спецификации и roadmap:
- `docs/compat/WINDOWS_COMPATIBILITY.md`
- `docs/compat/LINUX_COMPATIBILITY.md`
- `docs/compat/COMPATIBILITY_TEST_MATRIX.md`

Для Windows: PE/COFF, DLL/import resolution, Win32 API families, thread/process/synchronization/memory semantics, exceptions/TLS, filesystem/registry abstraction, graphics/audio/input/network adapters. Не включать kernel driver compatibility в пользовательский runtime.

Для Linux: ELF, Linux syscall ABI, libc/dynamic linker expectations, POSIX semantics, signals/futex/mmap/epoll-like APIs as required, VFS/network/graphics/audio/input adapters. Не считать Linux kernel modules совместимыми.

Порядок: спецификация ABI/API surface → минимальный loader → syscall/API dispatch → libraries/services → изоляция и resource quotas → app compatibility tests → расширение по реальной статистике.

Первый вертикальный срез должен быть минимальным, но end-to-end: реальное приложение запускается, получает только выданные capabilities, использует несколько API, корректно завершается; negative tests проверяют отказ неизвестного API, некорректного PE/ELF, выхода из sandbox и исчерпания ресурсов.

**Выход:** матрица совместимости с конкретными приложениями и API, а не маркетинговое «поддерживает Windows/Linux».

### Фаза 5 — KIRA Hypervisor design и реализация

Отдельный документ `docs/virtualization/HYPERVISOR_ARCHITECTURE.md` должен определить:
- VM/vCPU lifecycle, guest physical/virtual memory;
- Intel VMX, AMD SVM, ARM EL2 backend separation;
- VM exits, interrupt/timer virtualization;
- virtual devices and host I/O mediation;
- snapshots/checkpoints, restore and crash containment;
- IOMMU domains and DMA isolation;
- security boundary between host kernel, hypervisor, guests and device emulation;
- test matrix for QEMU and supported physical hardware.

Сначала интерфейсы и host/guest threat model, затем hardware capability discovery и минимальный guest boot, затем виртуальные устройства и recovery. Не помещать Hypervisor в список application runtimes.

### Фаза 6 — архитектурная верификация и стабилизация

- Проверить статически направление зависимостей и запрет запрещённых импортов/вызовов.
- Добавить contract conformance tests и negative security tests.
- Запустить host tests, x86-64 build, ARM64 build, BIOS/UEFI QEMU, VM scenario matrix, runtime tests и Limbo smoke отдельно.
- Для каждого теста сохранить точный commit SHA, команду, окружение, лог и результат.
- Отдельно зафиксировать тесты физического железа; отсутствие устройства не заменять QEMU evidence.
- Исправлять первопричины найденных ошибок и повторять тесты до подтверждённого результата.

**Выход:** отчёт о соответствии целевой архитектуре с доказательствами, остаточными рисками и блокерами.

### Фаза 7 — реализация по приоритетам, определённым зависимостями

После фаз 1–3 составить конкретный набор небольших PR. Приоритет определяется не желанием быстрее показать функцию, а критичностью архитектурного контракта, количеством зависимых компонентов и риском безопасности. Все 12 потоков продолжаются; интеграция не означает последовательное замораживание разработки.

## 3. Текущее состояние выполнения (обновлено 2026-11-12)

- Целевая архитектура, исполняемый план, обязательные входные ссылки в AGENTS/README — созданы в draft PR #135.
- Первичная инвентаризация, dependency map и contract registry — созданы. Это первичный source-level pass, не полный аудит всех модулей.
- Отдельные Windows/Linux compatibility specs, compatibility matrix и Hypervisor threat model — созданы в `docs/compat/` и `docs/virtualization/`.
- Open PR architecture tracker создан; выявлены важные вложенные веточные цепочки KDRV, IOMMU, KCC, Mind, scheduler. Ни один PR не сливался в рамках этой работы.
- Source-audit findings зафиксировали конкретные риски: kernel stack slot double-free/free-list ownership (#132); отсутствие подтверждённой publisher signature в main KDRV path (signature PR chain ещё не в main); статическая name-based capabilities; Mind action boundary; host tests с no-op locks; WASM fuel test-only; широкий syscall dispatcher; IPC receive dequeue before user-copy semantics.
- Первые RFC drafts созданы: BootInfo, syscall/native ABI, capabilities, IPC/SHM. Они фиксируют только наблюдаемые факты и открытые решения, не замораживают ABI.
- Последний main CI на baseline SHA `bbc7f7ab948e72511aed113b69043add10ec8350`: workflow run `43244194123`, 30 jobs завершились success (host-tests, ARM64, build, QEMU VM scenarios, Limbo smoke/stress). Это evidence для указанного SHA и конкретной матрицы, не доказательство полной архитектуры или физического ARM64/x86 hardware.
- Для draft PR #135 workflow runs на его head SHA не обнаружены; документы не получили отдельного CI подтверждения. Последнее сравнение с main: 38 commits ahead, 0 behind, 19 changed files. Все 19 изменённых файлов просмотрены в compare; PR remains draft, not merged.
- Дополнительная проверка open PR CI: #42 head `b08513f387d9ae9cc779f9faaa7e1eee26d02c39` workflow `43236730618` завершился success для audit, driver-e2e, KCC, ARM64, host tests and build, но VM/package jobs skipped; это не release gate и не подтверждает hardware-backed anti-rollback. #134 head `aa5378d2a1cc52f9cbc55ab7432a4b0f7dad25a7` workflow `43236030817` failed: host-tests lint flagged `fake_ticks` stub; ARM64 and VM shards 5/7/9 failed. #132 head has no PR-triggered workflow run at check time.
- Никаких host tests или локальных сборок в этой сессии не запускалось.

## 4. Текущая рабочая очередь

| Приоритет | Задача | Зависимости | Критерий завершения |
|---|---|---|---|
| P0 | Зафиксировать целевую архитектуру и правила чтения документов | Нет | Ссылки из README/AGENTS, PR открыт |
| P0 | Инвентаризация репозитория по всем 12 потокам | P0 architecture | Component inventory с доказательствами |
| P0 | Реестр ABI/API/IPC/security/device contracts | Инвентаризация | Contract registry и владельцы |
| P0 | Сверить открытые PR/ветки с main и зависимостями | Текущий GitHub state | PR matrix без ложного PASS/merge |
| P1 | Разделить Windows/Linux compatibility design | Контракты | Две спецификации и тестовая матрица |
| P1 | Hypervisor architecture/threat model | Memory, Device Model, IOMMU | Отдельная спецификация и backend plan |
| P1 | Service/Event/Resource/Power/Recovery/Observability audit | Инвентаризация | Статус каждого механизма и gap list |
| P1 | KIRA VM vs WASM decision gate | ADR-0001, KVM1 evidence | Сравнение и отдельный ADR; production ABI не меняется молча |
| P1 | Architecture conformance CI | Contract registry | Проверки направленности зависимостей и контрактов |
| P2 | Вертикальные реализации по каждому потоку | P0/P1 contracts | Интеграционные PR с реальными tests |

## 5. Обязательные правила выполнения

- Перед каждой сессией перечитать `AGENTS.md`, мастер-план, этот документ, `WORKLOG.md`, `README.md`, `ARCHITECTURE.md` и затрагиваемые subsystem docs.
- Проверить свежие main SHA, branches, open PRs, workflow runs и точные head SHAs.
- Не работать только над последним CI failure: сохранять все 12 потоков и их зависимости.
- Не сливать PR и не менять main напрямую без явного разрешения владельца.
- Не закрывать задачу только созданием документа или файла: код, тесты и доказательства остаются отдельными gates.
- Не заявлять PASS по queued/in-progress workflow.
- Не перезаписывать существующие изменения и не делать массовые архитектурные рефакторинги без карты зависимостей.
- В WORKLOG фиксировать SHA, изменённые файлы, реальные команды/CI, фактические результаты и следующую точку продолжения.

## 6. Что делаем прямо сейчас

1. Создаём отдельную ветку для целевой архитектуры и плана; main не затрагиваем.
2. Добавляем нормативную архитектурную спецификацию и исполняемый audit plan.
3. Подключаем эти документы в обязательный вход для любого агента через `AGENTS.md` и README.
4. Открываем draft PR без слияния.
5. Следующая работа в этой ветке: компонентная инвентаризация и contract registry по фактическому коду; затем compatibility specs и Hypervisor threat model.

## 6. Статус этого документа

На момент создания это baseline-план. Полный source-level аудит ещё не завершён. Пункты, подтверждённые только WORKLOG или историческим ARCHITECTURE.md, должны быть перепроверены по текущему `main`/точному PR head и актуальным тестам. Не трактовать список открытых PR как наличие этих изменений в main.


## 7. Выполнение первичного source-level прохода (2026-11-12)

- Получен полный рекурсивный список main: 828 файлов; дерево не усечено. Это карта репозитория, не доказательство реализации.
- Созданы `COMPONENT_INVENTORY.md` и `DEPENDENCY_MAP.md` на основе первичного чтения kernel.c, sched.c, syscall.c, proc.c, elf.c, vmm.c, caps.c, kdrv.c, kfs.c, vfs.c, userland/wasm.c, mind.c/mind_sys.c, net.c, wm.c, build scripts и host-test runner.
- Первичные разрывы: в main не обнаружены отдельные Windows Compatibility Runtime, Linux Compatibility Runtime и hardware Hypervisor; текущий ELF path является KIRA-native ET_EXEC loader, не Linux loader; WASM — имеющаяся переносимая runtime база; KVM1 — эксперимент в PR #134, не production baseline.
- В коде видны статическая таблица caps по имени программы, широкая связность kernel init/syscall dispatch и управляющие точки Mind к системным подсистемам. Это не объявлено дефектом без полного call-graph и security review; это приоритетные зоны архитектурной проверки.
- KiraFS v2 описывает crash consistency через порядок записи без журнала; фактическая устойчивость должна быть подтверждена power-cut/fault-injection tests.
- **Тесты в этом проходе не запускались.** GitHub source review не заменяет локальную сборку, host suite, QEMU, Limbo или physical hardware tests.
- Следующий gate: расширить inventory до публичных символов/полных зависимостей; создать CONTRACT_REGISTRY; начать kernel/boot/memory/process/syscall/IPC contracts и параллельно детализировать compatibility/driver/storage/Mind/GUI/network/Hypervisor contracts.
