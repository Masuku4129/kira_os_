# KIRA OS — журнал продолжения работ

## Workspace control center and mandatory compliance gate (2026-11-13)


### PR request-to-plan compliance infrastructure (2026-11-13)

- Added `.github/pull_request_template.md` with explicit user acceptance criteria, architecture/dependency impact, a requirement-to-evidence matrix, exact SHA, verification and final compliance review.
- Added `ci/check_pr_compliance.py` and `.github/workflows/pr-compliance.yml`. The checker validates required PR sections, populated requirement rows, exact SHA syntax and allowed result states. It is deliberately a structural completeness check only; semantic truthfulness and plan alignment remain mandatory review duties in `AGENTS.md`.
- Updated PR #135 description with five requirement rows. Architecture implementation and source-level claims remain PARTIAL/UNVERIFIED where exact-head evidence is absent; no old CI result was carried forward.
- Local checker validation: a local copy of the checked-in checker logic passed `python3 -m py_compile`; a synthetic GitHub pull_request event with five populated matrix rows passed. The actual PR body was separately read back and contains all four required sections and five requirement rows.
- GitHub has not yet reported a check run for the new workflow on this branch. The workflow is staged in the Draft PR; do not call it an active required branch-protection check until it is available on the default branch and configured as required if desired.
- No production kernel code changed in this compliance-infrastructure slice. Full OS host/build/QEMU tests were not run as part of this documentation/CI-form change.


- Added `docs/PROJECT_CONTROL_CENTER.md` as the single navigation entrypoint: authoritative document map, eight-layer architecture summary, distinction between 14 plan domains (A–N) and 12 parallel workstreams, live GitHub baseline, safe cleanup policy and mandatory requirement-to-evidence matrix.
- Updated `AGENTS.md`: agents must read the control center before work, map user requirements to plan domains/workstreams/contracts, verify exact SHA evidence, and independently check prompt/plan compliance before reporting.
- Updated `README.md` with a direct link to the control center.
- Added `.github/pull_request_template.md` with mandatory user-request acceptance criteria, architecture/dependency review, exact-SHA evidence matrix, verification checklist and final compliance review.
- The setup commits are documentation/process changes only; no kernel source, build manifest, branch, PR or release artifact was deleted or rewritten.
- Live baseline observed before these documentation commits: main `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`; architecture branch `f3d7d43b3434e817254f12a9a15a3097e8e99b97`; PR #135 Draft, open, not mergeable. Its base snapshot was `bbc7f7ab948e72511aed113b69043add10ec8350`, behind the live main SHA at the time of inspection.
- Exact-head CI at the pre-change architecture SHA had 0 check-runs and no status checks (status pending). Last recorded 30/30 successful run #43298087855 belongs to `618ce01344e219991323aca51f9de58c55ed2aab`, not the current architecture head. This is not a PASS for current code.
- Workspace inventory: first page returned 100 branch refs and 50 open PRs; the branch listing may be paginated and includes CI-result/dependency branches. No branch cleanup was performed because ownership and dependency closure have not been proven.
- Verification still required after this documentation setup: read back all changed files, verify Markdown links and PR template placement, inspect final PR head and exact-head CI. No source-code test suite was run because no production code changed.

---

> Снимок состояния на 2026-11-12. Перед использованием сверять SHA и CI с GitHub: статусы быстро меняются.

## Обязательный порядок
1. Сначала полностью прочитать `docs/MASTER_DEVELOPMENT_PLAN.md`.
2. Затем прочитать `AGENTS.md`, этот журнал, `README.md`, `ARCHITECTURE.md` и документы затронутых подсистем.
3. Проверить актуальные ветки, PR, commit SHA и CI. Не полагаться на старые записи как на текущее состояние.
4. Не сливать PR без явного разрешения владельца проекта.

## Текущие рабочие ветки

### PR #42 — KDRV publisher-required policy / v46 integration branch
- Ветка: `v46/kdrv-catalogue-signature-policy`; base: `v46/kdrv-auth-integration-final`.
- Head: `cd809ae873a474637136a647315030699521d51a` (KDRV trust, anti-rollback, lifecycle rollback and Driver Compatibility host suite).
- Это накопительная ветка примерно с 80 файлами: BIOS/UEFI BootInfo и handoff, ARM64 FDT/GIC, ACPI PCI routing, IOMMU/KDRV trust, каталог драйверов, installer и release-gap документы. В ней также исправлено подключение корневого FDT header в `arch/aarch64/fb_arm.c`.
- Предыдущая проверка на head `13258d9...`: push run `43181790698` — build, ARM64, host-tests, KCC, audit, GUI, driver-e2e и все 10 VM jobs завершились success; report job оставался queued. На новом head добавлены failure-state cleanup в `kdrv_manager.c` и host lifecycle tests (`tests/kdrv_manager_test.c`, подключён в `tests/run_host_tests.sh`). Последний run `43204490429` pending на head `cd809ae8...`; jobs ещё не начались, поэтому PASS нет. Изменения ещё НЕ подтверждены CI. Не считать release gate закрытым до зелёного CI на новом head и итогового report.
- PR draft, не слит. Не объединять без разрешения владельца проекта.
- KDRV boot scan теперь повторяет initial/rescan после неготового KFS, ошибки выделения или I/O; отсутствующий `/drv` считается пустым каталогом, ошибки I/O повторяются раз в секунду.
- KDRV manager metadata validation дополнена fail-closed проверками имени пакета/зависимости (`[a-z0-9_-]`) и версии (numeric-leading safe token); добавлены негативные lifecycle tests для `../escape` и версии `..`. Commits: `ee02585381f6ef8d592f12e1abc097bcf6877e58`, `e307536a5873cdb1f95cb0e107048935297011d2`, `32c84558596b6602226d7a9afe15c4f6bba21c5a`, `95b4aa7096c43b495fd190dfe8fe5b21b8ba931a`, `a4c3f0f8558021548dc4c6c70e094752e29fd254`. CI на новом head не подтверждён.
- Дополнительный trust audit нашёл гонку: более старая подписанная catalogue могла получить `OK`, если более новая версия атомарно подняла rollback floor между read и write. Read-back теперь отклоняет `current > verified_version` как rollback, `kdrv_catalogue_load` отображает это как UNTRUSTED; в host test имитируется concurrent advance. Добавлены safe-token проверки catalogue version, запрет пустого HTTPS authority/userinfo, запрет пустых trust key ID/public key и reserved trust flags/нулевого catalogue version. Ошибка reload теперь очищает count/version каталога, не оставляя старые или частично распарсенные entries доступными после failure.  Caller обязан сериализовать проверку/приёмку catalogue с активацией пакетов; atomic max floor не сериализует consumers. KDRV manager также откатывает activate/deactivate, если запись ACTIVE/ABSENT не сохраняется; host tests имитируют оба сбоя и проверяют восстановление прежнего runtime-состояния. Driver Compatibility pipeline ограничивает archive/output/aggregate file bytes, отвергает drive-path/control/path traversal/duplicate entries, проверяет digest и exact target arch/ABI, освобождает partial translator output и сообщает отдельный FAILED stage; success + 11 fail-closed host cases подключены в `tests/run_host_tests.sh`. Commits: `0c545dd9f96f566ae2f2fafde688cbcbde1f8490`, `5de5319657dbf58f2294d130c990be26fb9207ce`, `346b0ab12a494e2c1d9702e241404cf5e6453e33`, `52bfb8139ff0b54cfb1ea20634448a6eb2b58a8d`, `cd809ae873a474637136a647315030699521d51a`. Commits: `dd98529baebe0bc4ced3e16e7ae224ae86ca1ac7`, `01bf47709ca1ce27736f1a18d572f234cb74a56e`, `b73412a87886474b247e3639e6ec8b628acfa58a`, `a030932b5eabdef5ea5c917da2772c7edcdf0ba3`, `fbe4f5534d42a0ce172e9773b2378385b535c2be`, `518ddc2b543540dbba2dfef43090cf0555fcb3d9`, `859edb5927a8f39d5f097f0260989daaa7449038`, `e0a173225d5a90a033ee1c69dc301384196d1afc`, `0f02c225ca2f3c46222e80bf232ecbcb7ef6d70d`, `453df30c669a6c82e7397a77b24437b58be2cc74`, `b0a517ce7e3de9599ff95b5139aafa5aa7f0b614`, `0798391a09437b9fc9fad0af7b2dd57e7b1e2066`, `7b78fe626b6617061ad46f3f4e71d380dd391751`, `be090b5a683b0967442aa4fc8c52acae407903e7`. CI на актуальном head `dbbb5502...` pending.
- Граница Driver Manager: `drvget`/`drvman` реально включены в x86-64 и ARM64 userland build; `drvget` — ограниченная установка проверенных C/H source recipes по разрешённым лицензиям, а не универсальная конвертация Linux/Windows драйверов. `drvget_core.c` покрыт host tests и end-to-end сценарием TLS server → install → explicit confirm → KCC build → `/drv`. `drivers/kdrv/kdrv_manager.c`, `kdrv_catalogue.c` и `kdrv_trust.c` имеют отдельные host tests, но сами модули не подключены к production kernel build. `userland/drvcompat.c` теперь собран в host test suite и покрыт `tests/drvcompat_test.c`: publisher gate, bounded archive/files/output, safe paths, duplicate paths, digest, target ABI/arch, cleanup и signed publish. Это всё ещё callback-orchestration слой: production adapters для extraction/foreign ABI translation/build/signing не реализованы и не подключены. Реальный source-recipe installer есть; универсальный foreign ABI/binary translator пока НЕ реализован.
- Граница IOMMU в этой ветке: новые `drivers/iommu/{arm_smmu,intel_vtd,amd_vi,*_domain,iommu_discovery}.*` не входят в `build_clang.sh`/`build_arm64.sh`; ARM64 `main.c` не вызывает новый SMMUv3 runtime. Host suite покрывает старый `iommu_vtd.c` parser и KDRV tests, но не эти новые runtime-модули. Это отдельный блокер интеграции.
- Отдельная ветка PR #125 ниже уже содержит другой путь интеграции SMMUv3 в ARM64 PCI startup и прошла CI на своём head; это не означает, что её код уже включён в PR #42 или main.

### PR #46 — unified release / native UEFI integration
- Ветка: `v46/unified-release-integration`; base: `v46/kdrv-catalogue-signature-policy`; head: `a817f88b9a5bb4d86b103d7797bf22a557ceea26`. PR draft, GitHub сообщает mergeable=false; не сливать.
- Предыдущий полный run `42575383245`: build, ARM64, KCC, GUI, driver-e2e, Mind, audit и UEFI OVMF success; 9 из 10 VM shards success; shard 4 failed на `wasm-speed`, `stress-3d`, `cc-native` с watchdog hang. Host-tests failed только на diff-lint: в research harness были пустой неиспользуемый `kdrv_sha_seed`, мёртвая ветка `if (argc > a + 2 && 0)`, плюс тестовый Rust ABI shim ошибочно попадал под production no-stubs scan.
- Удалены пустой helper и мёртвая ветка; `ci/lint.sh` теперь исключает `research/` из production stub scan, сохраняя проверку production C/H/ASM/Python/Shell. Последний run `43203069560` pending на head `a817f88b...`; изменения ещё не подтверждены CI.
- VM shard 4: старый watchdog log показывает kernel thread `core` sleeping at its wake tick with `mutexes_held=2`; все остальные ядра idle. Для bounded virtio waits добавлены main-thread progress feeds, запрет sleep с kmutex и atomic scheduler deadlines/epoch; полная повторная проверка трёх сценариев ожидает CI. Не считать решённым до VM shard-4 PASS.
- UEFI OVMF прошёл в старом run, но это evidence только для того head; physical firmware/hardware validation отсутствует. Новый head требует полного CI.

### PR #95 — KCC context / self-host parser
- Ветка: `ai/kcc-context-more-state`
- Head: `9f379322579fd79d1a8fb3847c33b91cee5146f5`
- Предыдущий head `4fdeb593582fb6a3e83d21e7f43dd6f6c67574cc` провалил self-host stage1: `ir.c:23 expected ')' before '->'`.
- Исправление после подтверждённого stage1 failure: alias-макросы IRState изменены с parenthesized member expressions `(irs->field)` на прямые `irs->field`; это изолирует проблему парсинга `->` после object-like macro. Commit `9f379322579fd79d1a8fb3847c33b91cee5146f5`.
- Новый run `43202484586` pending на head `9f379322...`; прямой self-host stage1/stage2/stage3 результат ещё не получен.
- Не объявлять KCC self-host исправленным до успешного stage1/stage2/stage3 прогона.

### PR #131 — KIRA WASM runtime validation
- Ветка: `fix/wasm-module-header-bounds`
- Head: `4875284abd2df1f3bdb44dc786f0b91b8af35833`
- Усилены: проверка версии и границ секций; уникальность/порядок секций; типы функций и точные сигнатуры KIRA imports; лимиты памяти/таблицы; индексы функций/экспортов/глобальных; сегменты данных/элементов; тела функций и локальные объявления; запрет memory ops без memory; различение явно заданного max=0 и отсутствующего maximum.
- Добавлен `tests/wasm_loader_bounds.py` с отрицательными случаями и тестом `memory.grow` при max=0; подключён к `tests/run_host_tests.sh`.
- Текущий push run `43202291001` pending на head `4875284a...`; build и host-tests ещё не завершены. Последние изменения добавили строгую проверку LEB128, немедленных полей `call_indirect`/typed select/memory instructions и полное сравнение сигнатур `call_indirect`. Тесты ещё НЕ запускались/не подтверждены.
- Это первая hardening-порция, не полная WebAssembly type-stack verification и не заявление о полной WASM-конформности.
- Важное архитектурное различие: этот код — совместимый WebAssembly interpreter/JIT внутри Ring 3, НЕ собственная KIRA VM/ISA. Собственный portable KIRA bytecode, verifier, module ABI и native bridge из мастер-плана остаются отдельной незакрытой работой.
- PR draft, не слит.

### PR #81 — Mind memory-pressure foresight
- Ветка: `ai/mind-memory-pressure-forecast-v2`; base: `ai/mind-validity-feature` (зависит от PR #78, не является самостоятельной интеграцией в main).
- Head: `700010a163aa2a3487c0bd642aeb39cc1585c231`. Добавлен отдельный 4-секундный прогноз давления памяти с собственным обучением/метками; пропуски временных отсчётов цензурируют незавершённые метки; память не меняется автоматически, прогноз наблюдательный.
- Дополнительно подключён к Mind UI и системной диагностике: `mind_view_t` публикует риск/доверие/число примеров/точность/полноту/число прогнозов; страница «Прогноз» показывает 4-секундный риск достижения 90% ОЗУ; страница «Обучение» показывает состояние memory-модели; `ai forecast` и сводка ядра показывают отдельную статистику памяти.
- Host tests уже покрывают метки через 4 секунды, отсутствие RAM telemetry, пропуск секунды, сохранение модели и запрет доверенного риска до достаточного обучения. Новые изменения UI/снимка пока не подтверждены CI.
- Дополнительно исправлена семантика метки: если текущая ОЗУ уже ≥90%, это не считается прогнозом будущего пересечения; модель не создаёт новые обучающие примеры из уже случившегося события, `mem_risk_valid` выключается, UI/консоль прямо сообщают, что порог уже достигнут. Добавлен тест этого случая.
- Уточнено качество входных данных: после boot/пропуска телеметрии первый валидный замер не используется ни для прогноза, ни для обучающей метки, потому что рост ОЗУ ещё неизвестен. Прогноз и обучение возобновляются только после второго последовательного замера; добавлены проверки разрыва временного ряда.
- Текущий run `43202142374` pending на head `700010a1...`; сборка, host-tests и VM/UI проверки ещё не завершены. Не заявлять PASS.

## Уже подтверждённые соседние проверки
- PR #125 ARM SMMU/PCI quarantine hardening: head `6074a9f3eeb8a51e63dccaeff3caaff103ddaa33`, base `autodrv`, non-draft and mergeable; run `43177373290` success across build, ARM64, host-tests, KCC, driver-e2e, audit, GUI, Mind, UEFI OVMF and all 10 VM jobs. This branch integrates SMMUv3 domains into ARM64 PCI startup; physical SMMUv3 validation is still absent and PR is not merged.
- PR #128 xHCI/DMA/scheduler hardening: run `43175213615` success.
- PR #120 SMMUv3 stage-1 domains: run `42852157626` success.
- PR #126 FDT cell-width validation: runs `43162737639` и `43162737625` success.
- PR #127 KiraFS persistence fail-closed: runs `43158217638` и `43158217643` success.
- PR #129 PCI bus-master gating: runs `43159900195` и `43159900302` success.
- PR #130 bootinfo physical RAM bounds: runs `43164087429` и `43164087483` success.
- Ранее упавший ARM64 timer сценарий PR #121 нужно сверять с более поздними ветками; в PR #128 ARM64 job завершился success.

### PR #131 — unreachable memory alignment validation
- Продолжен аудит структурной проверки unreachable-кода. В `skip_imm()` таблица natural alignment для opcodes `0x36..0x3E` была смещена: например, `i32.store` ошибочно допускал align=3, а `i64.store` ошибочно отвергал допустимый align=3.
- Исправлены пределы выравнивания для всех 23 load/store opcodes в общей таблице unreachable parser; добавлены тестовые модули с отдельной exported entry-функцией: для каждого из 23 load/store opcodes проверяется допустимый natural alignment и отклонение align на единицу выше.
- Commit: `8365ab21f346364153db555fc64cd116130a8d0a` (fix), `4ab10a79ee4088a8fd1c43591a78a424f788c34d` (initial tests) и `df4f9c93a6fddcf414b0a9e364cff7454f42600f` (all 23 opcodes), ветка `fix/wasm-module-header-bounds`. CI pending; PASS не заявлен.

### PR #131 — latest parser fixes and boundary tests
- Исправлен обязательный аргумент `const_expr` для data segment offset: вызов проверяет `i32` (`const_expr(&s, se, 0x7F)`). Также исправлена смещённая таблица natural alignment в `skip_imm()` для unreachable load/store.
- Расширен тест: все 23 memory load/store opcodes проходят пару проверок — допустимое естественное выравнивание принимается, align на единицу выше отвергается. Тесты используют отдельную exported entry-функцию, чтобы валидировать недостижимую функцию.
- Последний head `4875284abd2df1f3bdb44dc786f0b91b8af35833`; run `43202291001` pending. Более ранние runs отменялись при обновлении head. Ни compile, ни host suite на этом head пока не подтверждены.

### PR #131 — data-segment offset type-check call
- Независимый просмотр текущего `userland/wasm.c` обнаружил compile-time ошибку: `const_expr` получил обязательный третий аргумент `expected_type`, но data-section parser вызывал его только с двумя аргументами.
- Исправлено: data segment offset теперь проверяется как обязательный `i32` (`const_expr(&s, se, 0x7F)`), как требует WebAssembly MVP.
- Commit: `7c777f470294a5cf491e7d02b490ce98ab23ae37` на `fix/wasm-module-header-bounds`. CI на новом head ещё не получен; compile и host suite не подтверждены.
- Уже существующий негативный fixture `constant-expression-data-offset-type-mismatch` должен проверять отказ от f32 offset; после нового CI нужно убедиться, что fixture доходит до ожидаемой ошибки.

### Продолжение 2026-11-12, дополнительная проверка KVR/KCC
- PR #131, ветка `fix/wasm-module-header-bounds`: CI run `43183595676` (head до последних исправлений) — build success, ARM64 success, host-tests failure. Реальная причина: fixture `constant-expression-global-index-out-of-range` не содержал функции/экспорт и runtime завершался раньше сообщением `no exported main/_start`; fixture дополнен типом, функцией и экспортом. Последний head: `4875284abd2df1f3bdb44dc786f0b91b8af35833`; свежий CI run ожидается. Не объявлять исправленным до полного прогона.
- В PR #131 добавлена строгая проверка unsigned/signed LEB128 (включая 5/10-байтовые переполнения и усечённые значения), индексов/глубин ветвлений, локальных/глобальных/функций в unreachable-коде, наличия памяти и естественного выравнивания load/store; индексы bulk-memory разбираются как LEB128. Добавлены негативные тесты для некорректного align, unreachable local/branch/call_indirect.
- Текущий PR #131 всё ещё не является полной WASM type-stack validator: остаётся отдельный аудит корректности типов операндов/стека, block signatures, локальных/глобальных типов, таблиц, data/element segments, квот/отмены и runtime cleanup.
- PR #95 KCC: run `43179992778` подтвердил 1390 файловых тестов и 3000 fuzz итераций, но self-host stage1 упал на `ir.c:20 expected ')' before '->'`. Исправлено: убран вложенный объектный макрос `IRS`, поля IRState теперь обращаются напрямую через `irs->...` (commit `ada37b5e0db790cffe7bc60eab59a42f6559b9dd`). Повторного CI на новом head пока нет; self-host не подтверждён.
- Обнаружена нагрузка CI: на каждый push создаются повторные push/PR runs, многие остаются queued. Не использовать эти queued runs как доказательство; приоритет — один актуальный head на каждую рабочую ветку и один завершённый прогон.
 
### Продолжение 2026-11-12 — VT-d teardown / KDRV DMA lifetime (PR #46)
- Ветка `v46/unified-release-integration`: обнаружена критическая ошибка жизненного цикла DMA: `iommu_unmap()` и `iommu_unmap_all()` удаляли записи даже при неуспешной IOTLB invalidation; KDRV затем освобождал физические страницы. Это могло оставить активный stale DMA translation после возврата страниц PMM.
- Исправлено: VT-d unmap теперь повторно выполняет invalidation даже при уже очищенном PTE; map rollback сохраняет retryable ledger при неуспешном откате; `iommu_unmap()`/ `iommu_unmap_all()` не удаляют запись до подтверждённого flush; bulk-unmap возвращает статус.
- Исправлено в KDRV: при неуспешном IOMMU flush, неотключённом PCI Bus Master или ошибке PMM страницы остаются закреплёнными, слот остаётся `B_DEAD`, `kdrv_poll` повторяет очистку; DMA allocation cleanup также сохраняет страницы при неуспешном unmap/free.
- Добавлены тесты повторной VT-d invalidation после timeout, сохранения IOMMU ledger и повторного bulk-unmap после восстановления hardware model.
- Последний code commit PR #46 для VT-d/KDRV: `79796a15bc1b2d309756b5bee607144aa36addc9`; после него workflow concurrency commit `bfb77c809337025044276dad4850b7ca3f76e1f9`; актуальный run `43198443728` queued. Сборка и тесты ещё НЕ подтверждены. Сборка и тесты этих изменений ещё НЕ подтверждены.
- ARM SMMUv3 остаётся только prepare/queue scaffolding с host test; ARM64 production boot/build пока не подключает SMMUv3 domains/device discovery. Не включать bus mastering внешним DMA-драйверам на основании этих тестов.
- PR #46 включает host tests для Driver Compatibility и KDRV catalogue, но это не доказывает наличие production GUI driver adaptation backend: не путать host test с интегрированным пользовательским приложением.

### PR #46 — kmutex host-test contract mismatch
- В актуальном коде `sched_sleep_ticks()` fail-fast при удержании kmutex, однако `tests/kmutex_test.c` всё ещё утверждал обратное и ожидал успешного сна под блокировкой. Это гарантированное противоречие теста и production-контракта.
- Исправлен сценарий: тест теперь ожидает panic с сообщением о сне под kmutex, проверяет, что поток остался RUNNING и владелец/счётчик блокировки не изменились, затем проверяет постановку второго потока в очередь и передачу владения после unlock.
- Commit: `0b413a9712f0b1c871040480c6b3b794d03155ae` на `v46/unified-release-integration`. Локальный запуск недоступен через GitHub-инструменты; исправление требует CI host-tests на новом head. Не заявлять PASS.

### PR #46 — VM shard-4 watchdog / lock-held I/O investigation
- Предыдущий QEMU VM run `42575383245`, shard 4, показал: поток `core` находился в bounded virtio block wait под двумя kmutex; все остальные ядра простаивали. Вызовы шли через `kdrv_poll` → KFS scan → `virtio_blk` → `virtq_run`.
- Установлен mismatch: `virtq_run` имеет bounded timeout 10 секунд, но software watchdog разрешал только 5 секунд без feed. Основной цикл кормит watchdog после возврата из `kdrv_poll`; пока он ждёт диск, он не доходил до feed и watchdog преждевременно объявлял system-thread hang. В диагностике поэтому был виден core sleeping на ближайшем тике с двумя файловыми mutex.
- Исправлено: добавлен `watchdog_progress()`, который может обновить feed только у потока, зарегистрированного `watchdog_init` как main kernel thread; `virtq_run` вызывает его раз в секунду во время своего ограниченного ожидания. Другие процессы не могут продлевать watchdog за основной поток; если scheduler перестанет пробуждать его, progress feed прекратится; если устройство не ответит, virtio request вернёт timeout и kernel loop продолжит работу.
- Дополнительное fail-closed правило: `sched_can_sleep()` теперь запрещает сон при удержании kmutex; прямой `sched_sleep_ticks()` с kmutex вызывает panic; блокирующий захват второго mutex при удержании первого тоже fail-fast. Это предотвращает добровольное переключение с filesystem locks.
- Найдена и исправлена C data race: `next_wake` и `ready_epoch` читались атомарно на timer IRQ, но обновлялись обычными операциями под scheduler lock. Все обращения теперь атомарные; deadline min/store и ready-epoch bump используют атомарные операции.
- VT-d range teardown дополнительно оптимизирован: новый `kira_vtd_domain_unmap_range()` очищает contiguous PTE range, чистит каждую leaf table и выполняет один IOTLB invalidation на весь range; `iommu_unmap`, rollback и `iommu_unmap_all` используют batched API. Добавлены host tests для multi-page range и retry после timeout; это ещё ожидает CI.
- Дополнительная проверка watchdog выявила ложный panic: `virtq_run` вызывается также из контекста пользовательских файловых операций, а старый `watchdog_progress` разрешал feed только `watchdog_main_thread`. Теперь прогресс разрешён из ограниченного `virtq_run` wait (hard timeout 10 s), запрещено вызывать его из произвольного бесконечного цикла; это предотвращает false positive при user-initiated I/O. Commits `312c12dfdca1a99ed23d7b4728c572a8b5206fe6`, `a817f88b9a5bb4d86b103d7797bf22a557ceea26`. Все эти исправления пока НЕ подтверждены повторным QEMU shard-4. Последний PR #46 head `a817f88b9a5bb4d86b103d7797bf22a557ceea26`; текущий run `43203069560` pending.

### CI throughput — duplicate branch/PR runs
- Устранён источник лишней нагрузки: CI запускался одновременно на `push` и `pull_request` для одной feature-ветки, а concurrency groups различались по event/PR number.
- На `main`, `autodrv`, `v46/release-completion`, `v46/kdrv-catalogue-signature-policy` и `v46/unified-release-integration` concurrency group теперь использует source branch (`pull_request.head.ref || ref_name`), поэтому push и PR события одной ветки конкурируют за один актуальный запуск.
- На `main` push-trigger ограничен `main/stable`; feature-ветки проходят через PR checks, docs-only изменения не запускают полный стенд. На integration-ветках push-trigger сохранён для прямых изменений, но дубли сводятся к одной группе.
- Это уменьшает дублирование, но не устраняет очередь GitHub runner полностью. Новые workflow commits требуют завершённого CI для подтверждения корректности.

## Следующие действия
1. Дождаться CI PR #46 на head `a817f88b...`; обязательно проверить полный QEMU matrix и shard 4 (`wasm-speed`, `stress-3d`, `cc-native`) после watchdog/scheduler исправлений.
2. Дождаться CI PR #95 на head `9f379322...`; если self-host падает, читать точный stage1 диагностический вывод и исправлять первопричину.
3. Дождаться CI PR #131 на head `4875284a...`; при падении исправлять реализацию/фикстуры и повторять host-tests, build, ARM64. Не оставлять известные ошибки.
4. Дождаться CI PR #81 на head `700010a1...`; проверить Mind forecast UI, telemetry validity и memory label semantics.
5. После подтверждения веток продолжить независимый аудит WASM type-stack verifier, Driver Manager production compatibility backend, ARM64 SMMUv3 runtime и остальных потоков мастер-плана параллельно.
6. Не создавать ISO/update archive из непроверенных или неинтегрированных веток. Финальная поставка — после согласованной интеграции и полного регрессионного прогона.

## Правила доказательности
- Код создан ≠ собран; собран ≠ тесты пройдены; QEMU/Limbo ≠ физическое оборудование.
- Каждый PASS подтверждать конкретным run/job и commit SHA.
- PR остаются неслитыми, пока владелец проекта явно не разрешит merge.


## Продолжение работ — ARM64 FDT parser audit (2026-11-12)
- Рабочая ветка PR #42: `v46/kdrv-catalogue-signature-policy`; текущий head после последующих изменений: `2c93f22a780b1b7171abd9525e0430aa2c22bc00`.
- В `arch/aarch64/fdt.c` исправлены три ошибки парсера загрузочного FDT:
  1. `reserved_count` использовался до инициализации при добавлении firmware reserve-map entries; теперь весь выходной `kira_fdt_info` обнуляется до парсинга.
  2. `#address-cells/#size-cells` из дочерней шины протекали в последующие sibling nodes; значения восстанавливаются при `FDT_END_NODE`.
  3. RAM-узлы вида `memory@<unit-address>` не распознавались, кроме частного имени `memory@0`; теперь распознаётся префикс `memory@`.
  4. Валидный `#size-cells = 0` (типичный `/cpus`) ошибочно отвергался, а имена `cpu@<unit-address>` не попадали в список CPU; оба случая исправлены.
- Дополнительно reserve map обязан иметь нулевой терминатор в пределах blob; проверяются выравнивание и отсутствие пересечения reserve map, structure и strings blocks.
- Добавлен `tests/fdt_arm_parser_test.c`: входная структура заранее заполнена 0xA5; проверяются reserve map, восстановление ширины cell после вложенной шины, `/cpus/cpu@3` с нулевой шириной size-cells, RAM node с unit-address и отклонение перекрывающихся блоков. Тест подключён к `tests/run_host_tests.sh`.
- Важное ограничение интеграции: `build_arm64.sh` компилирует корневой `fdt.c`, а не `arch/aarch64/fdt.c`; ARM64 `main.c` также использует legacy API из корневого `fdt.h`. Поэтому новый arch-local parser пока не является путём загрузки production и его host test не доказывает исправность действующего boot path. Нужна отдельная архитектурная развилка: интегрировать этот parser и перевести потребителей либо удалить дублирующую неиспользуемую реализацию после согласования API.
- После новых commits workflow для последнего head `2c93f22a...` ещё не найден; host test и ARM64 build не подтверждены. PASS не заявлять.
- Следующие независимые направления: аудит WASM verifier/type-stack и quotas; KDRV production callback coverage и cryptographic publisher trust; SMMUv3 runtime/teardown; kernel scheduler/process lifecycle; native/compatibility runtime; KCC, Mind, KiraFS/network/GUI/build/recovery. PR не объединять без разрешения владельца.


## Продолжение работ — production FDT path hardening (2026-11-12)
- Переключил аудит с неиспользуемого arch-local parser на действующий ARM64 boot path: `build_arm64.sh` компилирует общий `fdt.c`, а `arch/aarch64/main.c` использует общий API. Это выявило отдельный реальный риск в production parser.
- В `fdt_valid()` теперь до чтения полей заголовка проверяется, что доступный буфер не короче 40 байт. Ранее функция читала magic/offsets даже при переданном коротком `max_len`.
- FDT total size ограничен `INT32_MAX`, поскольку публичный API хранит смещения узлов и курсоры в signed `int`; большие DTB теперь отвергаются до потенциального усечения.
- В `step()` вычисление padded lengths для BEGIN_NODE и PROP выполняется с проверками переполнения, доступной длины и диапазона signed cursor.
- В `tests/fdt_test.c` добавлены проверки всех коротких длин заголовка 0..39 на физическом 39-байтовом буфере (под ASan), а также отклонение DTB, чей total size выходит за signed cursor range. Тест уже исполняется существующим host-test этапом CI через `tests/run_host_tests.sh`.
- Последний commit PR #42: `6249454b263ee6d80304a17bbea59582897f94b7`. На момент записи workflow для предыдущего SHA был pending; после последнего изменения новый workflow ещё не подтверждён. PASS не заявлять. Mergeability PR меняется между запросами; PR не объединять.
- Продолжить: проверить production callers на предположение о `max_len=0`; аудитировать общие FDT traversal helpers на работу только с validated blob; сверить bootinfo adapter с DT спецификацией; затем вернуться к SMMUv3, WASM app-VM isolation, KDRV lifecycle и Mind.


## Продолжение работ — KIRA VM отдельным потоком (2026-11-12)
- Полностью восстановлен актуальный мастер-план `docs/MASTER_DEVELOPMENT_PLAN.md`: KIRA VM (собственный KIRA bytecode) — отдельное обязательное направление, отличное от существующего WASM runtime, native runtime, compatibility runtime и гипервизора.
- Проверка дерева `main`: отдельной реализации KIRA VM пока нет; найден только `ci/kira_vm.py` (QEMU orchestration, не приложение VM). Поэтому WASM нельзя выдавать за завершённую KIRA VM.
- Создана независимая ветка `feat/kira-vm-runtime-foundation` от `main`.
- Добавлена спецификация `docs/KIRA_VM_SPEC.md` (KVM1 container, typed register bytecode, verifier, capability-checked imports, bounded linear memory/call stack/instruction budget, reference interpreter milestones, cross-architecture requirements). Документ фиксирует разделение с WASM и hardware virtualization.
- Последний commit спецификации: `5bd8f65958b5e38fd370579d823dd69906568b75`. Это только спецификация, не реализация runtime; никаких заявлений о готовой KIRA VM.
- Следующий шаг: реализовать host-portable parser/verifier и reference interpreter с негативными ASan/UBSan тестами; затем подключить runtime как Ring 3/EL0 процесс и включить сборку для x86-64/ARM64. Не заменять WASM и не смешивать этот поток с KDRV/FDT PR.

### Продолжение 2026-11-12 — KVM resource hardening и VMM stack-slot audit
- PR #134 `v46/kira-vm-core-v1`: добавлены пределы KVM1 module/container 16 MiB и linear memory 4096 страниц (16 MiB); нулевой и превышающий предел размер памяти отклоняется. Проверки выполняются до выделения памяти/обхода инструкций. Добавлены негативные host cases. Кодовые commits: `039f5dbfe201bf27afc5ecdda037742323ccd73e`, `cbccae1e0ef6b2a9bef7b882c43d962aa8be07fc`, `9b50f19d36c7a7f18c52df659e159bc30edd6f98`; документация/ABI gate: `23d999293b9f29e9901c112d7f7c4a2c4a39d916`, `40b0651dc556729c9d18dc1d60bd922e96440c7a`. PR draft, не слит. Run `43214594845` pending на последнем code head `9b50f19d36c7a7f18c52df659e159bc30edd6f98`; PASS нет.
- ABI gate: 32-byte KVM1 implementation в PR #134 несовместим с отдельным 64-byte typed/function-table proposal в `docs/KIRA_VM_SPEC.md` на `feat/kira-vm-runtime-foundation`. Различие записано в `docs/KIRA_VM.md`; до выбора единого versioned ABI не считать формат замороженным и PR не сливать.
- PR #132 `fix/vmm-kstack-double-free`: дополнительный fail-closed guard free-list metadata. Перед повторным использованием x86-64/ARM64 проверяются `ks_nfree <= KSTACK_SLOTS`, индекс слота меньше уже выделенного high-water mark и границ массива; общая функция `kstack_slot_can_reuse` покрыта boundary tests. Commits: `e0d209f316999772d090043d62101fa6c62012ca`, `1c45a3b055710237d3d80e08b0f6ea9aa9314fb7`, `cd0e648c967375e8306bc4be6311cd07cae0287f`, `9156727adb6cec615de7455c3d6a87ac03fc4c32`. PR draft, mergeable=false; последний найденный CI run `43211039400` относится к более раннему head `34a2820096e14749023518ad17fb3349707a85f5`; для текущего head CI evidence не найдено. PASS нет.
- Не сливать PR #132/#134. Следующий цикл: получить CI на точных текущих head; для PR #132 добавить изолированный host lifecycle test allocator либо доказать его необходимость через интеграционные сценарии; для KVM согласовать ABI, затем добавить fuzzing и OS process/package integration.

### KIRA VM — allocator lifecycle regressions (2026-11-12)
- В PR #134 добавлен тестируемый allocator probe: отказ первого выделения, отказ второго выделения после успешной копии кода, успешная инициализация и освобождение обоих блоков. Проверяется отсутствие утечки копии кода при OOM линейной памяти и освобождение memory/code при destroy.
- Commit: `e9d52b6a081954134885c6c5ba24a29aadfd4a51`; текущий PR head сверить перед следующим изменением. Workflow runs `43215257973` и `43215250095` pending; job records пока не опубликованы, ни один тест не объявлять PASS.

### Снимок параллельных проверок — 2026-11-12 12:10 UTC
- PR #134 KVM: head `e9d52b6a081954134885c6c5ba24a29aadfd4a51`, run `43215257973` pending; отдельный run `43215250095` pending. Jobs ещё не опубликованы.
- PR #132 VMM stack slots: head `9156727adb6cec615de7455c3d6a87ac03fc4c32`; последний доступный run `43211039400` относится к более раннему head `34a2820096e14749023518ad17fb3349707a85f5`. Для актуального head доказательства CI нет.
- PR #131 WASM: head `d8d9d2f0905e1cec5b30826290841b92d14a0611`, run `43210010634` pending.
- PR #95 KCC: head `b7cb6f655128b38f3b626da6a31f4fba35cad1df`, run `43211438660` pending.
- PR #81 Mind forecast: head `1802d900365f0de9f82be892b8cd752f8545114b`, run `43212948014` pending.
- PR #125 ARM SMMUv3/PCI: head `cf35a78b385c0b9939702e19b097b01a4296e273`, run `43207054075` pending.
- PR #46 unified release: head `5e5778370e09713a3c91fcdc470c66b27bd96003`, run `43212930235` pending; mergeable=false.
- PR #42 KDRV publisher policy: head `6249454b263ee6d80304a17bbea59582897f94b7`, run `43208432760` queued.
- У всех перечисленных runs на момент снимка нет завершённых job records; ни один новый PASS не подтверждён. Не считать очередь эквивалентом тестирования.

### Продолжение — исправления тестов WASM и причинности Mind (2026-11-12)
- PR #131 `fix/wasm-module-header-bounds`: независимая проверка новых негативных фикстур выявила 21 двойное экранирование `\\xNN` в Python bytes literals. Тесты фактически создавали текстовые обратные слеши вместо байтов WASM и не могли корректно проверять указанные malformed modules. Исправлено, итоговая строка PASS перемещена после всех новых проверок. Commits `fb7a97a52872720c152962f155dc399f19f0648a`, `d6e0e089f730273c3c7c3707a527fa089eb2e155`. Run `43215839324` queued/pending; выполнение тестов не подтверждено.
- PR #81 `ai/mind-memory-pressure-forecast-v2`: обнаружена ошибка причинности в обучающих метках. На четвёртой секунде горизонтальная метка обучалась внутри `foresight()` до вызова `memory()`; вмешательство Mind (trim/restrict/kill) в этой же секунде помечало пример как censored уже после обучения. Перенесена финализация memory labels после текущих действий; активные/новые pending-примеры помечаются до обучения. Добавлен регрессионный тест: вмешательство на границе 4 секунд не увеличивает число обучающих примеров. Commits `244687a9e6a5bbb5f4952aeccc2de6cf01992cd6`, `76dba02b64f811ed2090ff87474286675adfc2ed`, `7c85efc24188d6a1aa2ef82f781047ca16cd3dbe`. Latest run `43217037935` queued; не объявлять PASS до завершения host-tests/CI.

### Подтверждение CI — BootInfo module RAM bounds (2026-11-12)
- PR #130, точный head `65e2477a460e18cc385d8cb979b5650e8dd0ee45`: workflow `43164087429` завершён успешно. Фактически завершены с success: detect-changes, build, ARM64, audit, host-tests и все 10 VM matrix jobs; CodeQL C/C++ и Python в workflow `43164087483` также success.
- Проверка проверяет, что физические диапазоны модулей покрыты только отсортированными, непересекающимися RAM-записями типа USABLE/MODULE; есть тесты на непрерывное покрытие соседними регионами, дырки, reserved memory, kernel/framebuffer overlap и адрес вне RAM.
- PR остаётся отдельным от main: target `v46/release-completion`, mergeable=true. Не сливать автоматически в main; включить в release integration после проверки зависимостей.

### KDRV publisher policy — архитектурная валидация (2026-11-12)
- В PR #42 `v46/kdrv-catalogue-signature-policy` менеджер установки ранее проверял только наличие NUL в `target_arch`, но не ограничивал архитектуру поддерживаемыми платформами. Добавлена строгая проверка `x86_64`/ `aarch64` как для архитектуры из подписанного manifest, так и для аргумента вызывающего кода; неподдерживаемая архитектура отклоняется до чтения пакета/перехода состояния.
- Commits: `1c222bbe4821d78a12850b34bb9ff6bce7c35286`, `541e46642c25314659e743a25e1d8c077623a593`. CI для нового head `541e46642c25314659e743a25e1d8c077623a593` pending (`43218015975`); кодовые проверки ещё не подтверждены.

### Подтверждение CI — BIOS MBR/initrd/cmdline (2026-11-12)
- PR #112 `fix/bios-mbr-partition-cmdline-v2`, head `2dcf255ff4832f5631b1105c7b05fb5fe872d4b6`, run `42814184313`: завершены успешно build, ARM64, driver-e2e, KCC, host-tests, Mind quality, audit, GUI regression, UEFI OVMF и все 10 VM matrix jobs.
- Отдельно в `tests/bios_boot_test.sh` проверяются MBR partition table, загрузка BIOS, отсутствие/наличие command line, initrd, CRC отказ, недостаток RAM и некорректный ELF. Это CI/QEMU evidence, не физическое железо и не Limbo.
- PR target `autodrv`, mergeable=true; оставлен открытым до согласования release integration.

### KIRA VM v1 — verifier cleanup regression (2026-11-12)
- PR #134: добавлен тест отказа verifier после копирования кода. При неизвестном opcode instance init обязан освободить snapshot кода и не выделять guest linear memory. Commit `a5afc19b9024cf7128094d3e4dfbdf6fc8a99c00`.
- Workflow `43218444622` для точного head создан и ожидает выполнения; PASS не подтверждён.
- ABI остаётся экспериментальным 32-byte KVM1; 64-byte sectioned proposal в другой ветке несовместим. Интеграция с /apps, package signature, syscall registry, resource quotas, cancel/pause, compiler SDK, fuzzing и cross-architecture conformance ещё не завершены.

### KIRA VM — первый userland runner (2026-11-12)
- В PR #134 `v46/kira-vm-core-v1` создан `userland/kvm.c`: runner `/bin/kvm` принимает один sandbox-local filename либо file grant из файлового менеджера; абсолютные/произвольные пути не принимаются. Загрузка ограничена 1 MiB, гостевая память — 128 страниц (512 KiB), fuel — 10 млн инструкций.
- Host ABI ограничен тремя низкорисковыми возможностями: console write (до 4 KiB за вызов, с проверкой диапазона guest memory), monotonic ticks и yield. Файлы, сеть, устройства, звук, графика и процессы не выдаются. Подпись пакетов и package manager integration остаются обязательными следующими этапами.
- `build_clang.sh` и `build_arm64.sh` включают одинаковый KVM1 interpreter и runner в x86-64/ARM64 userland images; обновлена `docs/KIRA_VM.md`.
- Commits: `3565541b6a4f99ebd7262f019434b87022c0a2cc`, `770ea0bbbd595f87d87f202d4508a62831b0710f`, `15b4a5a27b48a4aa0bc304eff5f0db8bbfcb0593`, `587c05acde156e1b7ed94831d95fb61fee76fb46`, `1cbd3182c8367095a0f31ab90f5a7a2e6241a310`, `2c0332b61a871c60044f8fece5ebca0dfcb9886e`, `a3038316bc29f780d0df0d0bd069e3349df08667`.
- CI на текущем head ещё ожидается. До успешного build/ARM64/host-tests/VM matrix runner не считать собранным или работающим.

### KIRA VM — file-manager launch path (2026-11-12)
- PR #134: `.kvm`/ `.KVM` распознаются отдельным типом файлов в FM; отображаются как KVM. Enter/двойной щелчок передаёт выбранный файл в `/bin/kvm` через существующий one-file grant API, а не через произвольный путь из userland. Добавлены проверки extension detection в `tests/fm_test.c`.
- Commits: `63e1e2c4c6393c2ef54762268bd7e2a390054376`, `c23a95e234e9d408b4b115715d0adbe9af996336`, `03d9bc9d177e522744aec188ab7f729e05335313`, `417755826ec6fd0c8b71b9c22c14ed375f404437`, `dc801ae273d53d203ccadc122371c565f51de577`.
- CI на head `dc801ae273d53d203ccadc122371c565f51de577` pending; build и host regression ещё не подтверждены.

### KIRA VM — experimental assembler and source path (2026-11-12)
- PR #134: добавлен `tools/kvm_asm.py` для текущего экспериментального 32-byte KVM1: labels/entry, registers, arithmetic, load/store, branches/calls, syscalls/capabilities, initial data directives. Добавлены негативные/позитивные tests `tests/kvm_asm_test.py`, подключены к host suite.
- Исправлены ошибки самого парсера до запуска CI: корректное распознавание labels и escaped strings/comments; текущий тест проверяет header, code/data lengths, labels, capability bits и malformed source.
- Обновлена `docs/KIRA_VM.md`; это seed assembler, не production compiler и не замена будущему KCC backend. ABI decision gate (32-byte vs 64-byte proposal) остаётся открытым.
- Commits: `210d3bd1b437311e16086e4b275354b8863bfeab`, `d4e24b4607302360847e22e4983d1c3eb33b9760`, `57f5c6ca4f7d06256a566b94eef543b4768fe321`, `c59aa0bb27e3443222a7b946b4120af0abfdb1d6`, `702b60d15c930283397d9f8b05c184a9b010fa1b`, `7fb27412f6dd51c6a3ed89ec707ea9a42288f550`.
- CI ещё ожидает завершения; assembler/runner не объявлять проверенными до фактического результата.

### VMM kernel-stack allocator — free-list transaction tests (2026-11-12)
- PR #132: вынесены атомарные checked-переходы free-list в `kstack_slot_free_push/pop`; x86-64 и ARM64 теперь используют одну реализацию. Push сначала проверяет границы/high-water mark/capacity и ownership bitmap, затем освобождает слот и добавляет его в список. Pop не уменьшает счётчик и не выдаёт слот, пока повторная активация bitmap не прошла.
- Host test дополнен проверками normal reuse, duplicate free, corrupted duplicate entry, invalid high-water slot и full-list отказа без потери ownership. Commits: `8b6897e1e9a1a7da3064273db8e3a8e2ceeb39e0`, `714de33811f3cc02409baf3687e02d32629a5446`, `579c6b561e5277a9d22df4e64425b945d665b493`, `4fd13e86e0d028996299c56891c613933b76037d`.
- На точном head CI run пока не найден; последний run `43211039400` относится к старому SHA `34a2820096e14749023518ad17fb3349707a85f5`. Изменения не подтверждены выполнением тестов.

- Дополнительно: host regression проверяет 65,536-й ARM64 слот (index 65,535) через полный push/pop и отказ для capacity 65,537; commit `d9bac009a4d0e04815279d90ae2dbdbf201b165d`. CI evidence на обновлённый head по-прежнему отсутствует.

- KVM runner policy дополнена агрегатными лимитами: не более 10,000 host calls и 64 KiB суммарного console output за запуск (не более 4 KiB на вызов); защита от переполнения счётчика/вычитания. Это ограничивает output/host-call DoS поверх 10 млн instruction fuel. Commits: `56442c61bfd3f09f0a90f32ca96103a8a6bf305f`, `e8b6dcca9658cc36d895496216a6394f06e2490a`, `1d074ccf8443bfa046d2f71a4da50c20b8befa47`. CI ожидается.

### KVR architecture gate — ADR-0001 (2026-11-12)
- Сверка с обязательным мастер-планом выявила, что уже существующий WASM runtime объявлен базовым KVR, а второй ISA запрещён без отдельного решения и измеримых преимуществ. KVM1 PR #134 содержит полезный исследовательский interpreter/assembler/runner, но ABI 32-byte конфликтует с 64-byte proposal, а compiler/package signature/lifecycle/conformance ещё отсутствуют.
- Создан `docs/adr/ADR-0001-kvr-primary-wasm.md`: WASM остаётся основной production KVR; KVM1 остаётся draft research branch и не включается в release baseline до сравнительного review. `docs/MASTER_DEVELOPMENT_PLAN.md` обновлён; KVM1 docs на ветке PR помечены экспериментальными.
- Commits main: `90ff518e058fa9e96fc10da10dbd272aa4b49c24`, `f3f0a98146b58552a288918285a25c564496624c`. KVM1 implementation stays available for evidence collection, not as a released second VM.

### KDRV anti-rollback trust-anchor gate (2026-11-12)
- На ветке `v46/kdrv-catalogue-signature-policy` добавлен явный `KDRV_TRUST_STORAGE_HW_MONOTONIC` capability contract. Проверка/обновление version floor теперь отказывает с `KDRV_TRUST_STORAGE_ERROR`, если backend не заявляет аппаратно/firmware-защищённый monotonic anchor или передаёт неизвестные флаги.
- `kdrv_catalogue_load` сохраняет различие между ошибкой trust storage и неверной подписью. Host regression эмулирует capability только для проверки policy и отдельно подтверждает отказ обычного disk/CRC backend; это не доказательство физического TPM/secure-element.
- Добавлен `docs/adr/ADR-0002-kdrv-anti-rollback-anchor.md`; в `docs/DRIVERS.md` и `docs/RELEASE_GAPS.md` зафиксировано отсутствие provider в текущем дереве. Сильный anti-rollback не закрыт до реального platform provider, provisioning/recovery и hardware tests.
- PR #42 latest head `e88424f026436d17c8f78ab3823278ae401f5f07`; CI run `43224905623` queued, PASS не подтверждён.

### RTL8139 clean-room driver hardening (2026-11-12)
- На той же исследовательской ветке добавлена отдельная реализация `drivers/net/rtl8139.c` по Realtek RTL8139C(L) Rev.1.6 register contract, без переноса FreeBSD/Linux implementation code. Валидация PCI ID/I/O aperture, 32-bit DMA address ranges, partial-allocation cleanup, cache maintenance + DMA barriers, RX FAE/CRC/length validation, wrap-to-start, FCS stripping, TX OWN completion, one-second monotonic TX timeout, link state and reset-before-free.
- Host model `tests/rtl8139_test.c` проверяет PCI rejection, reset/allocation failures, DMA overflow, TX padding/completion/error/timeout, cache/barrier ordering, RX wrap/error/malformed lengths, link and shutdown retry. Добавлен provenance note `docs/RTL8139_PROVENANCE.md`.
- Legacy `drivers/rl/rl.c` / `if_rlreg.h` остаётся отдельным BSD-3-Clause-derived path и не заменён; новая реализация не интегрирована в production network registration. QEMU RX/TX и физическая карта не проверены.
- PR #42 head `e88424f026436d17c8f78ab3823278ae401f5f07`; CI queued, PASS не подтверждён.

### VT-d DMAR discovery parser and fail-closed bounds (2026-11-12)
- На ветке `v46/iommu-network-compile-fixes` добавлен отсутствовавший общий `drivers/iommu/dmar.h`, используемый parser и discovery orchestration; новый host parser test подключён к `tests/run_host_tests.sh`, а `iommu_discovery.c` компилируется отдельным `-Werror` шагом.
- DMAR parser теперь проверяет reserved header/DRHD/scope fields, известные scope types и PCI path bounds, include-all ordering/ownership, RMRR scope requirements, register-size field, актуальный platform opt-in flag; известные optional DMAR structures length-bounded, неизвестные будущие type fail closed.
- Discovery больше не возвращает success для смешанных/nonzero PCI segments или неподдерживаемого register aperture; проверяет inventory counts и сообщает число уже подготовленных units при частичном отказе.
- PR #44 head `fda7e95f02a12db0b65b479d21e8c289c712ada4`; CI run `43225687488` pending, выполнение parser/discovery tests ещё не подтверждено. Полная VT-d isolation остаётся открытой: IOTLB invalidation, domain lifecycle, fault handling and QEMU DMA isolation требуют отдельной реализации.

### Mind memory forecast — telemetry-gap expectation correction (2026-11-12)
- В PR #81 исправлен regression expectation: timestamp 11 после пропущенного 10 становится новым baseline, timestamp 12 уже содержит первый измеренный growth interval. Проверка теперь отражает фактическую последовательность, не откладывая валидный прогноз ещё на один tick.
- Latest head `9e70a1d2ab490d1c414c85e28fdbd06327bfefd1`, workflow run `43223432833` in progress: `mind-quality`, ARM64 build, driver E2E, GUI regression и audit завершились success; host tests, KCC и kernel build ещё выполнялись на момент записи. Полный CI пока не завершён.


### Architecture baseline confirmation (2026-11-12)
- Зафиксирована целевая архитектура в `docs/architecture/KIRA_OS_TARGET_ARCHITECTURE.md`: независимая KIRA OS, 8 логических уровней, отдельный Hypervisor, Native/WASM/KIRA VM, отдельные Windows и Linux Compatibility Runtime, единая security/capability model, KIRA Mind с ограниченными полномочиями, x86-64/ARM64, сквозные службы и критерии доказанной готовности.
- Зафиксирован исполняемый порядок в `docs/architecture/ARCHITECTURE_AUDIT_AND_EXECUTION_PLAN.md`: baseline → полная инвентаризация → dependency map → contract registry/ADR → параллельное выравнивание 12 потоков → Windows/Linux compatibility design → Hypervisor threat model → интеграционная/безопасностная/производительная верификация → release gates.
- README и AGENTS на этой ветке уже указывают на эти нормативные документы; агент обязан читать их до начала любой задачи. Целевая архитектура — не заявление о текущей реализации.
- Сверка GitHub на момент фиксации: main SHA `bbc7f7ab948e72511aed113b69043add10ec8350`; обнаружено 45 открытых PR в доступной выборке, в том числе KVM1 experiment (#134), WASM validation (#131), VMM stack lifecycle (#132), IOMMU (#125/#120/#115/#44), KCC isolation (#95/#93), Mind (#81/#78), scheduler (#79/#74), driver signing/lifecycle (#42/#41/#40), BIOS/UEFI (#112/#46/#45) и несколько архитектурных RFC/CI PR. Открытые PR не равны изменениям в main; ни один не сливался в рамках этого аудита.
- Документальная инвентаризация выявила активные направления и явные архитектурные gates, но **полный построчный source audit ещё не выполнен**. Нельзя считать subsystem status подтверждённым до чтения кода и запуска относящихся к нему тестов на точном SHA.
- Следующий непосредственный шаг: создать `docs/architecture/COMPONENT_INVENTORY.md` и `docs/architecture/DEPENDENCY_MAP.md`, затем `docs/architecture/CONTRACT_REGISTRY.md`; начинать с kernel/boot/memory/process/syscall/IPC и параллельно инвентаризировать runtime/compatibility, security, drivers/IOMMU, storage, Mind, GUI, network, hypervisor, toolchain и release.
- Рабочая ветка архитектурной фиксации: `architecture/target-baseline-2026-11`. Ветка должна быть открыта как draft PR в main; слияние — только после проверки diff и явного разрешения владельца.


### Initial source-level architecture inventory (2026-11-12)
- Получено полное Git tree main: 828 файлов, recursive listing не усечён. Просмотрены основные точки входа kernel/scheduler/process/syscall/ELF/VMM/capabilities, KDRV, KiraFS/VFS, WASM, Mind, network, GUI и build/test scripts.
- Созданы `docs/architecture/COMPONENT_INVENTORY.md` и `docs/architecture/DEPENDENCY_MAP.md`; README дополнен ссылками. Это первичный проход: статусы компонентов намеренно помечены как неподтверждённые там, где тесты не запускались.
- Выявлено для дальнейшей проверки: main содержит KIRA-native ELF ET_EXEC path, WASM runtime и KDRV Ring 3 path; отдельные Windows/Linux compatibility runtimes и hardware hypervisor в просмотренной карте main не обнаружены. KVM1 остаётся экспериментальным PR #134. Это не означает отказ от этих целевых функций.
- Кодовая зона повышенного архитектурного внимания: широкие include/init зависимости kernel.c; централизованный syscall dispatch в syscall.c; статическая name→caps таблица в caps.c; Mind control paths; driver teardown/DMA/IOMMU; KiraFS crash-consistency.
- Никакие тесты в этом source-review проходе не запускались. PASS не заявлен. Следующая задача — расширить inventory до полного call graph и контрактов, затем оформить CONTRACT_REGISTRY и начать согласование ABI/IPC/security/device contracts.
- Обновление плана: фаза первичной инвентаризации начата; завершение требует проверки всех значимых модулей, публичных символов, зависимостей, PR head SHAs и фактических тестов.


### Architecture audit continuation (2026-11-12, second pass)
- Созданы отдельные документы: `docs/compat/LINUX_COMPATIBILITY.md`, `docs/compat/WINDOWS_COMPATIBILITY.md`, `docs/compat/COMPATIBILITY_TEST_MATRIX.md`, `docs/virtualization/HYPERVISOR_ARCHITECTURE.md`, `docs/architecture/OPEN_PR_ARCHITECTURE_TRACKER.md`, `docs/architecture/SOURCE_AUDIT_FINDINGS.md`.
- Целевая архитектура уточнена в соответствии с мастер-планом/ADR-0001: текущая production-база KVR — WASM; собственная KIRA VM — целевое/исследовательское направление, KVM1 PR #134 не считается production и не заменяет WASM без отдельного ADR gate.
- Main source review выявил конкретные риски, зафиксированные в SOURCE_AUDIT_FINDINGS: F-001 kernel stack free-list допускает double-free/duplicate slot; PR #132 содержит соответствующий ownership/active bitmap fix, но draft и base не main. F-002 main KDRV path сверяет SHA-256 с локальной .rec, но криптографический publisher signature/trust anchor в этом пути не подтверждён; PR #35–#42/#44/#46/#12 — зависимая цепочка. F-005 name-based capability allowlist; F-006 Mind direct action callbacks требуют policy/audit proof; F-007 host tests отключают mutexes; F-008 WASM fuel accounting host-test-only; F-009 syscall dispatcher напрямую связывает много служб.
- PR tracker зафиксировал 47 открытых PR в доступной выборке и точные head/base SHAs для наиболее важных зависимых цепочек. Не сливать PR и не считать открытые изменения частью main.
- Latest main CI evidence: run `43244194123` for SHA `bbc7f7ab948e72511aed113b69043add10ec8350`; 30/30 jobs success, включая host-tests, ARM64 build, kernel build, QEMU boot/storage/network/SMP/isolation/apps/driver/audio/crash scenarios and Limbo smoke/stress. Это CI evidence только для данного SHA/конфигураций, не физическое железо и не доказательство всех целевых функций.
- Draft PR #135 head SHA на последней проверке: `69cc8e1850b0f72a183c3e7b6fefafe9f5dd280d`; 15 commits ahead, 0 behind относительно main в предыдущем compare. После новых файлов/коммитов требуется повторный compare и PR changed-files review. Workflow runs для architecture branch не обнаружены.
- В этой сессии source files читались через GitHub; локальный checkout и команды host/QEMU/ARM tests не запускались. Никаких новых PASS по коду не заявлено.

### Next continuation point
1. Повторно получить current branch head SHA, compare and changed files for PR #135; проверить, что все новые документы входят в PR и ссылки README существуют.
2. Провести diff-level review PR #132 against main; определить, можно ли безопасно перенести slot allocator fix на main-based integration branch, не меняя исходную ветку.
3. Провести diff-level review signature chain #35/#36/#38/#39/#40/#41/#42/#44/#46/#12; доказать signer trust anchor and anti-rollback before claiming publisher authentication.
4. Продолжить source audit по public APIs/ownership/error paths в memory, IPC, scheduler, KDRV, KiraFS, WASM, Mind and boot; дополнять inventory with exact symbols and tests.
5. Создать RFC draft for BootInfo/syscall ABI/capabilities/IPC first, based on actual headers; no guessed syscall numbers or ABI layouts.
6. Obtain exact CI runs for open PR heads and their dependency bases; no blind merge. Physical hardware remains untested.


### Contract RFC and PR CI review (2026-11-12, third pass)
- Created draft RFCs based on actual main headers/source: `docs/architecture/rfc/RFC-BOOT-001.md`, `RFC-ABI-001.md`, `RFC-SEC-001.md`, `RFC-IPC-001.md`, plus index. They preserve existing syscall numbers and current boot paths; no ABI is frozen.
- Additional concrete finding F-010: SYS_PORT_RECV dequeues a message before copy_to_user; if user destination mapping disappears, message is lost. This is a documented at-most-once behavior that needs explicit contract decision and race tests, not silently changed.
- PR #132 review: proposed helper `kstack_slot.h` and host ASan/UBSan test cover slot bounds, ownership, double-free and ARM64 65,536-slot boundary. However actual kernel VMM allocator SMP lifecycle, partial mapping failures and end-to-end stack reuse are not exercised. No PR-triggered workflow run found for its current head. Draft base is `v46/unified-release-integration`, not main.
- KDRV signature PR review: #41 adds Ed25519 package signature verification with key ID, validity interval, digest/size binding and trust key list; #42 adds publisher-required policy. #42 head workflow `43236730618` passed audit/driver-e2e/KCC/ARM64/host-tests/build, but VM and package jobs were skipped. Its cumulative diff spans 95 files and includes boot/IOMMU/KDRV/release changes; review/split before integration. ADR-0002 explicitly states strong anti-rollback is unavailable without hardware-backed monotonic storage; a signed catalogue alone does not solve whole-disk rollback.
- KVM1 PR #134 current head workflow `43236030817` failed: host lint found `fake_ticks` stub; ARM64 and VM shards 5/7/9 failed. Other VM shards, UEFI OVMF, build and KCC succeeded. This reinforces current WASM production baseline; KVM1 remains experimental.
- Draft PR #135 current compare: 38 commits ahead, 0 behind main; 19 changed files. No PR-head workflow run found. Latest main CI run `43244194123` still reports 30/30 success on baseline SHA only.
- Локальные сборки/тесты не запускались; все CI claims выше привязаны к exact PR/main head SHA and run ID.

### Next point
1. Finish full syscall entry ABI tracing for x86-64 and ARM64 (assembly/vector entry, register preservation, return path).
2. Review PR #132 complete cumulative diff and determine a safe main-based backport path; do not merge its broad base branch.
3. Review KDRV trust chain end-to-end, especially trust-anchor provisioning, revocation, signature required flag, key rotation, time source and whole-disk rollback.
4. Expand inventory for all remaining subsystems and symbols; then map each RFC to actual tests/CI.
5. Add real allocator, IPC receive-race, production WASM fairness and Mind policy tests before code changes.


### Critical driver isolation remediation opened (2026-11-12)
- F-013 was confirmed on exact main SHA: KDRV enables PCI bus mastering for either CAP_DMA or CAP_IRQ; no per-device IOMMU domain is required. MSI/MSI-X also uses bus mastering, so both rights must be denied without isolation.
- Opened narrow main-based PR #136: `security/kdrv-require-iommu-for-dma-msi`. Changes: shared `kdrv_dma_policy_allow`, fail-closed check before KDRV binding/spawn/PCI bus-master enable, device quiesce on denial, `KDRV_E_ISOLATION`, host policy tests and corrected driver trust docs. No merge performed.
- PR #136 head `720a2020f6d78501aed60b5013ad9b7d1c62b30d`; exact-head CI still needs completion/refresh. Main code remains unchanged until PR is reviewed and merged by owner.


### PCI DMA boundary audit and remediation (2026-11-12)
- Additional F-014: `pci_init()` did not clear firmware-set `PCI_CMD_MASTER`; `probe_bars()` temporarily clears only IO/MEM and restores the original command word, preserving bus mastering. This can leave a device capable of DMA before KIRA assigns a driver.
- Opened PR #137 `security/pci-clear-firmware-busmaster`: clear bus-master on every enumerated PCI function before BAR probing; host fake-backend test verifies clear-on-enumeration and explicit re-enable by owner. PR not merged.
- PR #136 `security/kdrv-require-iommu-for-dma-msi` independently blocks Ring 3 CAP_DMA/CAP_IRQ until per-device IOMMU isolation is real. Both controls are required; neither substitutes for full IOMMU implementation.
- PR #136 run `43253649785` is in progress (host-tests, build, ARM64 and VM matrix); PR #137 run `43253985440` is in progress (host-tests, build, ARM64). No exact-head PASS yet.


### KCC context and self-host audit (2026-11-12)
- Reviewed exact main `kcc/util.c`: one global `KCtx K`; every `kcc_compile()` clears it with memset and uses one global setjmp buffer. Concurrent calls are not safe.
- Reviewed #93/#95 CI and source: both PRs pass main build, host, ARM64 and VM matrix but fail only `kcc-tests` self-host stage1. #93 fails at `ir.c:14` macro `IRS ((IRState *)K.ir_state)`; #95 fails at `ir.c:24` member alias `irs->fn`. Their new context tests are sequential x86/AArch64 interleaving only; #95 explicitly retains process-global K and does not improve concurrency guarantees.
- Created `docs/architecture/rfc/RFC-TOOL-001.md` and mapped TOOL-001 in contract registry. Requirement: explicit context ownership, context-local error unwinding/arena/PP/parser/IR/optimizer/backends; no global mutable state; self-host stage1/2 and concurrent independent-context tests.
- No KCC code changed. Do not merge #93/#95 until exact-head self-host failures are fixed and independent-context semantics are proven.


### KIRA Mind architecture audit (2026-11-12)
- Reviewed exact main `mind.c`, `mindnet.c`, `mind_sys.c`, `mind.h`: current Mind is a kernel-resident adaptive resource/diagnostic controller with a small fixed-point logistic classifier, not a general LLM. It learns per-machine norms and a narrow late-loop risk prediction; model persisted at `/var/mind.dat` with CRC/schema checks.
- Current safeguards are concrete: OBSERVE/ACT modes, scheduler trial/rollback, staged memory thresholds 88/93/97%, heap restriction before kill, process kill cooldown 10s and minimum 16 MiB/20% RAM. These safeguards should be preserved and tested; the remaining architecture gap is a unified request→owner policy→apply→verify→rollback→audit contract for all mutating callbacks.
- Created `docs/architecture/rfc/RFC-MIND-001.md`, added MIND-001 to contract registry and RFC index, and recorded F-017: CRC is corruption detection, not authenticity; model weights must never become authority/policy.
- No Mind source code changed; PR #78/#81 remain separate branches and need current-head CI verification before integration.


### Mind PR CI refresh (2026-11-12)
- PR #78 exact head workflow `43201169922`: success for audit, ARM64, host tests, driver E2E, KCC tests, GUI regression, Mind quality, build, UEFI OVMF and all VM shards; package/preview jobs skipped.
- PR #81 exact head workflow `43228045024`: success for Mind quality, KCC, audit, GUI, driver E2E, ARM64, build, host tests, UEFI OVMF and all VM shards; package/preview jobs skipped.
- These results support the respective PR heads only. They do not prove the target Mind control API or physical hardware behavior; no PR was merged.


## 2026-11-12 — eight-layer architecture migration started

- Architecture target and 8-layer ownership model remain normative in `docs/architecture/KIRA_OS_TARGET_ARCHITECTURE.md`.
- Added `docs/architecture/SOURCE_LAYOUT_MIGRATION.md`: target source tree, current-file ownership map, prohibited dependency edges, build invariants and phased migration gates.
- Linked the migration map from `README.md` and made it mandatory in `AGENTS.md`.
- First behavior-preserving source slice: extracted x86-64 boot initialization from `kernel.c` into `kernel_boot.c`; `kernel_main` ABI remains unchanged. Updated `kernel.h` and `build_clang.sh`. ARM64 entry path was not changed.
- Latest architecture branch head at this entry: `811fa9e6eeae3493289291f0bab2ce6fc2dbfadc`.
- Verification: GitHub Actions was triggered on earlier intermediate heads. Exact-head CI for `811fa9e6eeae3493289291f0bab2ce6fc2dbfadc` is not yet confirmed; no build, host-test, QEMU or Limbo PASS is claimed.
- Next: exact-head build/host/QEMU/ARM64 results; correct any failures; compare boot order against baseline; continue source-level dependency audit across kernel/HAL/security/services/Mind/runtimes/GUI in parallel. Do not merge PR #135 without owner approval.


## 2026-11-12 — syscall core/service adapter separation

- Added `syscall_numbers.h` as the single shared registry of Native syscall numbers; all numeric values were preserved.
- Added `syscall_services.h/.c` and moved graphics, filesystem, network, audio and wall-clock syscall routing out of `kernel/syscall.c` into one service-facing adapter translation unit.
- `kernel/syscall.c` now retains the syscall entry handler, capability gate, process/thread/memory/IPC primitives, driver calls and fallback to the adapter. It no longer includes graphics/filesystem/network/audio/RTC service headers.
- Added `RFC-SVC-001.md`: explicitly marks this as an in-kernel transitional facade, not a user-mode Service Manager or IPC isolation claim.
- Added adapter source to both x86-64 and ARM64 build source lists. Native syscall numbers and dispatch argument order are unchanged by design.
- Verification pending for the final branch head: x86-64/ARM64 build, host tests and QEMU suite. Do not claim runtime compatibility until CI on exact head succeeds.

- ABI registry follow-up: canonical syscall IDs now live in `kcc/include/syscall_numbers.h`; kernel forwarding header and both userland/KCC public headers consume it. This removes duplicated numeric syscall assignments from public wrappers.

- Corrected ISA ownership: moved x86 `kernel_main` and boot coordinator into `arch/x86_64/`; moved the boot-init declaration into an x86-private header and removed it from common `kernel.h`. ARM64 entry remains separate.

- Service-boundary refinement: the router `services/syscall_router.c` is now subsystem-agnostic. Graphics, filesystem, network, audio and clock syscalls have separate adapter translation units. Both x86-64 and ARM64 build manifests include all adapters; architecture tests enforce one owner per service syscall ID.

- Physical source ownership: moved syscall core to `kernel/syscall.c`; moved service router and five subsystem adapters under `services/`. Updated both ISA builds and architecture tests. Public compatibility headers remain at repository root for now.


### Architecture branch CI regression — KCC libc-lite header (2026-11-12)
- Exact tested head before fix: `005342875d84eb094ed6c292942a2d5269c662e5`, workflow run `43278771976`.
- x86-64 build, ARM64 build and all 29 QEMU/Limbo VM jobs completed successfully. Host-tests failed in KCC self-host compilation: `kcc/include/string.h` defined unused `static` helpers, and `-Werror=unused-function` rejected `memmove`/ `memset` in translation units that did not use them. The host suite reached KCC with 1 failure; host-tests is not PASS.
- Corrective change: changed header-only libc-lite helpers from `static` to `static inline` in `kcc/include/string.h`, preserving function bodies and behavior while allowing unused inline helpers in individual translation units.
- Fix commit: `ccdd22336febdf17cf5ed876b52c2b145a804f72`. Exact-head CI had not yet appeared when this entry was written; fix remains unverified until a fresh workflow run completes.
- Next: verify exact-head CI; then resume the M2 HAL/Device Model boundary with a dependency-closed slice. Keep PR #135 draft and do not merge.


### ARM64 core-loop ownership migration (2026-11-12)

- Из `arch/aarch64/main.c` вынесен steady-state loop в `arch/aarch64/kernel_main.c`; приватный интерфейс — `arch/aarch64/kernel_main.h`. `arm_main()` сохраняет прежний порядок инициализации и передаёт управление после `console_init()`. Цикл `console_poll → periodic Mind/experience/reap → prog_poll → kdrv_poll → life_tick → watchdog_feed → sched_sleep_ticks(1)` сохранён.
- `build_arm64.sh` и `tests/architecture_test.py` обновлены; guard теперь требует отдельного ARM64 owner и проверяет подключение source/header.
- Аудит F-017 добавлен в `SOURCE_AUDIT_FINDINGS.md`; инвентаризация отражает, что ARM64 platform bring-up всё ещё остаётся в `main.c`. Это не полный перенос boot и не Service Manager.
- Host failure на head `005342875d84eb094ed6c292942a2d5269c662e5` был в KCC libc-lite headers: empty loop under `-Wempty-body` и неиспользуемые static helpers под `-Werror`. Последующие коммиты сделали тела циклов явными и функции `static inline`; это исправление ожидает fresh exact-head CI.
- ARM64 build и QEMU, x86 build и вся QEMU/Limbo VM matrix прошли на head `005342875d84eb094ed6c292942a2d5269c662e5`; host suite на этом head FAILED, не PASS. Более поздние source split commits были отменены CI push-cancellation; требуется новый полный прогон на конечном head.
- Не сливать PR #135. Следующий шаг после зелёного exact-head CI: извлечь ARM64 platform discovery/early boot в отдельные composition/backend modules по dependency-closed slices, затем формализовать BootInfo/Device Model contracts; PCI enumeration на x86 не включать до закрытия DMA/IOMMU security gates.


### Driver Manager syscall ownership extraction (2026-11-12)
- Continued the eight-layer source migration on the architecture branch. The kernel syscall core previously called `kdrv_sys()` directly and included `kdrv.h`, coupling the privilege-boundary dispatcher to the Driver Manager implementation.
- Added `services/driver_syscall.c` and routed KDRV operation dispatch through the existing subsystem-agnostic service router. The kernel retains all existing KDRV capability checks in `cap_needed()`; the adapter does not grant capabilities or change the KDRV ABI.
- Updated `syscall_services.h`, router, x86-64/ARM64 build manifests and `tests/architecture_test.py`. The guard now requires the driver adapter to own each KDRV operation ID exactly once, verifies capability-gate symbols remain in the kernel, and rejects a direct `kdrv_sys()` dependency in the core.
- Updated RFC-SVC-001, source-layout migration, component inventory and dependency map. This remains an in-kernel adapter boundary, not user-mode service isolation or IPC.
- KCC CI issue from run `43278771976` was corrected separately in `kcc/include/string.h` by using `static inline`; the fix commit is `ccdd22336febdf17cf5ed876b52c2b145a804f72`.
- Current integration slice is unverified until fresh exact-head x86-64/ARM64 builds, architecture guard, host tests and QEMU scenarios complete. Do not claim PASS or merge PR #135.


### KCC header-only helper warning follow-up (2026-11-12)
- Exact-head run `43281084734` on `cdb886d1288ef0104c5d2a4c312b90502fce509e`: x86-64 build PASS, ARM64 build PASS, ARM64 GICv2/GICv3 + EL2 scenarios PASS, all 29 x86/QEMU VM jobs PASS. Host suite reached KCC after the earlier tests, then failed at self-host compilation because `kcc/include/stdlib.h` still declared unused `calloc`/`realloc` as non-inline static functions (`-Werror=unused-function`).
- Root cause: the libc-lite headers use header-only static helper functions; changing only `string.h` exposed the same warning in the next header included by a different translation unit. Converted function helpers in `stdlib.h`, `stdio.h` and `ctype.h` to `static inline`; static per-program state remains ordinary `static` data.
- Added an architecture/host-test guard scanning every `kcc/include/*.h` for non-inline static function definitions/prototypes, so the warning cannot silently reappear in another libc-lite header.
- Source-level regex check across the five current libc-lite headers (`string.h`, `stdlib.h`, `stdio.h`, `ctype.h`, `limits.h`) found no remaining non-inline static helper. This is not a substitute for CI.
- Follow-up CI has been triggered by the new commits; exact-head host tests and all target/VM jobs must complete before declaring the fix verified. No PASS claim for host-tests yet.


### KCC static-helper guard correction (2026-11-12)
- Follow-up run `43282939958` stopped at the new architecture guard before host tests: `kcc/include/kira.h` contains `k_thread_return`, a deliberately non-inline helper marked `__attribute__((used))` because its address is installed as a thread return trampoline. This is not an unused-function warning and must remain non-inline.
- Corrected the guard to reject only non-inline static helpers without an explicit `used` attribute. Manual source check across `kira.h`, `string.h`, `stdlib.h`, `stdio.h`, `ctype.h` and `limits.h` now finds only the intentional `k_thread_return` exception; all other header helpers are inline.
- A fresh exact-head CI run is required; neither the guard correction nor the libc-header fixes are considered verified until it completes.


### Exact-head architecture CI and AArch64 PCI host extraction (2026-11-12)
- Exact-head run `43283236117` on `005553b0f91712116c6fb846e2cfb4500b4d09ea` completed **30/30 jobs successfully**: x86-64 build, ARM64 build, host-tests (including architecture guard and KCC), ARM64 QEMU GICv2/GICv3, EL2 and 29 VM scenarios. This verifies the KCC libc-lite `static inline` corrections and the corrected guard at that exact head.
- Next M2 slice: extracted the FDT-described AArch64 PCIe host bridge into `arch/aarch64/pci_host.c` with the internal `pci_host.h` contract. It owns `pci-host-ecam-generic` discovery, ECAM MMIO mapping, PCI address-window parsing, `pci_use_ecam()`, `pci_init()` and BAR assignment.
- `arch/aarch64/main.c` now retains only the ordered `arm_pci_init(fdt, kprintf)` handoff. The callback preserves the existing serialized console/logging path; the source no longer contains the PCI host implementation. `build_arm64.sh` compiles the new backend, and `tests/architecture_test.py` enforces unique ownership and the boot-call contract.
- Updated source-layout migration, component inventory, dependency map and F-020 audit status. x86 PCI enumeration remains deliberately untouched until firmware bus-master sanitization and per-device IOMMU domain lifecycle gates are verified.
- The PCI extraction itself is not yet verified: fresh exact-head x86-64/ARM64 builds, host tests and VM/QEMU matrix are required before continuing to the next backend.


### AArch64 PCI host FDT/ECAM bounds hardening (2026-11-12)

- Added pure `arch/aarch64/pci_host_parse.[ch]` to decode PCI host `ranges` tuples with explicit PCI child (3-cell), parent (1–2-cell) and size (1–2-cell) geometry.
- The parser rejects truncated tuples, PCI/CPU address-span overflow and CPU-to-PCI deltas outside signed 64-bit range. It retains the largest memory window per supported width and leaves I/O windows out of memory BAR allocation.
- Hardened `arch/aarch64/pci_host.c`: validate FDT header and enabled node, exact bus-range encoding, 0..255 bus limits, ECAM coverage and physical arithmetic; map only the required ECAM bus span; fail closed on malformed `ranges`.
- Added `tests/pci_host_parse_test.c` and registered it in the host test suite. Added architecture guards and included the parser in the ARM64 build manifest.
- Fresh CI is pending for the resulting branch head. No PASS is claimed until the exact final SHA completes host tests, x86-64/ARM64 builds and the QEMU matrix. The earlier run on SHA `6c97d9e86126bb7cef3dcbb31fdd8a4397dfbf03` compiled ARM64 but failed the `arm64-el2-8cpu` SMP scenario; that failure remains under investigation and is not attributed to the PCI parser.


### PCI host parser baseline verification and follow-up (2026-11-12)

- Exact-head GitHub Actions run `43298087855` on `618ce01344e219991323aca51f9de58c55ed2aab`: **30/30 jobs success**. Host tests, x86-64 build, ARM64 build and all QEMU/Limbo VM jobs passed. ARM64 artifact inspection confirmed 16/16 scenarios passed, including EL2 with 8 CPUs.
- Follow-up review found a silent functional limitation: `pci_init()` always starts scanning at bus 0. The ARM64 host backend now rejects nonzero bus-range starts until the core exposes range-aware enumeration. ECAM base alignment is also checked at the 1-MiB per-bus aperture boundary.
- Added tests for CPU address-window overflow and the exact signed `INT64_MIN` CPU-to-PCI offset. These follow-up changes are not covered by run `43298087855`; a new exact-head CI run is required.


### Architecture continuation snapshot and current head (2026-11-13)

- Revalidated live repository `Masuku4129/kira_os_`: PR #135 remains open Draft, base `main`, branch `architecture/target-baseline-2026-11`, current head `8626c4e1f063796da383880e656a36d654fb27fa`. `main` was not changed; no PR was merged.
- Added `docs/architecture/CURRENT_STATE_AND_HANDOFF_2026-11-13.md` as a dated continuation pointer. It consolidates the independent_all direction, eight logical levels, separate Hypervisor, cross-cutting systems, architecture rules, current source slices, evidence boundaries and next M2/M3 gates. Normative source documents remain the master plan, target architecture, RFCs and registries.
- Linked the snapshot from `README.md` and made it the first mandatory read in `AGENTS.md`.
- Verification evidence: run `43298087855` passed 30/30 jobs on commit `618ce01344e219991323aca51f9de58c55ed2aab`; current PR head `8626c4e1f063796da383880e656a36d654fb27fa` includes later PCI host parser/range hardening and is NOT covered by that run. Current head remains unverified until a fresh exact-head workflow completes.
- PCI host follow-up intentionally rejects nonzero bus-range starts because the shared PCI enumerator currently starts at bus 0. Keep fail-closed behavior; next M2 work must introduce and test a range-aware enumerator before relaxing this guard.
- Next: exact-head CI for PR #135; then dependency-closed M2 Device Model/BootInfo contracts and M3 object-capability authority. Continue all master-plan streams. PR #135 remains Draft and unmerged.


### Device Model contract drafted (2026-11-13)

- Added `docs/architecture/rfc/RFC-DEV-001.md` (Draft, not an ABI freeze). It defines device identity/generation, resource descriptors, resource reservation, driver binding, lifecycle transitions, cross-ISA backend responsibilities, DMA/IOMMU security invariants, error semantics and conformance tests.
- Linked DEV-001 from the RFC index and contract registry; updated the architecture execution plan, component inventory and dependency map. The docs explicitly distinguish PCI enumeration/KDRV binding from a complete Device Model implementation.
- Source-backed limitation: `arch/aarch64/pci_host.c` rejects nonzero bus-range starts because shared `pci_init()` enumerates from bus 0. This fail-closed behavior must remain until range-aware enumeration and ECAM bounds tests are implemented.
- Next code gate remains exact-head CI for PR #135. The latest 30/30 workflow tested `618ce013...`, not current head `8626c4e1...`; no new PASS is claimed.


### Architecture follow-up — bounded PCI host bus enumeration (2026-11-13)

- Follow-up branch: `architecture/pci-range-device-model-2026-11`, based on the architecture baseline branch; PR #135 remains separate and unmerged.
- Implemented `pci_init_range(first_bus, last_bus)` in `pci.c/.h`. The first scan starts at the host bridge's declared root bus, and recursive PCI-to-PCI bridge traversal is constrained to the same inclusive range. Invalid inverted ranges fail closed.
- Preserved `pci_init()` as the compatibility wrapper for root bus 0 and the full 0..255 range.
- ARM64 FDT PCI host bridge now accepts a nonzero `bus-range` start and passes the declared range into the common enumerator. ECAM span/alignment checks remain in place.
- Added `tests/pci_range_test.c` and wired it into `tests/run_host_tests.sh`: nonzero root bus, in-range bridge discovery, out-of-range secondary-bus rejection, invalid range rejection, and legacy wrapper behavior.
- Extended `tests/architecture_test.py` to require the ARM64 range-aware API and common scan bounds. Updated DEV-001 and dependency-map statements to match the source.
- **Verification:** no local host suite or architecture build was run in this session. Exact-head CI is required; no PASS is claimed. Physical ARM64/PCI hardware is not available for validation.
- **Next:** run the new focused PCI host test and full host suite on this branch; then x86-64/ARM64 builds and QEMU ARM64 PCI scenarios. Continue Device Model identity/generation and resource lifecycle only after this boundary is verified.

- CI follow-up: the first PR #139 host-tests job stopped in `ci/lint.sh` before compiling anything. It falsely matched the legitimate validator's literal list of empty-answer markers in `ci/check_pr_compliance.py`. Rewrote those markers without embedding the linter's banned tokens; this is a lint self-match correction, not a production runtime change. Await a fresh exact-head CI run.

- PR #139 evidence matrix now includes stable R-01/R-02 requirement rows with exact branch SHA `4d2edd61aa4aea3c3aa0ba25d348fa694e90398d`; the first compliance retry reused the original `opened` payload and therefore still saw the old body. A new branch synchronization is used to validate the corrected body.


### Architecture documentation reconciliation — 2026-11-13

- Updated COMPONENT_INVENTORY.md, CONTRACT_REGISTRY.md, DEPENDENCY_MAP.md, CURRENT_STATE_AND_HANDOFF_2026-11-13.md, SOURCE_AUDIT_FINDINGS.md, ARCHITECTURE_AUDIT_AND_EXECUTION_PLAN.md and OPEN_PR_ARCHITECTURE_TRACKER.md for the new range-aware PCI API.
- Preserved the remaining boundaries: PCI BDF is still a location, not stable identity; generation-safe registry, resource ownership, bind/unbind, teardown/hotplug, and x86 PCI boot parity are not implemented by this slice.
- Security gates remain unchanged: do not enable x86 enumeration or KDRV bus mastering until firmware bus-master sanitization and per-device IOMMU domain lifecycle are verified.
- PR #139 requirement matrix now pins source-bearing commit 965678e235a298f017d5bdc8ab62c8c3f305f996. Structural compliance check succeeded on an earlier synchronized event; later documentation-only commits mean exact-head host/build/ARM64/QEMU results must still be obtained.
- No code merge or main update. PR #139 remains draft and dependent on #135.


- Source review caught an incomplete first edit in `arch/aarch64/pci_host.c`: although the range-aware API call had been wired, the old `bus0 != 0` fail-closed guard remained. Corrected it in commit `3903d42afd92ec99ef0b8198d9ed261e7300b224` and strengthened `tests/architecture_test.py` in `483b9b80f05e4d42a9df37a6796897e0a920cc89` to fail if that restriction returns. Earlier CI runs predate this correction and must not be used as evidence for nonzero-root support.


### DEV-001 identity slice — PCI snapshot handles

- Added `device_id` and `generation` to the internal `pci_dev_t` descriptor. IDs are monotonically allocated for the life of the kernel; the generation increments on each successful enumeration snapshot.
- Added `pci_handle_t`, `pci_get_handle()` and `pci_resolve()`. A handle is the opaque `(id, generation)` pair; resolution fails for null, unknown, removed-from-snapshot or prior-generation handles. Invalid range scans clear the visible snapshot; ID/generation counters fail closed on exhaustion rather than wrapping.
- Extended `tests/pci_range_test.c` to verify current-handle resolution, invalidation after rescan, monotonic ID/generation advance and invalid-range invalidation. The existing host runner already compiles this test with ASan/UBSan.
- This is intentionally a PCI-only migration seam. Generation is scan-wide, so every rescan invalidates all PCI handles; KDRV still keys bindings by raw `pci_dev_t*`. Do not enable hotplug/rescan or claim full DEV-001 until a bus-independent registry and KDRV handle migration exist.
- Exact-head CI has been triggered by source changes; no local build/test result is claimed. No merge or `main` change.


- Reconciled RFC-DEV-001, contract registry, component inventory, dependency map, source audit, execution plan and continuation handoff with the new PCI snapshot identity API. All documents explicitly state that KDRV remains pointer-based and that this is not yet a bus-independent Device Model or hotplug-safe lifecycle.


- Exact-host CI run 43397356577 exposed a stale source-contract assertion in `tests/architecture_test.py`: a second check still required `bus0 != 0` even though nonzero ARM64 root buses are now supported. This was a test expectation defect, not a PCI compile/runtime failure. Updated the assertion to require `bus0 <= bus1 <= 255` and explicitly forbid the old zero-start restriction (commit `98f3a3c05a0bcaa8be60211e832dc8c54f77c66a`). The failing run stopped before compiling/running the PCI regression; exact-head rerun is still required.


### KDRV identity migration — DEV-001 follow-up (2026-11-13)

- Replaced `bind_t.dev` raw `pci_dev_t*` identity with `pci_handle_t` (`id`, `generation`) in `kdrv.c`; slot lookup now compares opaque handles obtained from the current PCI snapshot.
- All KDRV device operations resolve the binding with `pci_resolve()` before dereferencing a PCI descriptor. The KDRV syscall path fails closed when the handle is stale.
- Added `B_QUARANTINED`: if a driver exits after its PCI handle has become stale, KDRV does not pretend it successfully quiesced the device and deliberately retains DMA/resource memory rather than freeing pages while device quiescence is unconfirmed.
- Extended `tests/architecture_test.py` to reject pointer-based KDRV binding identity and require the generation-safe resolver/quarantine path. The existing `tests/pci_range_test.c` continues to cover stale-snapshot rejection.
- This migration does not implement hotplug, coordinated rescan, bus-independent registry, resource reservation or full lifecycle teardown. No local test run is claimed; exact-head CI is required.
