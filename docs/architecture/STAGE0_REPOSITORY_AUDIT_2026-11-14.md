# KIRA OS — Stage 0: repository baseline and audit control

**Статус:** IN PROGRESS — первичная сверка GitHub выполнена; полный source-level аудит и проверка исполняемых тестов ещё не завершены.  
**Дата снимка:** 2026-11-14  
**Базовая ветка:** `main`  
**Базовый commit:** `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Ветка документации:** `architecture/stage0-repository-audit-2026-11-14`

Этот документ фиксирует исходную точку аудита. Он не объявляет подсистемы готовыми и не заменяет `docs/MASTER_DEVELOPMENT_PLAN.md`, `ARCHITECTURE.md`, журнал работ или отдельные RFC.

## 1. Неизменяемые правила проекта

1. KIRA OS остаётся независимой ОС; ни одно направление мастер-плана A–N или 12 рабочих потоков не исключается.
2. Не добавлять заглушки, пустые функции, фиктивную production-логику или mock вместо реальной реализации.
3. Наличие файла, символа, компиляции или тестовой модели само по себе не доказывает работоспособность функции.
4. Любой PASS привязывать к точному commit SHA, команде/CI job, среде и фактическому результату.
5. Отдельно учитывать host tests, сборку, QEMU, Limbo и физическое оборудование.
6. Не удалять исходники, файлы, ветки и PR без проверки зависимостей и явного разрешения.
7. Не менять `main` напрямую и не сливать PR без отдельного разрешения владельца.
8. Все решения, контракты, отклонения, результаты и остаточные риски записывать в GitHub.

## 2. Подтверждённое состояние GitHub на момент снимка

| Объект | Наблюдение | Значение |
|---|---|---|
| `main` | `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4` | Единственная выбранная база текущего аудита |
| Git tree `main` | 829 файлов, 32 tree entries | Снимок состава; не размер ISO и не доказательство полноты релизного образа |
| Открытые PR | 61 | Все PR/ветки должны быть учтены до интеграции |
| `docs/architecture/` в main | отсутствует | Нормативный архитектурный пакет пока не принят в main |
| PR #135 | Draft; head `78a76003d0d808147f994c7ed23098c607039fd8`; 300 commits ahead / 3 behind относительно текущего main | Не использовать как базу новой разработки и не сливать вслепую |
| PR #148 | Draft; head `2476dc6d50e6616e9ca7ce6ef469d45c10816081`; 77 commits ahead / 0 behind | Содержит трёхсекционный статусный реестр и file-loss guard; exact-head CI: 43/43 checks success |
| PR #150 | Draft; head `79e956f591e83628b7a014b0958fecff00b4ce70` | Exact-head CI: 42/43 checks success; `host-tests` failed in `tests/wm_test.c:84`: `wm_compose_par(test_present, &composed_pixels) > 4` was false. This is a GUI compositor regression, not a kernel-stack test failure |
| CI `main` | 6 check-runs `inspect` = success; commit status API: 0 statuses, pending | Это не подтверждает build, host tests или VM matrix |

Ссылки:
- [PR #135 — целевая архитектура и source audit](https://github.com/Masuku4129/kira_os_/pull/135)
- [PR #148 — статусный реестр и защита состава проекта](https://github.com/Masuku4129/kira_os_/pull/148)
- [PR #150 — Spatial UI](https://github.com/Masuku4129/kira_os_/pull/150)
- [CI run PR #150 с упавшим host-tests](https://github.com/Masuku4129/kira_os_/actions/runs/43584536752)

### 2.1 Архитектурные документы и расхождение веток

PR #135 содержит целевую архитектуру, source audit, component inventory, dependency map, contract registry и RFC. Однако его ветка существенно разошлась с текущим main. Документы и исходники из этой ветки считаются сохранёнными branch-only материалами, а не текущим состоянием main. Переносить их можно только после пофайловой проверки diff, истории и зависимостей.

### 2.2 Существующий статусный реестр

PR #148 уже содержит модель:
- `01_READY.md` — файлы текущего baseline;
- `02_IN_PROGRESS.md` — сохранённые ветки/PR и неинтегрированные изменения;
- `03_PLANNED.md` — направления мастер-плана без завершённой реализации;
- manifest и branch inventory;
- `ci/check_project_status.py` — контроль состава baseline и build inputs.

Это полезная отдельная работа. Её точный head прошёл 43/43 CI checks, включая build, host-tests, ARM64 и VM jobs. PR остаётся Draft и не объединён. Перед принятием надо независимо проверить сам guard, актуальность снимка всех веток/PR и его поведение при намеренных переименованиях/удалениях.

## 3. Фактическая структура main — первичные наблюдения

В текущем main обнаружены отдельные каталоги `arch/x86_64/` и `arch/aarch64/`, но каталоги `kernel/`, `services/` и `docs/architecture/` отсутствуют.

Основные системные модули ядра пока находятся в корне дерева, среди них:
- память: `pmm.c/.h`, `vmm.c/.h`, `heap.c`, `ramext.c`;
- процессы и планирование: `proc.c/.h`, `sched.c/.h`, `percpu.c`, `futex.c`;
- системные вызовы и полномочия: `syscall.c/.h`, `caps.c/.h`;
- устройства и драйверы: `pci.c/.h`, `kdrv.c/.h`, `kdrv_fmt.c`, `kdrv_format.h`, `drivers/rl/`;
- архитектурные backend-ы: `arch/x86_64/`, `arch/aarch64/`.

Это описание расположения файлов, а не вывод о качестве или полноте реализации. В PR #135 предложено разделение `kernel/` и `services/`; оно ещё не интегрировано в main.

## 4. Полный объём, который нельзя потерять

Мастер-план остаётся источником требований. Предварительная карта всех областей и потоков: [STAGE0_COMPONENT_MATRIX_2026-11-14.md](STAGE0_COMPONENT_MATRIX_2026-11-14.md).
- 14 областей A–N;
- 12 параллельных рабочих потоков;
- критерии выпуска ISO и обновления;
- три обязательных статуса: baseline, in progress, planned — с дополнительным UNVERIFIED для неподтверждённых функций.

Аудит не должен сводиться только к ядру или текущему блокеру. Мы идём снизу вверх по зависимостям, но сохраняем все 14 областей и 12 потоков в общей карте.

## 5. Порядок Stage 0 и последующих нижних уровней

Stage 0 — инвентаризация всего репозитория и доказательств:
1. Сверить все обязательные документы с текущим main и сохранёнными PR.
2. Составить точный список файлов, модулей, build inputs, тестов и владельцев.
3. Для каждого компонента указать статус: BASELINE / PARTIAL / EXPERIMENTAL / TEST-ONLY / BRANCH-ONLY / PLANNED / UNVERIFIED.
4. Связать каждый компонент с областями A–N, потоками 1–12, зависимостями и PR.
5. Выявить противоречия документации и кода, дубли, обходы границ, пропущенные build inputs и неучтённые файлы.
6. Зафиксировать команды и результаты проверок на точных SHA. Непроведённые проверки явно оставить UNVERIFIED.

После Stage 0 реализация и приёмка идут снизу вверх:
1. Firmware / platform handoff / BootInfo / boot path.
2. HAL, CPU bring-up, exceptions, IRQ, timers и SMP backends.
3. Device Model, ownership ресурсов, PCI/USB discovery, DMA/IOMMU и lifecycle драйверов.
4. PMM/VMM, address-space lifecycle, W^X/NX, TLB и безопасный user access.
5. Kernel objects, processes/threads, scheduler, syscalls, IPC и synchronization.
6. Capabilities, identity, trust, policy, signatures и security audit.
7. Storage/block/VFS/KiraFS, networking и системные службы.
8. Native ABI, WASM/KVR, KCC и далее отдельные compatibility runtimes.
9. KIRA Mind — телеметрия и ограниченные policy-mediated действия.
10. Display/graphics/input/audio и пользовательская среда.
11. KIRA Hypervisor как отдельный контур.
12. Installer/update/recovery, полная матрица x86-64/ARM64 и выпуск.

Это порядок зависимостей, а не разрешение пропускать параллельные направления. Верхние уровни не объявляются завершёнными, пока их нижние контракты и критерии приёмки не подтверждены.

## 6. Карточка аудита каждого компонента

Для каждого компонента будут записаны:
- назначение и требования мастер-плана;
- исходники, публичные символы, build inclusion и владельцы;
- входящие/исходящие зависимости;
- ABI/API, версия, привилегии и границы памяти/устройств;
- статус x86-64 и ARM64 отдельно;
- production / prototype / test-only / legacy;
- positive, negative, stress, integration, fuzz и security tests;
- точные SHA, команды, CI links и результаты;
- дефекты, ограничения, риски и следующие критерии приёмки.

**Запрещено:** ставить READY только по наличию исходника, имени функции, комментария, успешной компиляции или наличию теста без фактического запуска.

## 7. Source-level findings

Подтверждённые первичные findings вынесены в [STAGE0_SOURCE_FINDINGS_2026-11-14.md](STAGE0_SOURCE_FINDINGS_2026-11-14.md):
- F-001 — небезопасное владение kernel stack slots на x86-64 и ARM64; дополнительно выявлен зависающий partial-map slot на x86-64.
- F-002 — Ring 3 DMA/MSI может включить PCI bus mastering без per-device IOMMU isolation.
- F-003 — PCI ECAM bus range сохраняется, но root enumeration начинается с bus 0.
- F-004 — unified BootInfo contract отсутствует в текущем main.
- F-005 — generic bus-independent Device Model lifecycle отсутствует в текущем main.
- F-006 — `ARCHITECTURE.md` содержит устаревшие утверждения, противоречащие текущему коду и CI.
- F-007 — `build_clang.sh --android` исключает тестовые/учебные программы из полного состава образа.
- F-008 — scheduler использует heap fallback без guard page и не очищает стек перед использованием.
- F-006 — `ARCHITECTURE.md` содержит исторические утверждения, противоречащие текущему коду/CI (SMP, USB, NVMe, QEMU).
- F-007 — `build_clang.sh --android` включает release filter и исключает тестовые/учебные программы из ISO.

Это подтверждённые по исходникам findings, а не runtime reproduction. Тесты для них в рамках этого аудита локально не запускались.

## 8. Очередь ближайшей работы

- [x] Получить актуальный main SHA и дерево.
- [x] Получить список открытых PR и текущие head/base SHA.
- [x] Сверить архитектурную ветку #135 с main.
- [x] Проверить exact-head CI #148 и #150.
- [x] Получить host-tests log PR #150: failure is `wm_compose_par(test_present, &composed_pixels) > 4` at `tests/wm_test.c:84`; GUI failure remains open and must not be hidden by the kernel work.
- [x] Прочитать актуальные `AGENTS.md`, `README.md`, `ARCHITECTURE.md`, мастер-план и WORKLOG.
- [ ] Сверить build scripts x86-64/ARM64 с tree и status manifest PR #148.
- [x] Составить предварительную компонентную матрицу по всем A–N и 12 потокам; полный file-to-component/build-profile mapping ещё не завершён.
- [x] Выполнить первичный source-level проход по kernel stacks, DMA/IOMMU, PCI enumeration, boot handoff и Device Model; зарегистрировать F-001…F-005. Полный аудит всех нижних уровней ещё не завершён.
- [ ] Сформировать отдельный набор RFC/ADR для незафиксированных контрактов.

## 9. История изменений

| Дата | Commit / источник | Изменение | Проверка |
|---|---|---|---|
| 2026-11-14 | main `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4` | Зафиксирован исходный baseline Stage 0 | GitHub tree/API read; исполняемые тесты в этой записи не запускались |
| 2026-11-14 | audit branch `cd12b7c31dccf97c1bd6d5ebf001bd0f3b9dac0a` | Добавлены F-001…F-005 по исходникам | Source review на pinned main SHA; runtime reproduction не выполнялся |
| 2026-11-14 | audit branch `9d6616b21f06418559f23d4a0b60130acbd9ffaf` | Добавлены F-006/F-007: docs drift и release profile | Сверены source/build/CI; runtime tests не запускались |
| 2026-11-14 | audit branch `d3c046b0ef2c7507e554d1eda6dc3e67e8cfec4e` | Добавлен F-008: heap fallback/stack initialization | Source review на pinned main SHA |
| 2026-11-14 | audit branch `a82791d821e7f8d5f0e8409a6aa616e9b01d023b` | Добавлена предварительная матрица 14 областей и 12 потоков | Inventory only; no READY statuses claimed |
| 2026-11-14 | PR #152, head `29d8b5842ff5de0b93dc8da97f665496c6e507bb` | Исправление F-001: общий ownership helper, x86-64/ARM64 allocator hardening и host regression | Первый CI run выявил пропущенные include/state declarations; исправлены. Exact-head workflow `43586072209` выполняется |
