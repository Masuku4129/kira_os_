# KIRA OS — план архитектурного аудита и исполнения

Статус: обязательный план текущего архитектурного цикла.
Дата: 2026-11-12.
Целевая архитектура: docs/KIRA_OS_TARGET_ARCHITECTURE.md.

## Главная задача

Полный архитектурный аудит репозитория, сопоставление фактического кода с целевой архитектурой, устранение противоречий и фиксация стабильных контрактов. Не наращивать функции хаотично поверх неясных границ.

Аудит не означает остановку разработки. Все направления продолжают подготовку и безопасную реализацию параллельно, с непересекающимися границами. Ни одно направление не исключается.

## Этап 0 — исходная точка (сразу)

1. Зафиксировать default branch, commit SHA, все активные ветки и открытые PR.
2. Для каждого PR установить цель, подсистемы, зависимости, тесты, конфликты и архитектурные последствия. Не сливать и не закрывать автоматически.
3. Снять карту репозитория: исходники, headers, спецификации, build scripts, CI, тесты и артефакты.
4. Запустить доступные базовые проверки без изменения кода; сохранить команды, окружение, SHA и результаты.
5. Зафиксировать baseline, не выдавая неподтверждённые функции за готовые.

Выход: воспроизводимая исходная точка и реестр активной работы.

## Этап 1 — инвентаризация компонентов

Проверить все направления:
- Kernel: boot, exceptions/IRQ, scheduler, memory/VM, processes, syscalls, IPC, SMP, synchronization.
- HAL/Drivers: x86-64, ARM64, device model, PCI/USB, storage, GPU, audio, network, DMA/IOMMU, power.
- Security: capabilities, isolation, signatures, secure boot, keyring, policies, audit.
- Storage: block layer, VFS, KiraFS, cache, journaling/transactions, recovery.
- Execution: native ABI/runtime, ELF, PE/COFF, Linux ABI, Windows API, WASM, KIRA VM, app sandbox, Hypervisor.
- Services: service manager, event bus, resource manager, package/update, network services, observability.
- Mind: telemetry, forecasting, policy, control API, recovery.
- User space: compositor, GUI, desktop, explorer, settings, terminal, SDK/tooling.
- Boot/install/release: BIOS/UEFI, boot manager, installer, updater, ISO, ARM images.
- Toolchain: KCC, IR, compiler backends, assembler/linker, reproducibility.
- Validation: unit, host, integration, QEMU, Limbo, fuzzing, sanitizers, performance, fault injection.

Для каждого компонента создать запись:
path | responsibility | layer | public interfaces | dependencies | privilege | status | evidence/tests | risks | next action.

Статусы строго: Реализовано, Частично, В разработке, Не начато, Не проверено, Заблокировано. Отмечать stubs/mocks/dead code отдельно.

Выход: component inventory, dependency graph, duplicate/conflict register, evidence matrix.

## Этап 2 — противоречия и зависимости

1. Построить граф зависимостей и отметить циклы.
2. Найти обходы ABI/IPC/security, утечки привилегий и скрытые глобальные состояния.
3. Найти дублирующие реализации, конкурирующие структуры и несогласованные форматы.
4. Проверить различия x86-64/ARM64 и host/emulator/real hardware.
5. Выделить блокеры дальнейшей безопасной интеграции.
6. Составить миграцию без массового переписывания и потери рабочего кода.

Выход: ранжированный список проблем с доказательствами и точными файлами.

## Этап 3 — спецификация контрактов

Порядок RFC/spec:
1. Kernel syscall ABI и Application ABI для x86-64/AArch64.
2. Process/thread/handle model и capabilities.
3. IPC, shared memory и Event Bus.
4. Memory ownership, DMA и IOMMU.
5. Device Model и Driver ABI.
6. Service manifest, lifecycle и Service Manager.
7. VFS/KiraFS, durability и recovery.
8. Package/update transaction и rollback.
9. Native/WASM/KIRA VM host APIs.
10. Linux ELF/syscall compatibility.
11. Windows PE/Win32/NT user-mode compatibility.
12. Hypervisor boundary, VM lifecycle, virtual devices и isolation.
13. Mind telemetry/control API.
14. Observability schema и test requirements.

Каждый RFC содержит scope/non-goals, структуры данных, call flow, ошибки, версии, безопасность, отказные сценарии, performance budget, compatibility policy, тесты и migration plan.

Выход: согласованные контракты, исключающие неформальные обходы уровней.

## Этап 4 — параллельная архитектурная миграция

Параллельные потоки с явными границами файлов:
1. Kernel/process/IPC/SMP.
2. Native ABI, ELF/PE loaders, Linux/Windows compatibility.
3. KIRA VM и WASM runtimes.
4. KCC, IR и compiler backends.
5. KIRA Mind и safe control API.
6. Security, capabilities, memory safety, IOMMU, signatures.
7. Driver framework, Device Model и Driver Manager.
8. KiraFS, block/storage и recovery.
9. Network stack/services.
10. Desktop, compositor, GUI и system apps.
11. BIOS/UEFI, installer, update/recovery.
12. HAL x86-64/ARM64, CI, fuzzing, performance, release.
13. Hypervisor и virtual devices.

Параллельность не означает одновременное редактирование одних файлов. Сначала согласовать интерфейс, затем реализацию. Существующие PR не терять и не переносить вслепую.

Выход: миграционные PR с тестами и ссылками на RFC.

## Этап 5 — интеграция и сквозная проверка

Проверить цепочки:
- boot → kernel → memory → process → syscall → IPC;
- device discovery → driver bind → DMA/IOMMU → service API;
- block device → VFS → KiraFS → package install → update → rollback;
- app loader → runtime → capabilities → host APIs;
- Linux ELF → Linux ABI → KIRA services;
- Windows PE → Win32/NT API → KIRA services;
- Mind telemetry → proposal → policy check → controlled action → audit;
- Hypervisor VM create → vCPU/memory → virtual devices → guest boot → teardown;
- GUI session → compositor → input/graphics/audio → apps;
- x86-64/ARM64 parity for shared contracts.

Негативные сценарии: malformed binaries, invalid syscalls, permission denial, peer crash, resource exhaustion, device removal, DMA violations, interrupted update, corrupted package, guest escape attempts, malformed VM state и Mind request outside policy.

Выход: сквозные результаты, coverage matrix и список дефектов.

## Этап 6 — производительность, надёжность, безопасность

Зафиксировать baseline latency/throughput/memory/boot time; измерить scheduler, IPC, memory, filesystem, network, graphics и runtime overhead; запустить fuzzing/sanitizers; выполнить fault injection, stress/soak и power-loss tests; провести threat modeling kernel/drivers/runtimes/Mind/Hypervisor; проверить signatures, provenance, supply chain и licenses. Оптимизации сравнивать только с воспроизводимым baseline.

## Этап 7 — выпуск

Чистая воспроизводимая сборка; BIOS/UEFI и x86-64 QEMU/Limbo; ARM64 QEMU и физическое железо отдельно; ISO, images, update archive, manifests, SHA-256; Termux/Debian instructions; список проверенных функций и ограничений; независимая проверка критичных изменений.

## Правила

- Не добавлять функции в обход архитектурных контрактов.
- Не удалять существующие возможности и направления.
- Не сливать PR без проверки diff, тестов и зависимостей.
- Не закрывать PR только из-за возраста.
- Не заявлять PASS без фактического запуска.
- Исправлять найденную ошибку по первопричине и повторять тест.
- Не оставлять известные заглушки незамеченными и не объявлять их готовыми.
- После каждого логического блока обновлять docs/WORKLOG.md: SHA, файлы, команды, результаты и следующая точка.
- Перед продолжением читать AGENTS.md, целевую архитектуру, мастер-план и WORKLOG.

## Ближайшая задача

Начать с этапов 0 и 1: полный снимок репозитория и инвентаризация. Не выполнять массовые рефакторинги до фиксации baseline, зависимостей и состояния PR. Независимые потоки могут продолжать безопасную работу, уже согласованную с существующими контрактами.
