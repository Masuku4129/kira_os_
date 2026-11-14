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
| PR #152 | Draft; current head `48518fac18ba16e49d66fb641ece96ae06515811` | F-001 fix; exact-head CI run `43586203250`: x86-64 build PASS, ARM64 PASS, host-tests PASS including stack helper under ASan/UBSan, all QEMU scenarios except `vm (apps-drvman)` PASS. `apps-drvman` failed after the fifth package was installed: `drv` printed only the package count and omitted package rows. Cause unresolved; no overall PASS claimed | 
| PR #153 | Draft; head `861707fc4538d396e00066cfbfe2ec171edb8abb` | F-009 atomic PMM contiguous release; exact-head CI **43/43 success**, including `apps-drvman` |
| PR #154 | Draft; current head `e299838c074604ee9cfeb734c819f84a2a5185ae` | Combined F-001 + F-009 plus per-slot 64-bit generation tokens to close ABA/stale-free risk; exact-head workflow `43587913640` running |
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

## 6.1 Verified source remediations on unmerged Draft PRs

These are exact-head CI results, not main-branch acceptance:
- F-001/F-009: PR #154, 43/43 PASS, run `43590094470`.
- F-010: PR #155, 43/43 PASS, run `43590730265`.
- F-012: PR #157, 43/43 PASS, run `43591626068`.
- F-013/F-014: PR #158, 43/43 PASS, run `43592051859`.
- F-015: PR #159, 43/43 PASS, run `43592382535`.

- F-011/F-012: PR #160, 43/43 PASS, run `43592802619`.
- F-003: PR #162, 43/43 PASS, run `43594509620`.
- F-002: current-main fail-closed policy in PR #161; latest head `ea6572f4d1a8f28732fa1c008cea0151fcd9d00b`, run `43600660082` in progress after scenario timing/expectation corrections.
- F-008: guarded-stack-only allocation and stack zeroing in PR #164; latest head `3729347c19f8a5dad2ac5587f60876d8adca6cda`, run `43600699384` pending after correcting the test to exclude the initialized context frame.
- F-004: proposed versioned BootInfo contract is documented in ADR-0006; no implementation yet.
No PR has been merged.

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
- F-009 — PMM освобождает contiguous block постранично, без атомарной проверки всего диапазона.
- F-010 — x86-64 TLB shootdown может завершиться по таймауту без подтверждения CPU, а backing pages затем освобождаются.
- F-011 — MMIO page align-up может переполнить `pa + len + PAGE_SIZE - 1` после успешной первичной проверки диапазона.
- F-012 — длинная запись старта драйвера превышает `LB_SIZE=192` и разрывается между CPU; CI serial assertion получает перемешанный текст.
- F-013 — ARM64 PMM не отделяет зарезервированные страницы от обычных занятых и позволяет ошибочно освободить kernel/FDT/DMA reservation.
- F-014 — ARM64 PMM освобождает contiguous block постранично и допускает частичный free при ошибке середины.
- F-015 — FDT `reg` parser допускает 32-битное переполнение индекса записи и 64-битное переполнение `address + size`.

Это подтверждённые по исходникам findings, а не runtime reproduction. Тесты для них в рамках этого аудита локально не запускались.

## 8. Очередь ближайшей работы

- [x] Получить актуальный main SHA и дерево.
- [x] Получить список открытых PR и текущие head/base SHA.
- [x] Сверить архитектурную ветку #135 с main.
- [x] Проверить exact-head CI #148 и #150.
- [x] Получить host-tests log PR #150: failure is `wm_compose_par(test_present, &composed_pixels) > 4` at `tests/wm_test.c:84`; GUI failure remains open and must not be hidden by the kernel work.
- [x] Прочитать актуальные `AGENTS.md`, `README.md`, `ARCHITECTURE.md`, мастер-план и WORKLOG.
- [ ] Сверить build scripts x86-64/ARM64 с tree и status manifest PR #148.
- [x] Зарегистрировать F-011 и подготовить checked MMIO range helper + host regression; точная CI-проверка ожидается.
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
| 2026-11-14 | PR #153, head `861707fc4538d396e00066cfbfe2ec171edb8abb` | Исправлен F-009: all-or-nothing contiguous PMM free и host regressions | Exact-head CI 43/43 success |
| 2026-11-14 | PR #154, head `a3bd9a47f9a9f2f7fad09823ac50c3c90c2a01cd` | Combined integration F-001 + F-009 | Exact-head CI queued/in progress |
| 2026-11-14 | PR #152, head `29d8b5842ff5de0b93dc8da97f665496c6e507bb` | Исправление F-001: общий ownership helper, x86-64/ARM64 allocator hardening и host regression | Первый CI run выявил пропущенные include/state declarations; исправлены. Exact-head workflow `43586203250` завершён с одним unresolved QEMU failure `vm (apps-drvman)` |


| 2026-11-14 | main `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4` / Draft PR #156 | F-011: checked MMIO page-range arithmetic and overflow regression | Exact-head CI pending |


## 8. Follow-up evidence and integration dependencies — 2026-11-14

This section supersedes earlier pending/stale PR status statements above where the same PR is listed.

- **PR #165 — F-002 firmware PCI bus-master handoff:** exact head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, workflow `43603442098`, completed successfully. All 30 jobs returned by the workflow-jobs endpoint completed with conclusion `success`, including host tests, x86-64 build, ARM64 build, USB UHCI, USB xHCI, SMP, NVMe, networking and application scenarios. This resolves the earlier USB-UHCI regression on that exact head. The patch still had temporary boot/PCI tracing; those diagnostics have now been removed in cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`. **The cleanup head has no CI run/status yet and is not accepted until exact-head CI passes.**
- **PR #166 — F-016 ARM64 FDT header memory-reservation map:** exact head `9d6443ff2db7195e8abf0c6d63a6257c1c38f229`, workflow `43602565437`, completed successfully. All 30 jobs returned by the workflow-jobs endpoint completed with conclusion `success`, including host tests, x86-64 build, ARM64 build, USB UHCI, QEMU ARM64/ARM-related boot scenarios and the broader VM matrix. F-016 parser/boot integration is therefore verified on this Draft PR head, not on main.
- **Required PMM integration ordering:** PR #166 calls `pmm_arm_reserve()` after `pmm_arm_init()`. On its main-based parent, ARM64 PMM tracks only the used bitmap; `pmm_free_page()` can therefore release a page that was reserved late. Draft PR #158 adds a separate reserved bitmap, refuses to free reserved pages, makes reservation range arithmetic overflow-safe, deduplicates overlapping RAM regions and makes contiguous release atomic; its exact head `70d0e89a65414818c22d61532f7ca4288c464d2c` passed 43/43 checks (workflow `43592051859`). PR #158 and #166 touch disjoint implementation files and should be reviewed/integrated together, with the reserved-page ownership change present before accepting F-016 as a permanent PMM guarantee. The FDT host tests alone do not prove the complete FDT-to-PMM allocation invariant.
- **PR #165 cleanup:** temporary `[KIRA BOOT TRACE]` statements in `kernel.c`, PCI USB trace statements and the PCI-only UART include were removed from the branch after the successful diagnostic run. New cleanup head: `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`; workflow not yet observed. No source changes were made to main.

Stage 0 remains IN PROGRESS. No PR was merged, and no local tests or physical-hardware tests were run in this session.


### Follow-up — FDT-to-PMM integration added to PR #166 (2026-11-14)

- PR #166 was expanded beyond the original FDT parser patch to carry the verified ARM64 PMM reserved-page ownership and atomic contiguous-release implementation from PR #158.
- Added an end-to-end host regression in `tests/fdt_test.c`: parse the FDT header reservation, apply it to ARM64 PMM, prove the reserved page cannot be freed, exhaust all remaining allocatable pages and prove the reserved page is never returned.
- Added the independent `tests/pmm_arm_test.c` suite and wired it into `tests/run_host_tests.sh`; the FDT test now links the actual ARM64 PMM implementation.
- Updated PR #166 description to state the combined scope and exact verification boundary. Current head: `5846df979218bd43e0edfdd456f7042d45e8c195`; exact-head workflow `43608492629` is pending. The earlier FDT-only head `9d6443ff2db7195e8abf0c6d63a6257c1c38f229` passed 29/30 returned jobs, but that result does not transfer to the expanded head.
- PR #158 remains open as the standalone source of the PMM change; do not merge both independently or duplicate the same implementation. Review the combined #166 diff and decide which PR is the integration vehicle after its exact-head CI completes.
- PR #165 diagnostic-cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb` has workflow `43608372681` queued/in progress; the previous pre-cleanup head passed all 30 returned jobs (not 29; the latest job listing contains 30 entries). No result is claimed for the cleanup head yet.


## New source finding F-017 — firmware DMA remains enabled during PCI BAR probing (2026-11-14)

**Severity:** HIGH; DMA safety remains BLOCKED until safe handoff or IOMMU isolation is established.

### Evidence
- Current-main PCI enumeration calls `probe_bars(d)` before any firmware `PCI_CMD_MASTER` sanitization.
- `probe_bars()` temporarily clears only `PCI_CMD_IO | PCI_CMD_MEM`, writes all ones to BAR registers to measure sizes, restores BARs and restores the original command word. If firmware left `PCI_CMD_MASTER` enabled, bus mastering remains enabled throughout this probe.
- Draft PR #165 currently clears Bus Master immediately after `probe_bars()`, before the device is published to a driver. This closes inherited permission at driver handoff, but not the earlier enumeration/probe window.
- The first experiment that cleared Bus Master before BAR probing caused a reproducible `vm (usb-uhci)` boot regression. Moving the clear after BAR probing made the pre-cleanup exact-head run pass; the safe quiesce ordering is still unresolved.
- No real per-device VT-d/SMMUv3 domain exists in main. PR #161 blocks Ring 3 DMA/MSI but does not protect firmware DMA before the kernel has taken ownership.

### Required closure
1. Establish a safe pre-probe handoff: either quiesce firmware-running devices with class-aware bounded procedures before clearing Bus Master, or bring up verified IOMMU isolation before probing devices that can DMA.
2. Preserve PCI bridge forwarding while ensuring endpoint DMA is denied until an owning driver has a verified DMA domain and explicit policy grant.
3. Add tests for command-register state before/during/after BAR probing, including a controller left active by firmware; test that no endpoint DMA can reach unowned memory.
4. Re-run the full QEMU matrix including UHCI, xHCI, NVMe, VirtIO and SMP on the exact combined head; physical hardware remains a separate required qualification.

### Cross-PR dependency
- PR #165 F-002 and PR #162 F-003 both modify `pci.c`. #162 fixes nonzero ECAM root-bus enumeration and parent-bounded subordinate windows; #165 changes Bus Master handoff. Their changes must be reconciled and tested together rather than accepted independently as the final PCI baseline.
- PR #165 pre-cleanup head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, workflow `43603442098`: all 30 returned jobs succeeded. Cleanup head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`, workflow `43608372681`: queued/in progress at last check.

F-017 is OPEN. PR #165 is a partial mitigation, not full DMA-handoff acceptance.


## New source finding F-018 — FDT v16 structure-size compatibility (2026-11-14)

**Severity:** MEDIUM; boot compatibility gap, not a memory-safety bypass once malformed blobs are rejected.

### Evidence
- The main FDT parser accepted `version >= 16` but unconditionally read header word 9 (`size_dt_struct`), which is introduced by FDT v17. FDT v16 has no such header field; its structure extent is derived from the offset ordering of the structure and strings blocks.
- The parser also used this value as the structure-block bound for every token traversal. There was no v16 fixture in the host test.

### Remediation under verification
- PR #166 now derives v16 structure size as `off_dt_strings - off_dt_struct` and uses the explicit `size_dt_struct` field only for v17+.
- Added a v16 fixture with the v17-only header word cleared; tests validate the tree root, compatible string and memory-reservation map.
- Current PR #166 head after combined FDT/PMM and v16 changes: `61b124d9a912b4c5cc2106ab6aadbf28f289c9ab`. Exact-head CI was not yet visible at last check; no PASS claimed.

### Required closure
- Host FDT parser tests under ASan/UBSan and complete ARM64/QEMU matrix pass on the exact head.
- Add or confirm at least one real bootloader-produced v16 DTB fixture if support for v16 is retained; otherwise explicitly reject v16 and document v17+ as the minimum supported FDT ABI.

F-018 remains OPEN pending exact-head verification and compatibility decision.


## New finding F-019 — userland artifact manifest omitted explicit UELF outputs (2026-11-14)

**Severity:** HIGH for release completeness / project-control integrity; no kernel runtime defect implied.

### Evidence
- The three-stage status manifest listed only names collected from `for app in ...; do` loops.
- Both build scripts also append five independently linked programs directly to `UELF`: `fetch`, `net6`, `kcc`, `drvget`, `drvman`. These were absent from the manifest's prior 20-name arrays.
- x86-64 actual UELF inventory is 25 apps; ARM64 actual UELF inventory is 25 apps. The x86 Android/release profile removes `TESTPROGS` and retains 13 apps.
- The previous CI checker repeated the same loop-only assumption and could not detect loss of one of the five explicit outputs.

### Remediation under verification — PR #148
- Updated `status_manifest.json` to record all 25 x86-64 and 25 ARM64 app outputs and the 13-app x86 Android/release profile.
- Updated `ci/check_project_status.py` to combine loop-built and literal `UELF` outputs, exclude the literal loop variable `$app`, compare exact lists and check all corresponding userland source files.
- Current PR #148 head `8f6657ac793ccfc8c2755fdafdecbdebf413ec5f`; exact-head workflow `43608902362` pending. No PASS claimed.
- PR #148 remains Draft and separate from main. Do not treat the corrected registry as active protection until the PR is reviewed and integrated.

F-019 remains OPEN pending exact-head CI and review of release artifact contents.


## 9. Full baseline file inventory — 2026-11-14

Created `docs/architecture/STAGE0_FILE_BUILD_MATRIX_2026-11-14.csv` from the recursive Git tree pinned to main SHA `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`. The snapshot contains exactly **829/829 tracked blobs**; the tree API reported `truncated=false`.

### Exact production build inputs
- x86-64 kernel common C sources: **98** = 80 shared with ARM64 + 18 x86-only.
- ARM64 kernel sources: **88** = 80 shared with x86-64 + 8 ARM64-only.
- x86-64 architecture source list: **16**.
- Userland UELF outputs: **25 x86-64 + 25 ARM64**; the x86 Android/release profile retains **13** after filtering `TESTPROGS`. The corrected source-of-truth and guard are in PR #148.

### File classification and unresolved coverage
- 365 KCC host fixtures; 64 host-test script references; 10 test helpers/fixtures needing linkage review.
- 22 architecture-support files need exact assembly/linker/build linkage review; 12 other architecture-support files are explicitly referenced by build scripts.
- 60 userland files are build outputs or linked support requiring file-level linkage review.
- 129 auxiliary source/support files are not in the pinned kernel source manifests and still require semantic ownership/build-profile review.
- Remaining documentation, CI, build tools, driver package inputs and packaged resources are categorized in the CSV.

The CSV is an **inventory baseline**, not a claim that every file has passed semantic review. Every row includes a primary area, provisional component, build profile, status and classification basis. Any `*_REVIEW` status remains open until its inclusion, dependency or intentional non-production role is verified against the build graph and source references.


## 10. Preserved branch-only work — priority review queue

The pinned all-branch audit from PR #148 records **302 refs**, **205 unique non-CI tree heads**, **722 unique branch-only paths**, and **77 non-CI branches without a PR**. No branch-only path is treated as integrated merely because it exists in a branch; all remain preserved until exact changes are reviewed.

Branch-only path classes:
- 201 source/runtime paths;
- 217 tests and test fixtures;
- 164 research/prototype files;
- 74 documentation files;
- 62 CI/build/release files;
- 4 assets/other files.

High-priority source families visible in the preserved branch inventory include:
- Versioned BootInfo producers/consumers: `bootinfo.c`, `bootinfo_mb2.c`, `bootinfo_fdt.c`, `bootinfo_uefi.c` and BIOS/UEFI architecture adapters.
- IOMMU and DMA isolation: `iommu_vtd*`, `iommu_arm*`, `drivers/iommu/amd_vi*`, plus tests.
- Device Model and bus routing: `device_model.*`, `drivers/bus/*`, `acpi_prt.*`, PCI routing and resource lifecycle.
- Driver trust/signatures/revocation/state transitions: `kdrv_sig*`, `kdrv_trust.h`, `kdrv_revocation*`, `kdrv_transition*`, signing tools and tests.
- GICv3, UEFI loaders, VMM policy and KIRA Mind synchronization/build subsystems.
- Release pipeline: image audit, release gate, package update, signed artifacts and reproducibility tests.

These are not approved designs or production-ready code by presence alone. They must be mapped to their exact source branch/PR, compared against the 829-file main baseline, reviewed for API/ABI conflicts, and tested before integration. Priority order is BootInfo/boot, PMM/IOMMU/DMA, Device Model/PCI, driver trust/lifecycle, then release and higher services.


### CI failure/fix record — PR #166 FDT v16 test (2026-11-14)

- Exact head `61b124d9a912b4c5cc2106ab6aadbf28f289c9ab`, workflow `43608737160`: `host-tests` failed at `ci/lint.sh` before compilation. Lint flagged the newly added fixture string `linux,dummy-virt` as a dummy marker. This was a test-fixture naming false positive, not a compiler or runtime-test failure; host tests were not reached.
- Removed the marker from the v16 fixture assertion; no lint rules were disabled or relaxed.
- Corrected branch head: `539285048cdd97f5d71ba5aefc9afda4058b7512`. Fresh exact-head workflow had not appeared at the last check. Do not carry forward any PASS from the failed head.


### CI completion correction — PR #165 diagnostic cleanup (2026-11-14)

- PR #165 cleanup exact head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`, workflow `43608372681`: **30/30 jobs completed successfully**, zero failed jobs. This supersedes earlier queued/in-progress notes for that head.
- Temporary boot/PCI diagnostics are removed; the earlier UHCI regression did not recur on this cleanup head.
- F-017 remains OPEN: Bus Master is still cleared after BAR probing, leaving the firmware-DMA window during BAR probing unresolved. PR #162's PCI host-bus/subordinate-window changes still need reconciliation with PR #165 in `pci.c`.
- This is a Draft branch result only; no main merge and no physical hardware claim.


## New finding F-020 — IOMMU and Device Model implementations are split across incompatible branch stacks (2026-11-14)

**Severity:** CRITICAL for DMA isolation / architecture integration. Main remains fail-closed for unisolated Ring 3 DMA, but does not contain production IOMMU domains or the common Device Model.

### IOMMU branch evidence
- PRs #115, #116 and #120 contain SMMUv3 address-width, FDT `iommu-map` requester-ID and stage-1 domain work. Their successful CI runs are on base `autodrv`, not current main.
- PR #125 attempts to combine SMMUv3 controller/domain integration, per-requester mapping, DMA map/unmap, PCI bus-master gating and VT-d domain work. It is also based on `autodrv`, not main.
- PR #125 exact head `cf35a78b385c0b9939702e19b097b01a4296e273`, workflow `43207054075`: **host-tests failed**, **ARM64 build failed**, and one QEMU VM scenario failed (`usb-uhci`). Host/ARM64 compiler error: `iommu_arm64.c:351: IOMMU_E_UNSUPPORTED undeclared` (the SMMU layer exposes `KIRA_SMMU_UNSUPPORTED`). VM failure: `usb-uhci`. Other build, audit, driver-E2E, GUI and KCC lanes passed.
- The PR #125 branch's `build_clang.sh` contains `iommu_vtd.c`, `iommu.c`, `drivers/iommu/intel_vtd.c`, `drivers/iommu/vtd_domain.c`, `kdrv_sig.c` and other branch-only sources. Current main's x86 build list does not contain those IOMMU files. The ARM64 branch builder explicitly includes `drivers/iommu/arm_smmu.c`, `arm_smmu_domain.c` and `iommu_arm64.c`, but this is not current-main integration.
- PR #161 is the current-main fail-closed Ring 3 policy transplant, exact head `29a654abe16fa8ee355a330b9a90dc4d9dfb2095`, workflow `43602754339` successful. It deliberately passes `device_isolated=0` because main has no implemented per-device IOMMU domain. This is a safe restriction, not IOMMU completion.
- PR #128's ARM64 DMA coherency fail-closed change passed CI on base `v46/release-completion`, not main. Its policy and tests must be reconciled with the current FDT/PMM and SMMU design.

### Device Model branch evidence
- PR #139 introduces bounded PCI host bus enumeration and FDT nonzero `bus-range` support, stacked on PR #135's target architecture branch. Exact head `ea83f83bdc59307bba22a6bd013b24c5c5c09731` has successful runs `43408282733`, `43407402297` and `43407396416`; an earlier run failed. It is not based on main.
- PR #141 adds the common bus-neutral device registry and PCI discovery adapter, stacked on #139. Exact head `cda2ba2b3b9432f0dcae1843bc331ac858c4b6e2` has five successful workflow runs; it is not based on main.
- PR #165 is a main-based PCI firmware Bus Master mitigation, but PR #162/#139 PCI bus-range changes overlap in `pci.c`. They must be reconciled with the Device Model adapter rather than merged independently.

### Integration order
1. Preserve PR #125's source stack and resolve its compile/UHCI failures on its original base before transplanting individual modules.
2. Rebase/adapt the SMMUv3 and VT-d APIs onto current main's contracts; no branch-only success counts as current-main evidence.
3. Integrate PCI host ranges and Device Model registry before enabling per-device IOMMU policy, so requester identity and resource ownership are stable.
4. Integrate PR #161 fail-closed KDRV policy with real domain readiness; keep DMA/MSI blocked until domain setup and teardown are verified.
5. Add end-to-end tests: firmware device -> PCI/Device Model identity -> IOMMU requester/stream -> domain map/unmap -> driver lifecycle -> fault/quarantine/teardown.
6. Keep x86-64 VT-d and ARM64 SMMUv3 separate backends behind one documented DMA/IOMMU contract; require exact-head host, x86, ARM64 and QEMU checks.

F-020 remains OPEN. No SMMU/VT-d code is considered integrated into main; do not rewrite or discard the branch-only implementations.


### CI failure/fix record — PR #148 userland inventory checker (2026-11-14)

- Exact head `8f6657ac793ccfc8c2755fdafdecbdebf413ec5f`, workflow `43608902362`: host-tests failed with Python `SyntaxError: unterminated string literal` in the new `android_release_app_names()` function; the host suite did not run.
- Restored the complete TESTPROGS regex, missing guard and return expression. No test was bypassed.
- Corrected PR #148 head: `2bbcef9eb86a43919099d2f26eb8af817c4c893b`. Fresh exact-head CI has not yet appeared; do not count the failed head as verified.
