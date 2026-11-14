# KIRA OS — журнал продолжения работ

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

### Утверждение визуального направления KIRA Spatial Minimal (2026-11-12)
- Пользователь утвердил минималистичную пространственную концепцию рабочего стола: свободное центральное поле, компактная навигация и Dock, быстрый ненавязчивый вызов KIRA AI.
- Отвергнуты постоянно открытые системные панели, графики CPU/RAM/диска на рабочем столе, перегруженные меню и традиционная модель Windows-подобного desktop.
- Добавлен полный дизайн-контракт `docs/KIRA_DESIGN_SYSTEM.md`; раздел J мастер-плана дополнен подпунктом J.1 и этапами UI-0…UI-5.
- Зафиксировано как спецификация/направление, не как реализованная функция. Следующий GUI шаг: UI-0 — инвентаризация текущих compositor/window manager, Dock, HUD, launcher и GUI regression tests; затем выбрать минимальный безопасный вертикальный срез реализации без переписывания работающих частей.
- Коммиты на main: design doc `45a816233490254eb70973ae71ceabc102142aaa`, master plan `143cc1e919bf7340644797c1c053d83fbaa20d55`. Тесты не запускались: изменена только документация.


## Продолжение работ — Stage 0 repository audit (2026-11-14)

- Audit baseline: `main` SHA `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`; Git tree: 829 blobs; 61 open PRs.
- Открыт Draft PR #151: [Stage 0 repository audit](https://github.com/Masuku4129/kira_os_/pull/151). Документы: `docs/architecture/STAGE0_REPOSITORY_AUDIT_2026-11-14.md` и `STAGE0_SOURCE_FINDINGS_2026-11-14.md`.
- Зафиксированы source findings F-001…F-005: kernel stack ownership на обеих ISA; Ring 3 DMA/MSI без IOMMU domain; PCI ECAM enumeration всегда с bus 0; отсутствие общего BootInfo contract в main; отсутствие bus-independent Device Model lifecycle.
- F-001 remediation начата отдельным Draft PR #152, основанным непосредственно на main: общий `kstack_slot.h`, проверки exact-base/active ownership, bounded free list, ARM64 allocator metadata вынесена из stack memory, host regression включён в `tests/run_host_tests.sh`. Текущий head `8358d14850ff48f0a710486106a550e01701f2a5`; workflow `43585911384` запущен. До exact-head CI результата PASS не заявлять.
- PR #132 содержит похожую работу, но основан на `v46/unified-release-integration`; его нельзя переносить целиком без reconciliation. Использован как reference, а исправление сделано на свежей main-based ветке.
- DMA/MSI security gate остаётся открытым: PR #136 и #137 имеют новые head SHA без exact-head CI; реальная IOMMU implementation отсутствует в main.
- PCI range/device registry work в PR #139/#141 зависит от архитектурной ветки #135 и не интегрирован в main; PR #139 exact head имеет один упавший VM job.
- PR #150 exact-head host-tests failure разобран по job log: `tests/wm_test.c:84`, assertion `wm_compose_par(test_present, &composed_pixels) > 4`; это compositor regression, не причина kernel stack finding.
- Текущая сессия использовала GitHub source/API review; локальный checkout и локальный запуск тестов отсутствовали. Никакой PR не объединён, main не изменён.
- Next: дождаться exact-head CI PR #152; исправить найденные регрессии без ослабления тестов; продолжить Stage 0 matrix для всех 14 областей A–N и 12 streams, затем закрывать нижние зависимости по очереди.


### Stage 0 — documentation drift and release profile (2026-11-14)

- Добавлены F-006/F-007 в source findings. `ARCHITECTURE.md` содержит устаревшие ранние утверждения (один CPU, нет USB/NVMe, нет QEMU), хотя текущий main содержит соответствующие реализации/драйверы и CI scenarios. Документ нужно явно отделить как исторический журнал либо обновить его верхний verified summary.
- `build_clang.sh --android` включает `RELEASE=1` и исключает `TESTPROGS` из `UELF`; это не полный тестовый/учебный образ. Для полного Limbo ISO нужно явно выбрать full profile и проверять manifest содержимого.
- PR #152 первый exact-head workflow `43585911384` выявил две ошибки интеграции: x86-64 не включил `kstack_slot.h`; ARM64 замена allocator state не попала в declaration block. Ошибки исправлены отдельными коммитами.
- Последний зафиксированный head PR #152: `29d8b5842ff5de0b93dc8da97f665496c6e507bb`; exact-head workflow `43586072209` выполняется. До завершения build, ARM64 и host-tests PASS не заявлять.


### Stage 0 — component matrix and additional stack finding (2026-11-14)

- Создан `docs/architecture/STAGE0_COMPONENT_MATRIX_2026-11-14.md`: предварительная карта всех 14 областей A–N и 12 потоков. Никакая область не объявлена READY; это inventory, а не acceptance report.
- Добавлен F-008: `sched.c` fallback-ит на `kmalloc`, когда guarded VMM stack недоступен, а `create_thread()` записывает только canary и не очищает остальную память стека. Политика исчерпания stack window и очистка перед публикацией потока требуют отдельного решения/тестов.
- Проверена точная CI matrix main: QEMU scenarios включают `boot-nvme`, `boot-8cpu`, `soak-8cpu`, `smp`, `usb-xhci`, `usb-uhci`, `net-e1000`, `net-virtio`, `net-ipv6` и другие. Это опровергает устаревшие ранние summary statements в `ARCHITECTURE.md`, но не является доказательством поддержки физического железа.
- PR #152 exact-head run `43586072209` на SHA `29d8b5842ff5de0b93dc8da97f665496c6e507bb` выявил ARM64 предупреждение `va` may be uninitialized; исправлено на head `48518fac18ba16e49d66fb641ece96ae06515811`. Exact-head run `43586203250` для этого SHA выполняется; PASS ещё не заявлен.


### Stage 0 — PMM contiguous ownership finding (2026-11-14)

- Добавлен F-009: `pmm_free_contiguous()` ранее вызывал `pmm_free_page()` отдельно для каждой страницы; диапазон мог частично освобождаться, а другие CPU могли получить страницы до завершения всей операции.
- Подтверждены callers в KDRV DMA cleanup/rollback, VirtIO, USB DMA и storage/controller paths.
- Открыт Draft PR #153 на свежей main-based ветке: один lock, предварительная проверка всех страниц, all-or-nothing bitmap transition, обновление счётчиков один раз; `tests/pmm_test.c` покрывает invalid middle page, no partial free, double release, zero/unaligned/out-of-range.
- Head PR #153: `861707fc4538d396e00066cfbfe2ec171edb8abb`; exact-head CI ожидается. Локальные тесты не запускались; PASS не заявлять до CI.


### PR #152 — exact-head CI result (2026-11-14)

- Exact head `48518fac18ba16e49d66fb641ece96ae06515811`, run `43586203250`: x86-64 build PASS; ARM64 build/scenario PASS; host-tests PASS; shared `kstack_slot_test` PASS under ASan/UBSan.
- QEMU passed SMP, 8-CPU boot/soak, stress, NVMe, VirtIO/SATA/ATA, USB UHCI, network, WASM, isolation and other matrix scenarios.
- One QEMU job `vm (apps-drvman)` FAILED. Downloaded artifact `vm-apps-drvman` shows the shell command `drv` prints `пакеты в /drv: 5` but then emits no package rows. The pinned `kdrv.c::show()` source should iterate those rows. Root cause is unresolved; do not label it pre-existing or attribute it to F-001 without a baseline comparison.
- PR #152 is therefore NOT fully green. F-001 implementation is present and core stack tests/stress pass, but integration acceptance is blocked until the `apps-drvman` scenario is explained and passes (or an independently proven test defect is corrected without weakening coverage).


### Сравнительная интеграция F-001 + F-009 (2026-11-14)

- PR #153 (только PMM F-009), head `861707fc4538d396e00066cfbfe2ec171edb8abb`, exact-head run `43586614175`: **43/43 checks success**. В том числе `vm (apps-drvman)` PASS; артефакт показывает полный вывод списка всех пяти пакетов.
- PR #152 (только VMM stack F-001), head `48518fac18ba16e49d66fb641ece96ae06515811`, exact-head run `43586203250`: 42/43 checks success; единственный failure `vm (apps-drvman)`. Артефакт показывает, что после пятого пакета команда `drv` останавливается после строки count и не выводит rows.
- Это сравнение указывает, что F-009 из baseline является необходимой предпосылкой для прохождения данного сценария; F-001 не следует обвинять в этом failure без дополнительного evidence.
- Создана combined main-based ветка и Draft PR #154, включающие оба изменения F-001 + F-009. Head `a3bd9a47f9a9f2f7fad09823ac50c3c90c2a01cd`; exact-head host/build/ARM64 checks запущены, затем ожидается полная QEMU matrix.
- Ни один PR не объединён; main не изменён. Финальная интеграционная приёмка зависит от PR #154 exact-head CI.


### Stage 0 — x86-64 TLB shootdown safety finding (2026-11-14)

- Добавлен F-010. `arch/x86_64/smp.c::vmm_tlb_shootdown()` после 200 мс таймаута возвращает управление без подтверждения всех CPU, при этом очищает `sd_busy`; запрос хранится в общих глобальных полях.
- Поздний IPI может увидеть уже перезаписанные `sd_root/sd_va/sd_pages` и очистить бит нового pending mask, создав ложное подтверждение.
- `proc_shm_unmap()` после void shootdown освобождает shared object; `proc_unmap_pages()` тоже не получает статус, а DMA cleanup может освободить backing pages. Это требует fail-closed lifecycle.
- ARM64 использует отдельный broadcast TLBI `vae1is/aside1is` с барьерами; отдельная проверка ASID lifecycle ещё нужна.
- Приоритет: после проверки combined F-001/F-009 закрыть F-010 до дальнейшего расширения DMA/driver security.


### Correction — combined memory integration isolated F-001 regression (2026-11-14)

- Предыдущий вывод о том, что apps-drvman failure PR #152 связан с F-009, был преждевременным и заменяется этим результатом.
- PR #153 (PMM-only) passed 43/43, включая apps-drvman.
- Combined PR #154 (PMM + stack-slot) снова FAILED vm (apps-drvman). Следовательно, регрессия находится в F-001 stack-slot changes, а не в F-009 PMM patch.
- В исходной F-001 реализации active bitmap предотвращал немедленный double-free, но не защищал от ABA: запоздалый free старого владельца мог попасть в тот же slot после его повторного выделения.
- Исправление: добавлены 64-bit generation per slot; token проходит через vmm_kstack_alloc/free, сохраняется в thread_t и проверяется при teardown. Host regression теперь проверяет stale free после повторного выделения.
- PR #154 latest head e299838c074604ee9cfeb734c819f84a2a5185ae; новый exact-head CI ожидается. До прохождения apps-drvman и всей матрицы F-001 не закрыт.


### PR #154 — integration CI diagnosis and test synchronization (2026-11-14)

- Combined run `43587356143` on the earlier PR #154 head passed 41 checks, failed only `vm (apps-drvman)`, and had one cancelled job.
- Downloaded and inspected the failing VM artifact `vm-apps-drvman` (serial log + result.json). The installed `rl` package was present and verified; the expected `drv` command output was absent because the scenario issued `drv` immediately after observing `drvget verify rl: OK`, before the `drvget` process had exited and returned the console prompt. The harness returned in 0.00 s on the stale/queued prompt; the serial log ends at the next prompt without executing `drv`.
- This is a scenario synchronization race, not evidence of a broken package list or PMM/VMM regression. The test now explicitly waits for `кира> ` after verify and before issuing `drv`.
- New exact head: `4dd1454093af90f7d569df03b4425650da8da8bc`; exact-head workflow `43590094470` is running. Do not mark PR #154 green until all jobs finish.


### F-010 remediation implementation (2026-11-14)

- Created main-based draft branch `fix/x86-tlb-shootdown-fail-closed-2026-11-14`.
- Replaced the shared pending-bit completion protocol with a monotonically increasing 64-bit shootdown generation and per-CPU acknowledged generation. The sender accepts only the exact generation it published.
- A 200 ms threshold now emits one warning only. It no longer breaks the wait or releases `sd_busy`; the request payload remains immutable and backing pages cannot be reclaimed until every target CPU has completed its invalidation.
- Added `arch/x86_64/tlb_shootdown.h` and host cases for partial acknowledgements, old-generation acknowledgements, newer generation and empty target mask in `tests/smp_test.c`.
- Updated VMM/SMP contract comments. Exact-head CI has not yet been accepted; the change remains pending verification.


- Follow-up hardening: generation wrap is now rejected instead of reusing generation 1; counter exhaustion enters fail-closed wait. Host tests cover next-generation progression and UINT64_MAX rejection. Draft PR #155 head is `f2df3472d78241e6287d5cc7601b5d3a632b3e52`; exact-head workflow `43590730265` queued.


### F-011 — MMIO range overflow remediation (2026-11-14)

- Source review found `vmm_map_mmio()` checked overflow of `pa + len` but then separately added `PAGE_SIZE - 1` for align-up without checking that second addition.
- Added `vmm_mmio_range.h` with checked half-open range arithmetic; the helper rejects raw-end overflow, align-up overflow, null outputs, zero length and non-increasing aligned bounds.
- Updated `vmm_map_mmio()` to use the checked page range and validated raw end.
- Added `tests/mmio_range_test.c` for normal/cross-page ranges, final representable page, raw-end overflow and align-up overflow; wired into `tests/run_host_tests.sh`.
- Draft PR #156 branch: `fix/mmio-range-overflow-2026-11-14`. Exact-head CI pending. No local test run claimed.


### PR #154 — combined F-001 + F-009 accepted by exact-head CI (2026-11-14)

- Latest PR #154 head: `4dd1454093af90f7d569df03b4425650da8da8bc`.
- Exact-head workflow `43590094470`: **43/43 jobs PASS**, zero failures, zero cancellations.
- This closes the prior `apps-drvman` test synchronization issue on this exact combined head and verifies stack-slot generation ownership plus atomic PMM contiguous release across the CI host/ARM64/QEMU matrix.
- Status: fixes are verified on an unmerged Draft PR branch. They are not yet part of `main`; do not claim main is fixed until review/merge.


### PR #155 — F-010 exact-head CI passed (2026-11-14)

- Exact head `f2df3472d78241e6287d5cc7601b5d3a632b3e52`, workflow `43590730265`: **43/43 checks PASS**, zero failures/cancellations.
- Host suite, ARM64 build and QEMU matrix all passed. This verifies generation-bound per-CPU acknowledgements, stale-generation rejection, non-reusable counter policy and fail-closed timeout behavior under the current automated matrix.
- Status: verified on unmerged Draft PR #155; main remains unchanged. Hardware fault-injection for a permanently nonresponsive AP is not covered by the current CI and remains a limitation to record.


### F-012 — long driver-start record interleaving (2026-11-14)

- PR #156 run `43591102543` failed only `vm (drv-edu)` at the point checked by the expected driver-start regex.
- Downloaded artifact `vm-drv-edu` and inspected `serial.log` + `result.json`: driver process started, MSI was enabled, DMA round-trip succeeded, protected config write was rejected and user driver printed PASS. The failure was the kernel's long driver-start record split at `LB_SIZE=192`, with another CPU's user log inserted between fragments.
- Root cause: `kdrv.c` assembled a >192-byte record in `ln[320]` then used streaming `uart_print()`; `linebuf.c` flushes at exactly `LB_SIZE` even without newline.
- Added `uart_print_atomic()` using existing `emit_line()` lock; driver-start assembled record now uses this path. Draft PR #157, exact-head CI pending.
- This is a real observability/logging defect, not a driver launch/DMA/MSI failure. PR #156 acceptance needs a rerun after the logging fix is available; do not mask or weaken the QEMU assertion.


- F-012 cross-architecture follow-up: first PR #157 CI run `43591470627` caught an undefined `uart_print_atomic` symbol on ARM64. Added an ARM64 alias to its existing whole-string locked UART implementation. Current head `59b391696baabfd49f2ff48acf5aba2b9db538de`; exact-head workflow `43591626068` pending.


### ARM64 PMM ownership parity findings F-013/F-014 (2026-11-14)

- Source review of `arch/aarch64/pmm_arm.c` found no reserved bitmap: `pmm_arm_reserve()` only set the used bit, while `pmm_free_page()` accepted any used page. A stale free could release kernel/FDT/early page-table/DMA-reserved memory.
- ARM64 `pmm_free_contiguous()` looped over individual frees and could partially release a block before encountering an invalid middle page. This was not covered by the x86-64 F-009 fix.
- Draft PR #158 adds a separate reserved bitmap, counts overlapping RAM entries once, rejects frees of reserved pages, and validates/releases contiguous ranges atomically under one lock.
- Added `tests/pmm_arm_test.c` for reserved-page rejection, overlapping regions, late reservations, double free and invalid-middle all-or-nothing block release; wired into host CI with ASan/UBSan.
- Exact-head CI pending; no local tests claimed.


- PR #158 first host run `43591902136` stopped before compiling the new test because its include path omitted `arch/aarch64`; added the missing `-Iarch/aarch64` flag. This was a test harness path error, not a PMM implementation result. Updated head `a0b0a722957cdc75e550ff4ee9de3e5835dd7d22`, workflow `43591985380` pending.


### ADR-0003 — PMM ownership contract (2026-11-14)

- Added `docs/adr/ADR-0003-pmm-reservation-and-atomic-block-release.md`.
- Defines distinct free/allocated/reserved page ownership, rejects freeing reserved pages, requires all-or-nothing contiguous release, counts overlapping RAM regions once and requires x86-64/ARM64 behavioral parity.
- Status remains Proposed while ARM64 implementation PR #158 completes exact-head verification; x86-64 F-001/F-009 fixes are verified on Draft PR #154.


### F-015 — FDT reg-range parser hardening (2026-11-14)

- Source review found two parser arithmetic defects in `fdt_reg()`: `idx + 1` wrapped as 32-bit before widening, and decoded nonzero-size `addr + size` ranges were not checked for uint64 overflow.
- ARM64 startup consumes these ranges for RAM, UART, GIC, PCI ECAM and framebuffer setup; invalid ranges could propagate into memory/MMIO setup.
- Draft PR #159 computes the entry offset in 64-bit with subtraction-based bounds checks, rejects wrapping half-open ranges, and adds FDT host regressions for `UINT32_MAX` index and address/size overflow.
- Exact-head CI pending. Remaining FDT arithmetic (overlapping RAM totals, reserved-memory and physical-address limits) remains a separate audit item.


### PR #157 — F-012 exact-head CI passed (2026-11-14)

- Exact head `59b391696baabfd49f2ff48acf5aba2b9db538de`, workflow `43591626068`: **43/43 checks PASS**, zero failures/cancellations.
- Includes host tests, ARM64 build and full QEMU matrix, including `drv-edu`. The long driver-start record now remains contiguous while the driver still completes its DMA/MSI scenario.
- Status: verified on unmerged Draft PR #157. PR #156 must be rerun with this dependency to obtain a clean exact-head MMIO result; its earlier `drv-edu` failure is explained by F-012, not F-011.


- PR #159 first host run `43592195609` reached FDT tests and caught a test-only undeclared `len` variable in the new malformed-reg case. Replaced it with a dedicated `reglen`; implementation had compiled up to that test stage. Current head `1a61bd5bdc11b3dd0338c3142113c963308598bf`, exact-head workflow `43592382535` pending.


### PR #158 — ARM64 PMM ownership parity exact-head CI passed (2026-11-14)

- Exact head `70d0e89a65414818c22d61532f7ca4288c464d2c`, workflow `43592051859`: **43/43 checks PASS**, zero failures/cancellations.
- Includes the new ARM64 PMM host suite under ASan/UBSan, x86-64 build, ARM64 build and full QEMU matrix.
- F-013 and F-014 are verified on this unmerged Draft PR. This does not mean main has the fix; no merge was performed.


### PR #159 — FDT reg parser exact-head CI passed (2026-11-14)

- Exact head `1a61bd5bdc11b3dd0338c3142113c963308598bf`, workflow `43592382535`: **43/43 checks PASS**, zero failures/cancellations.
- Includes the new malformed-index and address/size-overflow FDT host cases under ASan/UBSan, x86-64 build, ARM64 build and QEMU matrix.
- F-015 is verified on this unmerged Draft PR. Remaining FDT-derived RAM overlap, reserved-memory and physical-address-width validation are explicitly not covered by this finding.


### ADR-0004 — Ring 3 DMA/MSI requires per-device IOMMU isolation (2026-11-14)

- Added `docs/adr/ADR-0004-ring3-dma-requires-iommu.md`.
- Records that DMA and MSI/MSI-X both require bus mastering and must be denied to Ring 3 drivers until a per-device IOMMU domain is created, attached, invalidated, mapped/unmapped and safely torn down.
- Defines fail-closed behavior, PCI quiesce, MMIO-only exception, future VT-d/SMMUv3 acceptance requirements and physical-hardware evidence requirements.
- Implementation candidate is Draft PR #161; exact-head CI `43593017450` pending at time of record.


### PR #160 — MMIO range + atomic UART combined exact-head CI passed (2026-11-14)

- Exact head `d5628989db142212bf6af26510a06e5bd0e70c90`, workflow `43592802619`: **43/43 checks PASS**, zero failures/cancellations.
- Includes F-011 checked MMIO range arithmetic and its ASan/UBSan host boundary tests, plus F-012 atomic UART records from the previously verified PR #157.
- Host tests, x86-64 build, ARM64 build and full QEMU matrix passed. F-011/F-012 are verified on this unmerged Draft PR; main remains unchanged.


### ADR-0005 — PCI host bus-range and bridge subordinate bounds (2026-11-14)

- Added `docs/adr/ADR-0005-pci-host-bus-range-enumeration.md`.
- Records the F-003 source defect: `pci_use_ecam()` stores a host bus range but `pci_init()` starts at bus 0; bridge traversal follows secondary bus without carrying the subordinate limit.
- Defines nonzero root-bus support, validated primary/secondary/subordinate tuples, parent-window inheritance, cycle protection and legacy x86 bus-0 compatibility.
- Implementation and host/QEMU regression remain pending.


### F-003 — PCI host bus-range implementation opened for current-main verification (2026-11-14)

- Draft PR #162: `pci_init_range()`, ECAM root starts at declared `bus_start`, bridge primary/secondary/subordinate validation, parent subordinate bound inheritance and legacy bus-0 compatibility.
- Host tests include nonzero root, nested windows, out-of-range bridge, wrong primary, invalid-range snapshot preservation, legacy scan and real ECAM path.
- Exact-head CI pending. No main changes and no PASS claimed.


### PR #160 — MMIO overflow + atomic UART integration exact-head CI passed (2026-11-14)

- Exact head `d5628989db142212bf6af26510a06e5bd0e70c90`, workflow `43592802619`: **43/43 checks PASS**, zero failures/cancellations.
- Includes checked MMIO page-range arithmetic and the exact-head-verified atomic UART record fix, host tests, x86-64/ARM64 builds and full QEMU matrix.
- F-011 is verified on this unmerged Draft PR; main unchanged.

### PR #162 — PCI ECAM host-bus and subordinate-window bounds exact-head CI passed (2026-11-14)

- Exact head `2e30dc5c9edff973a949d40dacbed18d2d666ab8`, workflow `43594509620`: **43/43 checks PASS**, zero failures/cancellations.
- Host ASan/UBSan coverage includes nonzero ECAM root bus, nested subordinate windows, host-range escape, mismatched primary bus, invalid-range snapshot preservation, legacy bus-0 behavior and actual ECAM wrapper; x86-64/ARM64 builds and QEMU matrix passed.
- F-003 is verified on this unmerged Draft PR. Exploratory duplicate PR #163 was closed without merge in favor of #162. Main unchanged.

### PR #161 — DMA/MSI fail-closed policy test correction (2026-11-14)

- First exact-head run `43593017450` exposed three scenario assertions that did not match the current behavior: escaped bracket regexes in the app UI scenario, a package-selection assertion that assumed one of two matching packages would always be selected after stop, and a global DHCP prohibition that also matched the earlier boot phase.
- A corrected scenario commit `11538b6685e504e861e48328553b5b973a41a977` was run as `43599700642`. The corrected UI assertion reached the actual asynchronous driver-denial path; it then exposed a timing issue because Driver Manager polls for kernel completion for up to 6 seconds. The scenario now waits 7 seconds before checking the final user-facing result.
- A fresh exact-head run is required for the timing correction. No PASS claimed; PR #161 remains Draft/unmerged.


### F-004 — BootInfo contract proposal drafted (2026-11-14)

- Reviewed current x86-64 Multiboot2 entry (`kernel_main(magic, addr)`, including high-address MBI relocation) and ARM64 FDT entry (`arm_main(dtb, entry_el)`, which currently parses RAM, reserved-memory, framebuffer, PSCI/GIC and PCI host data directly).
- Added proposed ADR-0006 defining immutable/versioned BootInfo, normalized memory-map ownership, bounded optional firmware/module/framebuffer/command-line/DMA/IOMMU records, physical-range validation, source lifetime and adapter responsibilities.
- Updated F-004 in the source findings ledger. This is documentation/contract work only; no implementation or acceptance is claimed. Existing boot paths remain unchanged.


### F-008 — scheduler guarded-stack policy hardened on Draft PR #164 (2026-11-14)

- Removed production fallback from guarded `vmm_kstack_alloc()` to unguarded `kmalloc()`; guarded-window exhaustion now causes thread creation to fail closed.
- Clear the full stack allocation before installing its canary and publishing the thread, preventing stale stack contents from carrying across reuse.
- Added a scheduler host regression for zeroed stack body and updated the `kstack.h` contract.
- Exact-head CI pending; no PASS claimed. Production guarded-window exhaustion still needs explicit integration coverage.


### Stage 0 follow-up — exact CI evidence and dependency review (2026-11-14)

- Rechecked PR #165 after the earlier UHCI regression: exact head `64afef2c74f72597dd9620468c4b6bff1fc1bb31`, workflow `43603442098`, completed successfully. All 30 jobs returned by the workflow-jobs endpoint succeeded, including `vm (usb-uhci)`. This supersedes the previous failure status for that head.
- Removed temporary `[KIRA BOOT TRACE]` instrumentation from `kernel.c` and temporary USB PCI trace code / UART include from `pci.c` on PR #165. Cleanup head is `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`; no exact-head CI run/status has appeared yet. Do not accept the cleanup head until CI confirms it.
- Rechecked PR #166 F-016: exact head `9d6443ff2db7195e8abf0c6d63a6257c1c38f229`, workflow `43602565437`, completed successfully; all 30 returned jobs succeeded, including host tests, x86-64, ARM64 and QEMU matrix.
- Source review found a cross-PR dependency: #166's new late `pmm_arm_reserve()` calls require #158's reserved bitmap semantics so reserved firmware pages cannot later be freed. #158 exact head `70d0e89a65414818c22d61532f7ca4288c464d2c`, workflow `43592051859`, 43/43 PASS. Plan combined review/integration and add an FDT-reservation-to-PMM host regression; no merge performed.
- Main remains unchanged. No local test execution or physical ARM64 test was performed.


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

- PR #165 cleanup exact head `18a4bc319d85c3b05ea3fc9f341db616acfdffdb`, workflow `43608372681`: **43/43 jobs completed successfully**, zero failed jobs. This supersedes earlier queued/in-progress notes for that head.
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


### F-004 branch-preservation follow-up — BootInfo and firmware loaders

- The unified handoff is not absent from project history: it exists in preserved branch stacks and must be reused rather than recreated.
- PR #12 (`v46/release-completion`, base `v45/overhaul`, head `c3342321667045a29b3c7e4726a31b926117a9b1`) adds `bootinfo.c`, `bootinfo_mb2.c`, `bootinfo_fdt.c`, BootInfo tests, ARM64 PMM, and related build/security/release changes. Its two recorded CI runs `43236520117` and `43236520194` succeeded, but this branch is not based on main.
- PR #46 (`v46/unified-release-integration`, base `v46/kdrv-catalogue-signature-policy`, head `dacc04d0dafd507ef0721698b5e9c2512f1947a5`) adds UEFI BootInfo, BIOS/UEFI boot composition, UEFI builder/tests, IOMMU modules, driver trust/signatures and broad release integration. No exact-head workflow run was found for #46.
- PR #45's UEFI ELF loader hardening is a separate branch on the same v46 stack; its recorded run `42385882597` failed. PRs #60/#61 preserve UEFI command line/modules and initrd loading on base `autodrv` and each passed CI; PR #130 adds module-to-usable-RAM validation on the v46 release branch and is closed there. None of these results are current-main acceptance.
- Integration rule: retain BootInfo v1 ABI and producer tests from the v46/autodrv work; compare the exact structures and ownership rules against proposed ADR-0006; transplant one adapter at a time onto current main; do not replace working MB2/FDT paths until the common consumer and PMM reservation ledger pass both-architecture boot tests.


## 11. Current-main kernel remediation stack — verified but not integrated

These are separate Draft PRs directly based on current main. Their exact-head CI has passed, but main remains unchanged:

| PR | Scope | Exact-head run | Result | Integration note |
|---|---|---:|---|---|
| #154 | Kernel stack slot ownership/generation, partial-map cleanup, x86 PMM atomic block release | `43590094470` | success | Overlaps #164 in `kstack.h`, `sched.c`, `sched.h`; integrate together. Also overlaps standalone #153 in `pmm.c`; do not merge both PMM implementations. |
| #155 | x86 TLB shootdown fail-closed behavior | `43590730265` | success | Separate SMP/VMM contract; re-run with the final combined memory branch. |
| #158 | ARM64 PMM reserved-page ownership, overflow-safe reserve, atomic contiguous release | `43592051859` | success | Required by FDT reservation-map application in #166; #166 now carries this implementation. |
| #161 | Ring 3 DMA/MSI denial until per-device IOMMU is verified | `43602754339` | success | Current-main security gate; intentionally blocks DMA/MSI because main has no domain implementation. |
| #164 | Remove heap-stack fallback, zero new stacks, fail closed on guarded stack exhaustion | `43600699384` | success | Overlaps #154's scheduler/stack ownership changes; combine and re-run together. |
| #165 | PCI firmware Bus Master handoff mitigation, diagnostics removed | `43608372681` | 30/30 success | F-017 remains open during BAR probing; reconcile with PCI range #139/#162. |
| #166 | FDT reservation map + ARM64 PMM integration + v16 parser | `43609132620` at head `539285048cdd97f5d71ba5aefc9afda4058b7512` | 30/30 success | Subsequent source review found and corrected a block-overlap validation defect; new head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e` requires fresh exact-head CI. |

### Required integration sequence
1. Combine #154 + #164 in one scheduler/stack ownership change; keep #153 as an overlapping alternate, not a second independent merge.
2. Integrate #155 against the final VMM/stack baseline and verify cross-CPU shootdown failure semantics.
3. Integrate #158 through #166's FDT-to-PMM test; ensure permanent firmware reservations cannot be freed.
4. Keep #161's DMA/MSI denial active until real VT-d/SMMUv3 domains are integrated and per-device readiness is proven.
5. Reconcile #139/#141 PCI identity/range work with #165's firmware Bus Master logic before accepting Device Model/PCI.
6. Run the full exact-head host, x86-64, ARM64 and QEMU matrix after all overlapping changes are combined. Individual green PRs do not prove the combined tree.


### CI failure/fix record — PR #148 duplicate checker tail (2026-11-14)

- Exact head `2bbcef9eb86a43919099d2f26eb8af817c4c893b`, workflow `43609381410`: host-tests failed with `SyntaxError: unmatched ')'` at line 211. A duplicated stale tail of the previous checker implementation remained after the final success messages.
- Removed the entire duplicate tail; the single checker now ends after the four success messages. No checks were bypassed.
- Corrected PR #148 head `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`, workflow `43609627054`: all 30 jobs succeeded, including host-tests, x86 build, ARM64 and QEMU VM scenarios.



## 12. Exact-head CI and source-audit follow-up — 2026-11-14

- PR #166 head `539285048cdd97f5d71ba5aefc9afda4058b7512`, workflow `43609132620`: all 30 jobs succeeded (host-tests, x86 build, ARM64 and VM scenarios).
- A deeper source review then found that `fdt_valid()` paired the structure and strings offsets with the opposite size fields during overlap checks. Corrected only those range pairings and added a one-byte-overlap regression test in PR #166.
- Corrected PR #166 head: `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`. No exact-head CI result is available yet; do not reuse the prior head's PASS.
- PR #148 head `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`, workflow `43609627054`: all 30 jobs succeeded. The complete x86-64, ARM64 and Android-release app manifest is verified by that exact-head CI.
- PR #125's failed host and ARM64 logs were inspected. Besides the known undeclared `IOMMU_E_UNSUPPORTED` symbol, its QEMU shard failed `usb-uhci` because the guest did not reach the shell prompt in 90 seconds. This confirms that the branch is not ready to transplant.
- Added F-021: the branch-only x86 VT-d and ARM64 SMMUv3 backends have different effective legacy/no-coverage DMA policies. Keep current-main PR #161's fail-closed DMA/MSI gate until one explicit cross-architecture policy contract and matching tests exist.
- Main remains untouched. No ISO or release archive was produced.


## 13. Scheduler and memory overlap integration — 2026-11-14

- Reviewed exact diffs for PR #154 (generation-token guarded stack ownership + atomic PMM contiguous release), #164 (fail-closed guarded-stack allocation + zeroing), and #153 (duplicate PMM-only change).
- Found #154 still fell back to unguarded heap stacks on VMM allocation failure. Integrated #164's fail-closed policy and stack clearing into #154's branch, preserving generation-token allocation/free signatures and architecture-specific VMM implementations.
- Added host regression checking the canary and zeroed stack body before publication.
- Latest #154 integration head: `8f4b9be58f63d93911817184dcbbce0e4eca36c1`. No exact-head CI result is available yet; the previous 43/43 pass belongs to the earlier head and is not evidence for this change.
- #153 is a duplicate of the PMM contiguous-release code already included in #154; #164 is an overlapping scheduler reference. Keep the integration candidate separate and do not merge duplicates independently.
- Corrected #148 record: head `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`, workflow `43609627054`, all 30 jobs succeeded.
- PR #166's corrected head remains under workflow `43610515593`; host-tests and ARM64 were still running at the last poll. No final PASS is claimed.


## 14. UEFI branch-stack and module-memory policy audit — 2026-11-14

- Inspected exact PR #46 UEFI loader, KBI v1 adapter, and kernel entry. That branch does not collect UEFI LoadOptions or load INITRD.IMG; its compatibility adapter emits only mmap/framebuffer tags.
- Confirmed UEFI initrd + LoadOptions work exists in PR #61 and MB2 module/cmdline serialization in PR #60, both merged into the separate `autodrv` branch on 2026-11-10. Neither is integrated into PR #46's current branch head.
- Inspected PR #130 module validator, merged into `v46/release-completion`. It accepts module ranges only in `KBI_MEM_USABLE` or `KBI_MEM_MODULE`.
- PR #61 classifies UEFI `EfiLoaderData` as `KBI_MEM_BOOTLOADER` and does not reclassify the exact INITRD.IMG allocation as `KBI_MEM_MODULE`. This is a real integration conflict: applying PR #130 unchanged would reject the module, while broadly accepting LoaderData would weaken validation.
- Added findings F-023 and F-024 with exact branch evidence and required resolution. No branch merge or main change was made.


## 15. PCI range + firmware DMA policy integration — 2026-11-14

- Merged the source logic from main-based PR #162 into PR #165's security branch without touching main: host ECAM range validation, nonzero root-bus enumeration, bridge primary/secondary/subordinate checks, and nested subordinate limits.
- Preserved #165's firmware Bus Master cleanup policy and added both #165's `pci_test.c` and #162's `pci_range_test.c` to the same host test runner. Updated the virtio bridge model to state its primary bus explicitly.
- PR #165 combined head is `f986d3f84fdc6bac6dd72ce7d4fd6a76bc4d3caf`. Exact-head CI has not yet appeared; prior #165 43/43 PASS is not reusable for this combined head.
- F-017 remains open because Bus Master is still cleared after BAR probing; the earlier pre-probe attempt caused a UHCI VM regression. No security-complete claim is made.
- PR #162 is a duplicate source candidate after integration; do not merge it independently. PR #139's device registry/PCI branch stack remains separate.


## 16. FDT exact-head CI completion — 2026-11-14

- PR #166 corrected head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`, workflow `43610515593`: 43/43 jobs succeeded.
- Confirmed host-tests, x86 build, ARM64 build/scenarios and all VM matrix scenarios succeeded on the corrected source, including the FDT v16 and overlap regression tests.
- This is a verified branch CI result, not a main merge or release claim. Main remains untouched.


## 17. Device Model completeness and rescan-owner audit — 2026-11-14

- Inspected PR #141's exact registry, PCI adapter, KDRV handle binding, rescan guard and tests. Exact head `cda2ba2b3b9432f0dcae1843bc331ac858c4b6e2` has five successful workflow runs; PR remains Draft and stacked on #139's architecture branch.
- Confirmed the registry is a partial discovery/identity slice: no typed MMIO/port/IRQ/DMA resources, IOMMU domains, power/reset, package provenance, resource teardown or full lifecycle states.
- Found the rescan guard checks KDRV binding slots only. Built-in PCI drivers also enable bus mastering and map BARs, but are not represented in that guard. Post-boot PCI rescan must remain globally denied until all built-in and KDRV owners participate in a coordinated quiesce/revoke transaction.
- Added F-026. No registry or PCI code was transplanted into main-based PRs; the separate architecture branch remains preserved for later coherent integration.


## 18. PR #154 VM failure diagnosis and UART correction — 2026-11-14

- Workflow `43610795635` on PR #154 head `8f4b9be58f63d93911817184dcbbce0e4eca36c1`: 29/30 jobs succeeded; only `vm (drv-edu)` failed the expected KDRV success log match.
- Inspected the uploaded VM artifact (`result.json`, `serial.log`, screenshots). The driver actually launched, DMA round-trip passed, protected PCI configuration writes were rejected, and the driver printed PASS. The log line was 225 UTF-8 bytes but the per-CPU line buffer was 192 bytes, splitting it inside a multibyte character and allowing another CPU's log to appear between chunks.
- Fixed long-string output in `uart_print()` to flush pending text and emit an oversized string atomically. No test was weakened.
- PR #154 new head `bfdffa64555ae221aa66c7c71efbb47f169c3f6a`, workflow `43611654110` queued; exact-head CI is pending.


## 19. Status manifest exact-head CI — 2026-11-14

- PR #148 head `bf35ca714526d38f5fdc3021c03b2bedf3ec2c99`, workflow `43609627054`: 3/3 jobs succeeded (host-tests, x86 build, ARM64 build).
- Manifest now parses explicit UELF additions and Android profile exclusions; verified counts: x86-64 25, ARM64 25, Android release 13. This is inventory guard coverage, not a claim of full semantic review.


## 20. Kernel memory/scheduler integration exact-head CI — 2026-11-14

- PR #154 head `bfdffa64555ae221aa66c7c71efbb47f169c3f6a`, workflow `43611654110`: 43/43 jobs succeeded across host-tests, x86, ARM64 and full VM matrix.
- UART oversized-line correction resolved the earlier `drv-edu` log assertion without weakening tests. Both `drv-edu` and `usb-uhci` pass on this exact head.
- PR remains Draft; no main merge and no ISO build.


## 21. F-022 status correction — 2026-11-14

- Replaced the stale pending-CI status for F-022: PR #154 exact corrected head `bfdffa64555ae221aa66c7c71efbb47f169c3f6a` passed 30/30.
- CI gate closed; per-ISA stack allocator source review and final merge composition remain open.


## 22. PCI UHCI diagnostic rerun and IOMMU enum correction — 2026-11-14

- PR #165 combined PCI range + Bus Master branch had one UHCI VM failure on `f986d3f...` before PCI enumeration completed. A temporary trace build passed the same UHCI scenario and showed root bus 00 only, then the diagnostics were removed. Clean no-trace head `4b99d54...` is pending fresh exact-head CI; no claim of resolution.
- PR #125 had a real compile blocker: missing `IOMMU_E_UNSUPPORTED` in the common error enum. Added the explicit `-10` code. Corrected head `8dc795e...` workflow `43612438370` is running; do not claim pass until all jobs complete.


## 23. PCI enumeration hang: per-function instrumentation — 2026-11-14

- Clean PR #165 run `43612506879` reproduced the UHCI boot hang: 28/30 completed jobs passed; `vm (usb-uhci)` failed before the PCI enumeration completion log. QEMU emitted tens of millions of rejected UHCI DMA reads at physical address 0 while firmware DMA remained active.
- Root cause is still not isolated. Trace-only PCI range build passed once; clean build failed again. Added temporary per-function diagnostics around BAR probing and Bus Master clearing on PR #165 head `2b3b59828bacabee18001fc80faea1ff3e39cc31`. Await exact-head VM logs; remove diagnostics after locating the stall.
- #165 is blocked from merge until clean full-matrix verification succeeds.


## 24. F-022 per-ISA source review — 2026-11-14

- Reviewed x86-64 `vmm.c` and ARM64 `arch/aarch64/main.c` guarded-stack allocators plus common `sched.c` wrappers and `kstack_slot.h`.
- Both paths use generation-bearing slots, retain guard pages, quarantine partial mappings and reject stale/double frees. Host ASan/UBSan tests cover the shared helper, including ARM64's last slot at index 65,535.
- PR #154 full x86/ARM64 CI and VM matrices pass normal scheduler/mutex worker paths. Dedicated guard-fault and full slot-exhaustion QEMU tests are still absent; no false claim of complete negative-path coverage.


## 25. Exact UHCI PCI probe stall isolated — 2026-11-14

- PR #165 head `7ad140cfadb51c4dacf809eca77fff60ff7fe12c`, run `43613588426`, UHCI job `132504574309`: serial trace completed functions 00:00.0, 00:01.0 and 00:01.1, then stopped between `begin 0:1.2` and `probed 0:1.2`. This isolates the hang to `probe_bars()` on the UHCI controller (00:01.2).
- The uploaded QEMU log is ~4.19 GB; its tail repeats rejected DMA reads from 0x0/0x4/0x8/0xC. #165 clears Bus Master only after BAR probing, while #125 clears it before probing. This is strong evidence of an interaction between active firmware DMA and BAR probing, not yet a fully proven root-cause mechanism.
- #125 corrected run `43613583149`: UEFI OVMF and all non-VM checks pass; remaining VM shards are still running. Compare its UHCI result before any PCI policy change. Both PRs remain unmerged; temporary trace must be removed after diagnosis and clean full CI rerun.


## 26. F-025 trace interpretation corrected from exact artifacts — 2026-11-14

- Re-downloaded and inspected both VM ZIP artifacts instead of relying on the earlier summary. PR #165 run 43613588426, job 132504574309: serial trace reaches “probed 0:1.2” and then stops before “secured 0:1.2”; BAR probing returned. The remaining policy sequence reads PCI Command and conditionally clears Bus Master with pci_write16().
- PR #125 run 43613583149, failed shard-0 job 132504922587: serial trace reaches “disable BM 0:1.2” but not “BM disabled 0:1.2”; its pci_disable_bus_master() helper performs config read/write/readback under cfg_rmw_lock.
- Both branches therefore stall while attempting to change the UHCI function's Bus Master state, but their instrumentation does not yet identify the exact sub-operation. The earlier Stage 0 statement that PR #165 stalled inside probe_bars() is withdrawn and corrected in STAGE0_SOURCE_FINDINGS.
- PR #125's other shard-0 scenarios boot-virtio, apps-explorer2 and net-ipv6 passed their recorded checks; UHCI failed after 90 s waiting for the prompt. At inspection, #125 had 18 successful jobs, one failed VM shard and one in progress; #165 had 29 successful jobs and the UHCI VM failure.
- Next: instrument bounded Command read/write/readback and UHCI USBCMD/USBSTS state; compare class-aware controller quiesce against early IOMMU isolation; add bounded host/QEMU regressions. Do not merge either PCI branch until repeated clean full CI passes. No main change, no merge, no ISO build.


## 27. FDT parser source review and test coverage inventory — 2026-11-14

- Reviewed main FDT parser and host fixtures. The current main parser has bounded structure/property payload traversal and rejects malformed structure tokens, but property-name comparison uses streq(strings + nameoff, name) without a remaining-length bound. Added F-027 as a MEDIUM malformed-firmware-input validation gap; no crash or exploit is claimed.
- PR #166 exact head 4bc69a224887ead8d8d1737e782ed76cf4b8e95e, associated workflow 43610515593: 43/43 jobs succeeded. This includes FDT v16 structure-size compatibility, header reservation-map validation/application, block overlap checks and reg index/address overflow regressions. Its fdt_prop() property-name terminator boundary remains uncovered and F-027 is still open.
- Counted the pinned-main test entry points: tests/run_host_tests.sh defines 47 named host suites; ci/scenarios.py defines 38 QEMU VM scenarios. Host coverage spans PMM, scheduler/context/FPU, KDRV package parsing, PCI/virtio, KiraFS, process/ELF, IPC, SMP, graphics/UI, KIRA Mind, FDT, USB UHCI/xHCI, FAT/VFS, networking/TLS, KCC and driver workflows.
- The 38 VM scenarios cover boot devices/modes, memory/SMP, USB, networking, sound, graphics/apps, power, fault isolation, KIRA Mind and driver installation/management. This is a broad existing test base, but scenario names alone do not prove every failure mode or both-ISA coverage; map each scenario to the component/ISA/build profile before acceptance.
- Stage 0 percentage unchanged: inventory and test entry-point counts are verified, but semantic review of all auxiliary source paths and branch-only integration dependencies is not complete.


## 28. BootInfo branch-stack comparison — 2026-11-14

- Compared PR #12 and PR #46 exact source files, not only PR descriptions. Both expose BootInfo v1 with the same packed header and record layouts, but the implementations differ materially.
- PR #12 has the stronger common validator: it verifies that each module's complete physical range is covered by USABLE/MODULE memory-map entries. Its FDT producer consumes the firmware reserve map, reserves kernel+DTB, subtracts /reserved-memory and reserves simple-framebuffer memory.
- PR #46 supplies the missing UEFI path: PE32+ loader reads the kernel ELF, collects GOP/RSDP and EFI memory descriptors, builds KBI, retries GetMemoryMap/ExitBootServices on stale map keys, then enters the kernel with Microsoft x64 ABI arguments (BootInfo pointer and size). It also adds UEFI KBI tests for memory-type mapping, GOP geometry, descriptor overflow/overlap and RSDP alignment.
- PR #46's bootinfo.c does not contain PR #12's module_in_ram() validation; its bootinfo_fdt.c is also a shorter implementation without the same reserve-map and reserved-memory subtraction. The headers share v1 layout, but their semantic validators are not interchangeable.
- Exact CI evidence: PR #12 runs 43236520117 and 43236520194 succeeded; PR #46 has no workflow run associated with its exact head. PR #45 UEFI hardening run 42385882597 failed; PR #60 run 42779406776 and #61 run 42780403870 succeeded; PR #130 runs 43164087429 and 43164087483 succeeded, all on divergent bases.
- Added F-028. Integration rule: preserve the stronger PR #12 validator/FDT reservation semantics while adapting and separately verifying the PR #46 UEFI producer/loader. Do not merge branch stacks wholesale or assume same ABI version means same semantics.


## 29. PR #125 diagnostic workflow terminal result — 2026-11-14

- Re-polled workflow 43613583149 on exact PR #125 head fee8c9aed19ffc37dab7fd599c2bd63bfc496924 after the last VM shard finished.
- All 23 jobs are terminal: 20 success, one failure (vm (0), job 132504922587), two packaging jobs skipped by workflow conditions. All non-VM checks and 9 of 10 VM shards passed; the sole failure is usb-uhci.
- PR #165 workflow 43613588426 remains 29/30 with only vm (usb-uhci) failed.
- The two exact artifact traces stop at different diagnostic boundaries but both fail on UHCI Bus Master handling. This is not a PASS and does not authorize merge.


## 30. CI topology and architecture-specific test inventory — 2026-11-14

- Read the pinned-main .github/workflows/ci.yml, ci/scenarios.py, ci/kira_vm.py, ci/arm64_vm.py and .github/workflows/mind-train.yml rather than inferring test coverage from filenames.
- x86 scenario catalogue: 38 explicitly declared scenario definitions plus two dynamically generated stress-spam variants (2-core and 4-core) = 40 available x86 scenarios. The main CI matrix runs 39; mind-train is intentionally excluded and runs only through the separate manual ~25-minute workflow.
- ARM64 has 16 separately defined QEMU TCG scenarios, including GICv2/v3, 1–8 cores, EL1/EL2, graphics/input, storage/NVMe, networking/TLS, KCC, drivers, DMA-noncoherent and power/fault paths.
- Main CI has distinct host-tests, x86 build, x86 scenario matrix, ARM64 build/scenarios and artifact/report jobs. The x86 scenario runner uses KVM when available and increases timeout scale when it is not; ARM64 harness explicitly uses QEMU TCG.
- The earlier count of 38 referred only to explicit sc(...) declarations, not the expanded runtime catalogue or architecture-specific totals. The matrix documentation now distinguishes all three.
- This topology supports tiered testing: run a focused named x86 scenario and matching ARM64 case for shared contracts, then affected neighbors, then full 39+16 acceptance. Keep the separate mind-train job out of routine regression loops unless Mind training/persistence is changed.


## 31. Three-bucket preservation model and remaining source-review queue — 2026-11-14

- Parsed the 829-row CSV with a quote-aware CSV parser (the first quick comma split was invalid because classification text contains commas; no inventory data was changed by that failed read).
- Exact classification counts: 129 AUXILIARY_SOURCE_REVIEW, 22 ARCH_SUPPORT_REVIEW, 10 TEST_HELPER_REVIEW, 60 USERLAND_BUILD_OR_SUPPORT_REVIEW and 3 DRIVER_SOURCE_REVIEW = 224 paths still needing semantic ownership/build-profile review. The 365 KCC fixtures are separately classified as test fixtures, not production source omissions.
- Confirmed build-input groups remain separate: 98 x86-64 common kernel C inputs (80 shared + 18 x86-specific), 88 ARM64 kernel C inputs (80 shared + 8 ARM64-specific), 16 x86 architecture sources, plus 12 architecture build-support files referenced by build scripts and 4 boot build inputs.
- Reframed the project inventory into three preservation buckets: (1) baseline—tracked production/build/test inputs that must never disappear from complete builds; (2) under audit/rework—review-status source files and active Draft PR changes; (3) planned/branch-only—features preserved in branches or architecture plans but not active production inputs in main.
- The buckets are preservation/work-state labels, not readiness claims. A file in the baseline bucket can still contain a defect; a branch-only feature is not integrated merely because it has source and tests.
- Stage 0 remains incomplete: 224 source/support paths require semantic review and cross-branch API/build linkage checks. No percentage increase is claimed from inventory counting alone.


## 32. Architecture-support header review — x86-64 / ARM64 — 2026-11-14

- Reviewed all 22 architecture-support headers classified in the pinned-main matrix, plus the 12 architecture assembly/linker files classified as build-support inputs.
- Verified the shared per-CPU and scheduler interfaces are architecture-specific implementations with expected ABI differences: x86-64 starts a new thread with return-address stack alignment; ARM64 uses aligned SP and link register semantics. User VA layout differs by architecture as expected.
- x86 CPU/FPU no-op helpers are explicitly guarded by KIRA_HOST_TEST (or the dedicated host FPU test switch); production branches use x86 instructions. They are test harness accommodations, not production stubs.
- ARM64 smp_crash_test(mode=1) explicitly returns -5 because the platform cannot software-inject SError without the relevant RAS feature. The ARM64 QEMU scenario asserts the documented “cannot raise SError from software” result rather than pretending that path passed. This is an explicit unsupported diagnostic mode, not a silent stub.
- ARM64 FPU state is 528 bytes versus x86-64 512 bytes; architecture-specific formats must remain separate and are covered through architecture FPU/context tests, not forced into one raw layout.
- No production empty function or silent success stub was identified in this reviewed header set. This is limited to the reviewed architecture interfaces; it is not a project-wide stub audit.


## 33. Test-helper and stub containment audit — 2026-11-14

- Reviewed all 10 paths classified TEST_HELPER_REVIEW. The special tests/stub/kira.h contains deterministic fake kernel APIs for a host-only window/application test; tests/run_host_tests.sh references it through tests/stub include path for the paint test.
- Production build scripts build_clang.sh and build_arm64.sh do not include tests/ or tests/stub. The host runner compiles with KIRA_HOST_TEST and test-only model headers; production userland build uses userland/kira.h and the real syscall ABI.
- tests/usbdev_model.h and tests/rl_test.c provide host device/driver models; tests/kfs_v1_impl.inc is a test reference implementation. They are not production driver or filesystem implementations.
- tests/tls_host_api.h and tests/wasm_gen.py are test harness support. The test-only fixtures are not evidence of production functionality.
- No reviewed test helper is linked into the production ISO build manifests. This closes the classification ambiguity for these 10 paths; the actual test quality remains separately assessed by the 47-suite runner and VM matrix.


## 34. PR #148 three-stage registry exact-head verification — 2026-11-14

- Re-fetched the three status documents and manifest from PR #148 current head bf35ca714526d38f5fdc3021c03b2bedf3ec2c99: 01_READY, 02_IN_PROGRESS, 03_PLANNED. This directly implements the user's requested separation of stable baseline, active branch work and future work.
- Verified the definitions are careful: READY means present in current baseline/build inputs, not complete; IN_PROGRESS includes branch-only source that is preserved but not in main; PLANNED lists explicit acceptance criteria.
- The status manifest distinguishes current x86 release artifacts (kira.iso, kira.disk, kira.bin) from ARM64 images built but not published by current main CI.
- Exact current PR head workflow 43609627054 completed **30/30 success**. This supersedes previous pending status for PR #148.
- The checker protects the pinned 829-file baseline, source lists, 25 x86/25 ARM64 userland programs, 13-app x86 Android profile and complete branch/PR snapshots. PR #148 is still Draft/unmerged, so the protection is not yet active on main.


## 35. x86-64 / ARM64 kernel initialization-order comparison — 2026-11-14

- Read the complete x86 kernel_main() and ARM64 arm_main() initialization sequences from pinned main, rather than assuming the two builds share one boot path.
- x86 sequence: Multiboot2 relocation/early console → GDT/per-CPU/PIC/IDT/FPU → PMM/VMM/null guard/SMEP/SMAP → RAM extension/ACPI/hardware profile → heap/scheduler/security/watchdog/console/process/syscall → experience/KiraFS/life/Mind → display/input/USB/network/audio → PIT/TSC/APIC/IOAPIC → SMP → Ready.
- ARM64 sequence: FDT validation/firmware discovery → RAM map/DMA policy/MMU → GIC/timer/CPU/PSCI → early PMM + reserved ranges → heap/scheduler/GIC online → timer verification → PSCI SMP → clock/PCI/RNG/security/watchdog/storage/filesystem/life/process/syscall/Mind → RTC/input/display/USB/network/audio → Ready.
- The order is not identical and should not be forced into one sequence. The acceptance rule is dependency readiness: no consumer before its memory, interrupt, identity and security prerequisites are established. Tests must exercise the distinct boot path and verify equivalent externally visible contracts.
- Current-main gaps remain: ARM64 does not yet apply the FDT header reservation map to PMM (PR #166 adds and verifies this on a Draft branch); x86/ARM64 still consume loader-specific data directly rather than a unified BootInfo v1 contract (F-028).
- Added explicit dependency ordering to the test graph; no kernel source changed and no readiness claim was upgraded from this comparison alone.


## 36. PR #165 UHCI QEMU log streamed and counted — 2026-11-14

- Streamed the entire 4,187,160,076-byte uncompressed qemu.log from artifact 14121317727; did not allocate the full log in memory.
- Rejected 4-byte DMA reads: 0x0 = 15,377,534; 0x4 = 15,377,534; 0x8 = 15,377,534; 0xC = 15,377,533.
- The VM scenario attaches a USB tablet and keyboard to the UHCI controller. The repeated low-address reads are consistent with a live controller fetching an invalid/zero-based frame list while the PCI handoff stalls.
- This is strong supporting evidence, not register-level proof. The artifact does not contain USBCMD/USBSTS/FLBASEADD values, and the serial log stops after BAR probe before the “secured” marker.
- Next discriminating test: capture those UHCI registers before handoff, halt/reset the controller with bounded waits, then clear Bus Master and verify the same Limbo-like USB scenario repeatedly. Preserve the IOMMU-first alternative. No kernel code changed.


## 37. USB controller DMA handoff order reviewed — 2026-11-14

- Compared the PCI scan path with the actual USB driver entry sequence. usb_main() starts controllers only after pci_init() returns.
- Found a second DMA handoff issue beyond the UHCI enumeration stall: uhci_start() re-enables PCI_CMD_MASTER together with I/O decode before requesting USBCMD stop/HCHalted and before programming the new FRBASE. xhci_start() similarly enables PCI_CMD_MASTER with MEM decode before BIOS handoff, USBCMD stop, HCRST, and before replacing DCBAAP/CRCR/event-ring state.
- Therefore the PCI enumeration clear is not a durable boundary. The drivers can re-grant DMA while firmware-owned controller state is still active. Added F-029 as HIGH and separate from F-025 (current UHCI boot hang) and F-017 (firmware DMA during BAR probing).
- Safer order: enable IO/MEM decode without MASTER; bounded controller-specific BIOS handoff/stop/reset; allocate and program kernel-owned DMA structures; only then enable MASTER through the DMA/IOMMU policy gate. On any timeout, keep MASTER denied and quarantine.
- No source changes made; this is Stage 0 source evidence and a required test dependency for the future PCI/USB fix.


## 38. Cross-driver PCI Bus Master ordering audit — 2026-11-14

- Reviewed current-main PCI enable helper and DMA setup order across VirtIO, NVMe, AHCI, e1000, HDA, AC'97 and USB.
- pci_enable() only ORs bits into PCI Command; callers commonly request MASTER at the same time as IO/MEM decode.
- Identified early MASTER grants before reset or kernel-owned DMA structures: VirtIO before status reset/queue setup; NVMe before CC.EN stop and admin queues; AHCI before BIOS handoff/GHC reset and per-port setup; e1000 before device reset and RX/TX rings; HDA before controller reset and CORB/RIRB/BDL; AC'97 before cold reset and DMA buffers; UHCI/xHCI as F-029.
- This is a broad static source finding, not proof every device actually DMA-writes before reset. The safe contract must not rely on firmware leaving every controller idle.
- Added F-030. Future remediation should stage IO/MEM decode, controller quiesce/reset, DMA memory preparation/programming, Bus Master grant/readback, then start. The grant must also pass the IOMMU/DMA policy gate.
- No driver source changed; the remaining current-main DMA source review and test matrix are still part of Stage 0.


## 39. CI diagnostic latency and QEMU log-volume audit — 2026-11-14

- Read the pinned-main ci/kira_vm.py runner. Every x86 VM currently launches QEMU with -d guest_errors and writes all guest-error output to qemu.log.
- The UHCI hang produced a 4.18 GB qemu.log in a 90.9 s run while serial stopped during PCI enumeration. This artifact is useful for root-cause analysis but expensive to store, transfer and inspect; the ordinary test harness waits the scenario's default boot_timeout=90 before failing.
- The runner already supports per-scenario boot_timeout, and its VM configuration is centralized in VM.args(). A future diagnostic profile can set a short boot_timeout (10–15 s) and optionally disable verbose QEMU guest-error logging; full acceptance scenarios keep their existing complete timeouts.
- Recommended harness improvement after Stage 0: add a scenario/CLI switch for QEMU guest-error logging, plus a dedicated short PCI/USB preflight scenario. Keep one focused evidence run with guest_errors enabled when needed; use serial/register assertions for routine regressions to avoid multi-gigabyte artifacts.
- Do not shorten the full acceptance USB scenario; the short profile is only a diagnostic gate. Final T4 CI retains all current acceptance checks.


## 40. KDRV Ring 3 DMA authorization and startup sequence audited — 2026-11-14

- Read current-main kdrv.c start(), quiesce(), sys_dma_alloc() and KDRV_SYS_CFG_WRITE, then compared with the in-tree RTL8139 driver.
- Found that KDRV start() enables PCI_CMD_MASTER before proc_spawn_driver() whenever CAP_DMA or CAP_IRQ is granted. This is earlier than the driver's BAR mapping, controller reset, DMA allocation and ring programming.
- CAP_IRQ is not a safe substitute for DMA authorization: PCI Bus Master is device-wide. An MSI write is a memory transaction, but enabling the bit for an IRQ-capable driver also gives the requester general bus-master access; absent an IOMMU domain, that cannot be restricted to just the MSI message.
- sys_dma_alloc() allocates contiguous identity-mapped memory and tracks the pages in the binding, but does not program/establish an IOMMU domain restricting the requester to those pages. This must not be described as DMA isolation.
- RTL8139's own order is map BAR → allocate RX/TX buffers → reset/read MAC → initialize ring addresses and enable RX/TX, but the manager has already enabled Bus Master before the driver starts. This manager-level grant defeats the otherwise safer driver order.
- Added F-031 HIGH. Required fix is staged decode/IRQ/DMA permission, registered DMA ownership, verified IOMMU domain or fail-closed policy, bounded quiesce, and Bus Master readback/revocation. No source code changed.


## 41. KIRA Mind authority and memory-pressure kill policy reviewed — 2026-11-14

- Reviewed Mind process profiling, ACT-mode scheduler tuning, adaptive limits and memory() policy, plus mind_sys callback wiring and proc_kill().
- Current implemented Mind is a rule/profile-based adaptive resource controller: it learns per-machine norms, compares process syscall behavior to saved profiles, tunes scheduler quantum, adjusts weights/limits, manages graphics load and memory pressure. The inspected source does not establish a general neural/LLM reasoning engine.
- At 97% memory usage in ACT mode, Mind can kill the process with the largest heap if it uses at least 16 MiB and at least 20% of total RAM, throttled to one kill attempt per 10 seconds. Callback goes directly to proc_kill().
- The inspected process sample and policy have no explicit essential/non-killable service class, foreground/user-work veto, pending-write/transaction check or restartability requirement. Thus the current candidate selection can choose a critical service or unsaved application if it happens to be the largest process.
- Added F-032 HIGH policy/reliability finding. Required closure: kernel-owned process classes, protected services, reversible reclaim first, safe termination/restart vetoes, observe-only deployment and adversarial selection tests.
- This does not claim a concrete process was killed in a VM; it records the source-level authority gap. No code changed.


## 42. FAT/VFS persistence and mount-table concurrency audit — 2026-11-14

- Inspected FAT mount/write/replace/mkdir/unlink/flush paths, KiraFS write/rollback paths, block-device flush wrapper, VFS mount/unmount and tests.
- Confirmed F-033: fat_flush() propagates FAT-sector and FSInfo write errors but discards the underlying device flush callback result and returns success. vfs_flush() therefore can falsely report durable completion. Existing FAT host fixture has no flush callback; separate store_test flush-failure coverage does not test FAT.
- Confirmed F-034: per-slot locks protect mount/unmount and volume operations, but vfs_any() and the root branch of vfs_list() read mount slot state without locks. Concurrent unmount/reuse can yield an inconsistent root listing/availability result.
- KiraFS write path has a stronger copy-on-write-like ordering for file replacement: new data and metadata are flushed before inode switch, then old blocks are freed. KiraFS still has no general journal/atomic transaction layer, so crash tests and mount repair remain essential.
- Added F-033 HIGH and F-034 MEDIUM to source findings. No filesystem code changed.


## 43. Current exact-head CI and UHCI failure boundary refreshed — 2026-11-14

- Rechecked current PR metadata and exact commit workflow runs.
- PR #165 head 7ad140cfadb51c4dacf809eca77fff60ff7fe12c, run 43613588426: 29/30 success, only vm (usb-uhci) fails (job 132504574309). Re-downloaded artifact 14121317727 and inspected the exact serial/result: serial stops after "[PCI TRACE] probed 0:1.2"; no "[PCI TRACE] secured 0:1.2"; prompt not received after 90.9 s. qemu.log is 4,187,160,076 bytes.
- Compared exact pci.c source at that PR head: after probe_bars(), scan_fn reads PCI Command and conditionally writes it with PCI_CMD_MASTER cleared; only after that does the "secured" marker print. Current evidence localizes the stall to this read/conditional write path, but the trace does not distinguish the config read from the config write. The invalid DMA storm occurs concurrently. Next run needs two separate markers and the PCI Command value.
- PR #166 current head 4bc69a224887ead8d8d1737e782ed76cf4b8e95e, workflow 43610515593: 30/30 success. Earlier workflow 43608492629 was cancelled on an older head and is superseded.
- PR #151 current head d2a53c8847bf40525780bb2624ea42efe9ba287a, base still main 811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4. No workflow run returned for this docs-only commit; no CI PASS is claimed.
- No code changed in this diagnostic cycle.


## 44. Test-to-component matrix and staged CI strategy added — 2026-11-14

- Created docs/architecture/STAGE0_TEST_MATRIX_2026-11-14.md from the actual current-main tests/run_host_tests.sh, ci/scenarios.py, .github/workflows/ci.yml and ci/arm64_vm.py.
- Mapped current host tests and x86-64/ARM64 QEMU scenarios by subsystem, explicitly recording skipped/conditional coverage (FAT depends on dosfstools/mtools) and the difference between host models and production drivers.
- Added targeted test requirements for F-017/F-025/F-029…F-034: PCI command ordering, firmware-active DMA controllers, KDRV CAP_IRQ/CAP_DMA gates, Mind termination vetoes, FAT flush fault injection and VFS mount-table races.
- Defined T0–T6 order: static/source → focused host → short diagnostic QEMU → subsystem acceptance → cross-ISA → full exact-head CI → PR integration compatibility.
- The 10–15 second QEMU diagnostic profile and optional guest-error logging switch are recommendations only; current harness has not been changed. Full 90-second acceptance scenarios remain required.
- No production source changed.


## 45. Driver Manager / drvget capability and provenance audit — 2026-11-14

- Reviewed drvget_core.c, drvget.c, drvman.c, KDRV record format, RTL8139 recipe, unit tests and drvget end-to-end harness.
- Confirmed current behavior is a real but constrained source-porting workflow: HTTPS/TLS 1.3 recipe + source downloads, optional per-file pins, allow-listed license scanning, manual combined SHA-256 confirmation, KCC compilation on the target OS, KDRV package and Ring 3 launch. It rejects ELF/PE/archive binaries and GPL/unknown licenses.
- It is not a universal Linux/Windows driver adapter: no arbitrary foreign binary loading, Linux/Windows ABI translation or automatic rewrite of any driver. The present compatibility is limited to supported C source recipes and the BSD compatibility header; P-06 remains planned.
- Found F-035: the install report shows per-file URL/license/hash, but the installed KDRV record preserves only recipe URL, recipe-declared license, combined source-set hash, package hash and ELF hash. Per-file provenance is not retained. The parser also does not require detected per-file license markers to match the recipe-level declared license.
- The minimum accepted manual confirmation is 8 hex characters (32 bits); this is a user confirmation token, not a cryptographic signature. The current flow has no source signature/key rotation/revocation layer.
- Added F-035 MEDIUM. Recommended versioned per-file provenance manifest, explicit license-set consistency, stronger default confirmation length and separate signed provenance. No source code changed.


## 44. Network socket lifecycle and cryptographic API boundary audit — 2026-11-14

- Reviewed current-main UDP/TCP syscall ownership, process teardown, packet delivery, RNG and cryptographic primitive APIs, plus existing network/crypto tests.
- Confirmed F-035: UDP allocation/close and packet delivery use ulk, but udp_gone() revokes owners without ulk and uown() reads owner without synchronization. A teardown/close/reopen race can invalidate a slot while another syscall or packet-delivery path is using it. tcp_owner[] and listen_owner[] need the same lifecycle audit.
- Confirmed F-036: HKDF-Expand uses an 8-bit block counter but does not enforce RFC 5869's 255-block maximum; >8160-byte output wraps the counter. It also silently truncates info to 300 bytes. TLS HKDF label/context are silently truncated at 64 bytes and encoded output length is not range-checked. Existing crypto tests cover standard vectors, not these boundaries.
- Also flagged ChaCha20/AES-GCM maximum-length/counter checks for the crypto hardening pass; no exploit in a current TLS caller is claimed.
- Existing tests/net_test.c is narrow (checksum and address parsing); it does not exercise socket ownership/teardown concurrency. No production source changed.


## 45. TLS reentrancy and X.509 critical-extension policy audit — 2026-11-14

- Reviewed TLS handshake state, certificate parsing, chain verification and OCSP scratch ownership.
- Confirmed F-037: TLS transcript/hash/certificate/OCSP scratch buffers are file-scope static mutable state, and X.509 chain/OCSP validators also use static mutable scratch objects. Concurrent connections/verifications in threads of one process are not isolated by tls_t contexts; no reentrancy lock or single-connection API contract was found in the inspected headers.
- Confirmed F-038: parse_ext() reads but ignores the critical BOOLEAN and silently skips unknown extension OIDs. basicConstraints stores CA=true/false but ignores pathLenConstraint; chain validation checks is_ca without enforcing path length. keyUsage/serverAuth EKU also require explicit acceptance-policy review.
- Existing x509 tests cover several positive and negative chain/signature/hostname/time/OCSP cases, but not critical-extension rejection or CA path-length constraints.
- No production source changed.


## 46. TCP state-machine locking and handle lifetime audit — 2026-11-14

- Followed TCP packet input/tick updates under tlk against syscall paths and wait predicates.
- Confirmed F-039: tcp_wait() evaluates TCB condition callbacks without tlk while packet/timer paths mutate those fields under tlk. Additional unlocked reads/writes exist in tcp_send(), tcp_connect_ipa() timeout cleanup, tcp_peer() and tcp_accept() listener pre-check. Connection/listener ownership teardown has no complete slot-pinning/generation contract.
- Existing tests/net_test.c only exercises checksums and IPv4 address parsing; it does not test TCP concurrency or socket lifecycle.
- Required closure is a single TCB/listener synchronization contract, lock-safe wait predicates, slot pinning/generation handles, and concurrent lifecycle tests. No source code changed.


## 47. Exact GitHub Actions recheck: PR #125 / PR #165 — 2026-11-14 16:45 UTC

- PR #125 run `43613583149`, exact head `fee8c9aed19ffc37dab7fd599c2bd63bfc496924`, completed **failure**: 20 success, 1 failure (`vm (0)`), 2 skipped (`package`, `preview-package`). In shard 0, four scenarios passed and `usb-uhci` failed after 90.7 s. Artifact `14120979815`; serial ends after `disable BM 0:1.2`, before `BM disabled`. PR #125 targets `autodrv`, not main.
- PR #165 run `43613588426`, exact head `7ad140cfadb51c4dacf809eca77fff60ff7fe12c`, final conclusion **success** on attempt 2. Attempt 1 had 41/43 jobs success; `vm (usb-uhci)` and `report` failed. UHCI timed out at 90.9 s; artifact `14121317727` serial ends after `probed 0:1.2`, before `secured 0:1.2`. Attempt 2's latest job list is 30/30 success, including UHCI in 11.7 s (job `132514447432`, artifact `14121774090`). Same exact SHA on both attempts; preserve the failed attempt as history and do not describe the retry as a proven fix.
- In both failed artifacts, QEMU guest-error output begins with repeated rejected reads at `0x0/0x4/0x8/0xC`. This supports, but does not prove, a live invalid UHCI DMA fetch. PR #125's source disables Bus Master before BAR probing; PR #165's source probes BARs first and then reads/clears PCI Command. Their trace boundaries are different and do not identify the exact failing config transaction.
- PR #125 current state: open, not Draft; base `autodrv` SHA `e2c55cec08a972187953dce17fcb0e231e382c1a`. PR #165: open Draft; base main SHA `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`. Neither merged.
- Stage 0 stays **43%**: the network/TLS audit had already advanced it from 40%; this CI recheck closes no new audit domain. No production source changed; main untouched; no merge or ISO build.
- Detailed per-job status lists and source/trace comparison: [Stage 0 repository audit, section 12](architecture/STAGE0_REPOSITORY_AUDIT_2026-11-14.md#12-exact-ci-attempt-history-and-uhci-comparison--2026-11-14-1645-utc). Next: instrument PCI Command read/write separately and capture UHCI `USBCMD`, `USBSTS`, `FLBASEADD` before handoff.


## 47. KCC compiler, ELF backend and Ring 3 runtime audit — 2026-11-14

- Reviewed KCC lexer, preprocessor, parser/type system, IR lowering, x86-64/AArch64 code generation, ELF emission, compiler CLI and Ring 3 compiler integration.
- Confirmed F-040: kcc_compile() uses singleton global KCtx; preprocessor, parser, IR and both backends also keep mutable file-scope state. Concurrent calls in one process are not isolated and can corrupt each other's compile state. The shipped cc app is currently single-invocation, so this is an API concurrency defect rather than a demonstrated reachable application exploit.
- Reviewed the KCC acceptance harness: it builds with ASan/UBSan, compares C acceptance and constant values against clang, checks host execution output for x86-64 and AArch64, exercises native compiler app integration, malformed inputs and 20,000–100,000-token stress cases, plus fuzzing (default 3,000 iterations). This review inspected the harness but did not execute it; no PASS is claimed.
- Confirmed compiler feature boundary: the test harness explicitly separates supported C, strict rejections and valid-but-unsupported constructs; current compiler is a substantial C subset, not a complete ISO C compiler.
- Reviewed Ring 3 cc filesystem sandbox/header access and growable arena. No production changes made.


## 48. WebAssembly interpreter/JIT binary validation audit — 2026-11-14

- Reviewed WASM 1.0 module loader, LEB128 decoding, instruction-immediate skipping, section parsing, function-body compilation, memory/table setup, interpreter and x86-64 JIT entry/guard paths.
- Confirmed F-041: multiple nested parser fields and skip_imm() advance pointers without checking the current section/body end; leb_s() can return a partial value when a continuation byte is truncated. This creates out-of-bounds-read risk for malformed user-supplied modules. No runtime exploit is claimed.
- The runtime has meaningful existing limits and traps (module size cap, bounded memory, value/call stacks, JIT sealing and interpreter fallback), and tests include differential operation/trap generation and damaged-module fuzzing. However, this review did not prove exhaustive truncation coverage and did not run those tests.
- No production source changed.


## 49. WASM import ABI and entry-point validation audit — 2026-11-14

- Followed the import section through export selection and runtime entry dispatch.
- Confirmed F-042: imported function type indices are stored without checking `ti < ntypes`; imported functions can be exported as the program entry; final entry validation checks only the function index, then umain dereferences `types[funcs[entry].type]`. This permits an out-of-bounds type-array dereference on a crafted module.
- Host import signatures are not validated against the fixed KIRA host ABI. `host_call()` accepts up to 32 declared parameters but captures only the first four in its local argument array; mismatched imports can therefore invoke host operations with unintended argument/result layouts.
- Capability boundary: the wasm executable is granted CAP_GFX, CAP_GRANT and CAP_JIT; host imports expose only the documented KIRA write/sleep/ticks/exit/graphics surface. The module itself remains inside its Ring 3 process address space. No production code changed.
- Reviewed ELF parser/loader and existing tests/prog_test.c: segment mapping is page-aligned, W^X and window bounds are enforced, overlapping pages rejected, stack is RW-NX, and 20,000 corruption mutations are tested. Header `e_ehsize`/`e_version` and PT_LOAD `p_align` are not validated; record as hardening follow-up, not a demonstrated loader escape. No host tests were run in this pass.


## 50. System-call authorization and user-memory boundary audit — 2026-11-14

- Reviewed syscall capability dispatch and authorization table, all user-facing syscall dispatch branches, `copy_from_user`, `copy_to_user`, `user_range`, futex user-word access, process capability assignment, trust digest checking and the wasm executable's trusted capability profile.
- Confirmed the intended OR semantics for combined rights (e.g. CAP_NET or CAP_NET_SERVE) are explicit in the dispatcher comment and applied as `!(caps & need)`. Filesystem authorization remains delegated to operation-specific checks in fsrv; driver syscalls use device-bound capability checks.
- User-memory copies validate the complete range before architecture-specific fault-contained copy; stack growth is retried through `proc_stack_prepare`. `user_read32` requires aligned, mapped user memory before the architecture-specific read.
- No new confirmed privilege-escalation finding in the reviewed syscall/uaccess paths. This is a bounded source review, not proof of all syscall implementations; network, FS, audio, IPC, graphics and KDRV subsystem internals retain their own findings and acceptance gates.
- No production source changed and no host tests were run in this pass.


## 51. Audio stream lifecycle and synchronization audit — 2026-11-14

- Reviewed the software mixer, per-process stream table, resampler write path, mixer consumption, close and process-teardown paths, and capture entry points.
- Confirmed F-043: `audio_sys()` validates a reusable stream slot without the stream lock and uses it without pinning; close/process teardown can release the slot while an operation is in flight, allowing a reused slot to be touched by a stale operation. Gain writes also race with mixer reads, and resampler/format fields are not covered by one consistent synchronization contract.
- Exact source evidence: `audio.c:120-143` (mixer reads under `lk`), `224-262` (syscall validation and operations), `333-336` (process teardown under `lk`).
- Required fix is a pinned/generation-tagged stream handle with close/drain semantics and one lock contract for resampler and queue state. Add concurrent write/gain/close/exit/reopen tests and TSan.
- This is a source audit only; no audio tests or runtime race reproducer were executed in this pass. No production source changed.


## 52. IPC objects, port waiters and shared-memory lifetime audit — 2026-11-14

- Reviewed `kobj.c`, process handle-table operations, shared-memory mapping/unmapping, port send/receive and the syscall entry paths.
- Positive source evidence: `proc_handle_get()` retains the kernel object while holding the process handle-table mutex; close removes the handle under that mutex and releases the reference afterward. Shared-memory mappings retain an object reference independently of the descriptor. Port queue and waiter-list mutations use the per-port lock, and receive waiters are removed or awakened under that lock.
- No confirmed use-after-free in the reviewed object-reference and waiter paths was established from this source pass.
- Remaining security contract: named ports and shared-memory objects are global by name, and the reviewed open path has no per-object owner/ACL check. This may be intentional public IPC, but the architecture must explicitly define whether names are public rendezvous identifiers or protected capabilities; add negative cross-process access tests for the intended policy before treating IPC isolation as complete.
- The `kobj_count()` / `port_queued()` diagnostic counters also need a documented synchronization/atomic-read contract if called concurrently.
- Source review only; no IPC stress, race-detector or QEMU tests were run in this pass. No production code changed.


## 53. Input queue SMP synchronization audit — 2026-11-14

- Reviewed PS/2 keyboard/mouse decoders, multi-source producer serialization, keyboard and mouse rings, two-consumer keyboard access, raw-byte diagnostics and overflow accounting.
- Confirmed F-044: keyboard producer reads `tail` non-atomically while consumer advances it atomically under a different lock; mouse producer similarly reads `mtail` non-atomically while consumer atomically advances it under another lock. The `dropped` diagnostic counter is also read without synchronization.
- Existing input tests cover decoder correctness and sequential overflow only; no concurrent SMP stress or TSan run was performed.
- Required fix: consistently atomic ring indices, atomic/locked diagnostics, then concurrent multi-producer/multi-consumer stress and QEMU SMP validation.
- Source audit only; no production code changed.


## 54. Graphics event consumer concurrency audit — 2026-11-14

- Reviewed graphics syscall event dequeue, event producer path, compositor-owned slot processing and process thread creation.
- Confirmed F-045: a process may have multiple user threads, but `gfx_sys_event()` has no per-slot consumer serialization. Concurrent threads can read the same queue tail, copy the same event, and publish the same tail increment, causing duplicate delivery and lost dequeue progress.
- Existing graphics tests do not exercise concurrent same-PID event consumers. No runtime stress or race-detector test was run.
- Required fix: serialized dequeue/commit or a strictly enforced single-consumer API contract, plus multi-thread same-process event stress and copy-failure/lifecycle coverage.
- The independent app-slot reclamation/lifetime race remains under review; no conclusion is claimed for that path.


## 55. Graphics service teardown and framebuffer lifetime audit — 2026-11-14

- Reviewed app-slot lookup, graphics syscall use, process-exit notification, compositor destruction, double-buffer physical allocation and VMM shared mapping behavior.
- Confirmed F-046: graphics lookup returns an unpinned pointer into a reusable global slot. Process exit schedules asynchronous destruction; the display server frees framebuffer pages and clears the slot without waiting for syscalls that already acquired the pointer. This creates a credible use-after-free/stale-slot race under SMP.
- The service destruction path does not itself show explicit user unmap/TLB shootdown before physical page reuse; this must be resolved in the lifetime protocol, not assumed safe.
- No runtime reproducer or SMP QEMU test was run. Source audit only; no production code changed.


## 56. KiraFS and FAT persistence/error-path audit — 2026-11-14

- Reviewed KiraFS v2 superblock validation, mount scan/repair, inode/data block allocation, direct/indirect/double/triple-indirect trees, directory operations, write replacement, unlink and crash-cutoff tests.
- KiraFS has useful host fault-injection coverage: writes can be cut off sector-by-sector and operations are remounted/rechecked. But the test flush callback always returns success and host-test locking is a no-op; flush-failure and concurrent-operation behavior remain untested.
- FAT audit confirmed F-033 remains open: `fat_flush()` discards the block-device flush return value and returns success.
- F-047 — FAT allocation may leave a partially allocated chain behind when `fat_set()` fails; some `chain_alloc()` error paths return without rollback, and `fat_write()` does not have an independent allocation list for cleanup.
- F-048 — FAT unlink and replacement discard `chain_free()` errors after directory metadata has been changed, allowing incomplete reclamation to be reported as success.
- Existing FAT test device has no flush callback and its write-failure check does not inject failures at each FAT-sector transition; it therefore does not close these gaps.
- No runtime fault-injection suite was executed in this audit turn. Source review only; no production code changed.


## 57. ARM64 architecture parity and physical allocator audit — 2026-11-14

- Compared ARM64 boot/EL transition, exception vectors and frame layout, context switch, FPU state, user-access fault fixups/PAN, TTBR0/ASID lifecycle, page-table teardown/TLBI, PMM and SMP paths with the shared x86-64 contracts.
- Confirmed several parity features exist in source: per-thread SIMD state save/restore, EL0 SVC routing, fault-contained LDTR/STTR user copies, PAN where supported, ASID-tagged TTBR0 and inner-shareable TLB invalidation.
- F-049 — ARM64 PMM uses one bitmap for both allocated and reserved pages. Consequently `pmm_free_page()` can release a reserved page; `pmm_free_contiguous()` frees sequentially and can partially release a range before an error. This violates the explicit shared ADR-0003 contract.
- F-001 already records the separate ARM64 kernel-stack free-list ownership issue; it is not duplicated here.
- QEMU ARM64 CI scenarios are defined in `ci/arm64_vm.py`, including SMP, EL2, GUI, storage, USB, crash and DMA-pool tests. This source review did not execute them or establish physical-board support.
- No production code changed and no runtime tests were run in this audit pass.


## 58. ARM64 DMA coherency and FDT parser security audit — 2026-11-14

- Traced non-coherent ARM64 detection, pool carving/reservation, DMA allocator behavior and Ring 3 DMA guard. Confirmed F-050: VirtIO and USB allocators fall back to ordinary cacheable PMM pages when the non-cacheable pool is absent or exhausted; the platform coherency query itself is derived from pool existence, so pool-creation failure is misreported as coherent to the Ring 3 guard.
- This can invalidate DMA data visibility on non-coherent boards. No runtime data-corruption reproduction was run; the finding is source-confirmed and requires forced-noncoherent fault-injection/QEMU validation.
- Confirmed F-051: FDT property-name comparison is unbounded after checking only that its offset is inside the strings block. A missing NUL can make lookup read beyond the bounded block. Existing FDT tests do not cover this malformed-name case.
- ADR-0004 remains a separate mandatory IOMMU policy gate; this audit does not treat the DMA pool as isolation.
- No production code changed; no tests run in this source-only pass.


## 59. ARM64 device-tree DMA coherency scope and controller sequencing — 2026-11-14

- Extended the driver audit across UHCI/xHCI, AHCI, NVMe, e1000, HDA, AC'97 and VirtIO (block/net/GPU/input/RNG). Confirmed the existing F-030 class of issue across multiple built-in drivers: Bus Master is enabled before all DMA structures are allocated/programmed and before controller reset/quiesce is complete. This remains an existing finding, not duplicated as a new one.
- Confirmed F-050 is shared by both generic DMA allocators: VirtIO and USB fall back to ordinary PMM pages after the ARM64 non-cacheable pool returns NULL.
- Added F-052: when no PCI host node exists, ARM64 coherency detection scans all FDT nodes and treats any one node's `dma-coherent` property as platform-wide coherency. This can misclassify unrelated devices on mixed-coherency boards.
- Reviewed FDT validation/tests: test fixture covers header offsets, malformed property payload length and normal QEMU-virt-like tree, but no unterminated property-name or mixed-coherency negative fixture.
- No production source changed; no tests run in this source-only audit pass.


## 60. ARM64 PSCI power-control path — 2026-11-14

- Reviewed PSCI detection and CPU_ON, secondary entry, SYSTEM_OFF and SYSTEM_RESET call sites.
- Confirmed F-053: `psci_hvc == -1` means PSCI unavailable, but `psci_call()` tests it as a boolean; -1 selects HVC. CPU_ON is guarded, while poweroff/reboot call it unconditionally. This can execute an unsupported HVC when firmware has no PSCI.
- Also noted platform status currently prints PSCI unconditionally; correct reporting belongs in the same fix and tests.
- Source-only finding; no runtime power test was run and no production code changed.


## 61. PCI/UHCI artifact re-analysis — 2026-11-14

- Downloaded and inspected exact PR #125 failing shard artifact and PR #165 successful UHCI artifact, rather than relying only on workflow summary.
- PR #125 run 43613583149 failed shard 0: `usb-uhci` timed out at 90.7 s. Serial stopped at `disable BM 0:1.2` before the completion marker. QEMU log: 4,915,946,761 bytes; 72,286,473 rejected invalid-read messages (0x0/0x4/0x8/0xC recurring; first anomalous address 0x3FFDE384).
- PR #165 run 43613588426 latest attempt 2: 43/43 jobs success. UHCI scenario 11.7 s / boot 5.8 s; keyboard, hub, USB mass storage, FAT32 mount and file read all confirmed in serial/result artifact.
- Important: PR #165 passing artifact still contains 393,602 rejected invalid-read messages in a 26,767,488-byte QEMU log. Thus the scenario's functional PASS does not close F-025; exact DMA requester/register origin is still unproven. These runs are not controlled A/B because branches and PR bases differ.
- Updated audit disposition: PR #165 CI latest attempt PASS; F-025 remains OPEN; no source implementation changed.


## 62. CI result reconciliation — full pagination and ARM64 artifact review — 2026-11-14

- Corrected the previous partial-page count: full jobs API enumeration shows runs #154 (43611654110), #164 (43600699384), #166 (43610515593) and #165 (43613588426 attempt 2) each completed 43/43 jobs successfully.
- PR #165 includes successful host tests, x86 build/VM matrix and ARM64 job. Downloaded artifact 14121710332 and verified all 16 ARM64 scenarios report OK.
- Confirmed actual ARM64 coverage: GICv2 and GICv3+ITS; PSCI HVC and SMC paths; 8-core EL2 entry; GUI and USB; fault/isolation; non-coherent DMA with a 32 MB pool. This is QEMU evidence, not physical hardware.
- Negative cases remain absent: PSCI unavailable, FDT unterminated property-name, mixed DMA coherency and non-coherent pool exhaustion. Their findings F-050..F-053 remain open.
- x86 UHCI passing artifact still emits 393,602 rejected QEMU invalid-read records; F-025 remains open despite functional test pass.


## 63. USB controller Bus Master re-enable ordering — 2026-11-14

- Compared PCI enumeration mitigation with actual UHCI/xHCI driver start sequences.
- Found F-054: UHCI enables Bus Master before schedule allocation, FRBASE programming and controller stop/reset; xHCI enables Bus Master before BAR mapping, BIOS handoff, stop/halt, reset and ring allocation. Both have post-enable failure exits without a shared quiesce/disable rollback.
- This is a strong candidate explanation for the remaining QEMU invalid-read DMA records because firmware may leave a controller running with stale pointers. It is not yet proven: no QEMU register/requester trace ties the messages to UHCI.
- No production changes or tests in this pass. Next verification is register-level trace and host ordering/failure-injection tests before changing controller code.


## 64. QEMU invalid-read log attribution audit — 2026-11-14

- Inspected the actual PR #165 UHCI success artifact locally, not just its workflow summary. The result JSON confirms keyboard/tablet, USB storage, FAT32 mount and file-read checks all pass.
- Independently counted 393,600 QEMU `Invalid read` records: 384 unique addresses across 0x3FFDE000–0x3FFDE2A8 and 0x3FFDECAC–0x3FFDEFFC, plus 98,304 reads each at 0x0/0x4/0x8/0xC.
- Found F-055: `ci/kira_vm.py` starts `-d guest_errors` before firmware and never records a byte offset/phase boundary at the OS-ready serial prompt. Therefore the artifact cannot establish whether the records come from firmware, PCI enumeration or KIRA's USB runtime. Previous wording that implied the log necessarily represented guest-driver DMA was too strong; attribution remains unresolved.
- Updated the next diagnostic requirement: preserve full log, add pre-ready/post-ready segmentation and a no-USB negative control. F-025 remains open; F-054 remains an independently confirmed source-ordering hazard.
- No production source changed and no tests run in this pass.


## 65. xHCI slot allocation failure/retry audit — 2026-11-14

- Found F-056: xHCI allocates per-slot IN context, OUT context, transfer buffer and EP0 ring sequentially. On partial failure, `usb_dev_free()` disables the slot but leaves the partial pointers in `xs.slot[sl]`.
- Retry uses only `xs.slot[sl].in` as the initialization sentinel. If IN succeeded but OUT failed, the retry takes the initialized branch and clears `xs.slot[sl].out[i]` through NULL. A missing transfer buffer/ring can also leave a slot marked initialized without complete resources.
- The current DMA allocator has no public free operation, so failure recovery must explicitly preserve/reuse partial resources or introduce a real DMA free contract; simply zeroing the slot would leak already allocated pages.
- Source-only finding; no production code or tests changed in this audit pass. Required regression: fail each allocation stage, retry same slot, and verify safe reuse.


## 66. Phase-aware QEMU baseline and PCI/UHCI differential — 2026-11-14

- PR #168 exact head `7ad4b30cf9e7cbb2cefaecaa06359838c5971841`, run `43620850523`: 43/43 jobs passed. Inspected actual artifacts for `usb-uhci` and `boot-virtio`; each contains `qemu-diagnostics.json`, a zero-byte raw `qemu.log`, and 0 invalid read/write records on either side of the ready marker.
- Compared against PR #165 run `43613588426`, whose UHCI artifact contains 393,602 invalid-read records despite a functional pass.
- This strengthens, but does not prove, the F-054 hypothesis that the PCI firmware Bus Master clear/re-enable sequence in #165 exposes a controller's stale firmware DMA schedule before reset. The runs are different source heads, not a controlled A/B.
- PR #169 is stacked on #165 and moves BM activation after controller reset and DMA ring/schedule setup; its exact-head host/QEMU results will decide whether this hypothesis is supported. No production fix is claimed before that verification.


## 67. QEMU xHCI immediate ERSTBA DMA evidence — 2026-11-14

- Inspected PR #169 first QEMU failure artifact, run `43621930596`. xHCI initialized but USB enumeration failed with command timeouts/no free slot.
- QEMU log explicitly reported `xhci_er_reset: DMA memory access failed!` and rejected reads at 0xE15000/04/08/0C, matching the ERST table address written by the driver.
- Root cause of this specific failure: the first fix delayed Bus Master until after ERSTBA was written. QEMU fetches the ERST table immediately on ERSTBA write, so the DMA permission was still disabled. Corrected the ordering: allocate/initialize all DMA objects with BM off; grant BM while xHCI remains halted; then write DCBAAP/CRCR/ERST pointers; finally RUN. The host model now checks that pointers are still unexposed at the BM grant point.
- Corrected exact-head CI run is pending. This validates the need for a device-specific activation boundary and refines F-054; it does not yet close it.


## 68. USB controller startup allocation-failure lifecycle — 2026-11-14

- Traced \x60usb_dma_alloc()\x60 and both controller startup paths after reviewing the stacked F-054/F-056 changes.
- Added F-057: UHCI and xHCI controller-level DMA allocations are not transactionally released on failed startup. Because \x60usb_dma_alloc()\x60 has no matching free API and \x60nctl\x60/\x60started\x60 advance only on successful startup, a failed controller can strand its schedule/rings/scratchpad pages while the next controller reuses the same index and allocates again.
- Existing xHCI failure injection covers device-slot buffers and EP0 retry, not controller-start allocation failures; UHCI tests cover successful start only. No test demonstrates restored DMA accounting followed by successful startup of another controller.
- F-057 is source/test-coverage only. No production code changed and no test is claimed as run in this audit pass. Main remains unchanged; no merge or ISO build.

**Stage 0 progress: 69%.** Added controller-start resource-lifecycle review and a concrete negative-path test requirement. The remaining 31% is not a count of unread files; it covers unfinished semantic review, cross-branch reconciliation, integration and verification gates.


## 69. FDT string-property bounds review — 2026-11-14

- Rechecked FDT parsing and existing host tests after the earlier property-name termination finding (F-051).
- Added F-058: `fdt_count_type()` and `fdt_node_enabled()` call unbounded `streq()` on `device_type` and `status` property values, despite `fdt_prop()` returning an explicit byte length and not validating NUL termination. Existing tests do not cover malformed unterminated values.
- Kept the finding scoped: `fdt_compatible()` already walks a bounded string-list value and is not included in this defect.
- No production source changed and no test is claimed as run. Main remains unchanged; no merge or ISO build.

**Stage 0 progress: 70%.** Added a distinct FDT string-value boundary review and a concrete malformed-input test requirement. Remaining work includes broader semantic review, branch reconciliation, and actual verification of the resulting integrated source.


## 68. Stacked USB fix QEMU artifact review — 2026-11-14

- PR #169 intermediate head `6fd90d39`, run `43622220409`: targeted UHCI and xHCI VM jobs both completed successfully before later instrumentation commits superseded the run.
- xHCI: all hub/keyboard/tablet/storage/FAT32 read-write checks pass; QEMU `qemu.log` is 0 bytes, with no invalid reads/writes and no `xhci_er_reset: DMA memory access failed`. The corrected BM-before-pointer-exposure order resolves the observed ERSTBA failure in this QEMU scenario.
- UHCI: functional USB and FAT32 checks pass, but `qemu.log` is 40,081,525 bytes with 589,375 invalid-read records; 147,200 reads each at addresses 0x0/0x4/0x8/0xC. F-025 remains open. The earlier UHCI BM-order fix alone did not remove the QEMU log storm.
- The current PR #169 branch adds serial/QEMU log offsets at PCI scan, UHCI/xHCI initialization and OS-ready to identify whether the remaining UHCI errors precede PCI securing, occur during controller startup, or continue at runtime.


## 69. UDP socket lifecycle audit — 2026-11-14

- Reviewed the current-main UDP implementation in `net.c`: `uown()`, `net_udp_sys()`, `udp_gone()`, `udp_deliver()` and `ulk`.
- Confirmed that owner validation is unlocked, process teardown clears owner unlocked, and operations do not pin or generation-check a slot after validation. A concurrent close/exit and immediate reopen can recycle the slot while an old syscall is still using its index.
- Confirmed `tests/net_test.c` only exercises pure checksum/IP parsing functions; no UDP ownership or lifecycle tests exist.
- Recorded F-059 with explicit lifetime, locking, lock-order and concurrency-test closure criteria. This is source evidence only: no runtime reproduction, production fix or test PASS is claimed.
- The existing F-039 TCP state/lifetime finding remains open and is not counted as fixed by this UDP audit.


## 69. UHCI firmware-controller quiesce gap — 2026-11-14

- Added F-057 after reviewing the phase-aware `usb-uhci` artifact and PCI scan source. PCI enumeration clears Bus Master but does not stop the firmware-running UHCI controller; the normal USB driver does not run until after the complete PCI scan.
- QEMU `usb-uhci` artifact contains 393,600 invalid reads at 0x0/4/8/C; same-branch `boot-virtio` no-USB control has no QEMU guest_errors. The UHCI log is fully populated by the driver-start marker and does not grow after guest-ready. File-size offsets are best-effort due QEMU buffering.
- Likely mechanism: the controller continues trying to fetch its old frame list while BM is revoked; QEMU rejects the accesses. This is not proof of successful DMA or memory corruption.
- Required next fix: quiesce UHCI immediately after BM revocation during PCI enumeration, with bounded HCHalted polling and mocked-port host tests. Keep BM off if quiesce fails.


## 70. UDP transmit error-path audit — 2026-11-14

- Traced the documented `netdev_t.send` return contract and both production NIC send callbacks (`e1000.c`, `virtio_net.c`): queue/buffer exhaustion returns a negative error.
- Confirmed `eth_send()`, `ip_send()` and `ip6_send()` propagate that error, but `udp_xmit()` discards the `ipa_send()` result and returns the payload length unconditionally.
- Recorded F-060 with required error propagation and fake-netdev IPv4/IPv6 tests. No runtime test or production fix is claimed.


## 71. Целевая архитектура и новая Stage-карта — 2026-11-15

- Создан `docs/architecture/KIRA_TARGET_ARCHITECTURE_AND_STAGE_PLAN_2026-11-15.md`: draft целевой гибридной capability-микроархитектуры и Stage 0–13.
- Зафиксировано распределение языков: C/KCC для текущего ядра и критического low-level кода; Assembly для узких ISA участков; Rust `no_std` как кандидат для новых изолированных служб/user-mode драйверов после проверки toolchain/ABI; WASM/WASI-профиль для переносимых приложений; Python/Bash для host tooling. SPARK/Zig остаются экспериментальными кандидатами.
- Зафиксированы кандидаты форматов/технологий по назначению: FlatBuffers/Cap’n Proto/CBOR для IPC и metadata; Arrow/Zstd/event log для аналитики; Parquet для долговременных аналитических данных; CAS/Merkle DAG/CoW/signatures для integrity/object storage; LSM/B+Tree — по benchmark.
- Проверен recursive Git tree pinned baseline `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`: 861 entries; 593 C, 165 headers, 15 assembly, 13 shell scripts, 11 Python scripts, 16 Markdown, 3 YAML. Tracked Rust manifests, schema files for proposed binary formats, and WASM/WAT modules were not found in this snapshot. Existing WASM runtime is C source; x86-64 JIT is not ARM64 JIT.
- Проверены `build_clang.sh`, CI workflow, `ARCHITECTURE.md`, README, ADR-0001 and master-plan sections. Existing baseline is Clang/LLD/NASM on x86-64 with a separate AArch64 build and host/QEMU test layers.
- Обновлён Stage 0 audit report ссылкой на новый план и явным разграничением integrated baseline vs proposed technology.
- Это документационная и inventory работа. Production code не менялся; новые технологии не объявлены внедрёнными; тесты и сборки в рамках этой записи не запускались.
- Stage 0 остаётся IN PROGRESS. Ранее записанные 70% — предварительная оценка старого объёма аудита; новая архитектурная/форматная сверка расширила критерии закрытия и требует повторного расчёта после reconciliation всех A–N workstreams.


## 72. Новые требования закреплены в нормативном мастер-плане — 2026-11-15

- В `docs/MASTER_DEVELOPMENT_PLAN.md` добавлен раздел 8: целевая гибридная capability-микроархитектура, языковая политика, правила выбора форматов, KIRA IR/SSA, технологические ворота и новая последовательность Stage 0–13.
- Создана `docs/architecture/STAGE0_REQUIREMENTS_CROSSWALK_2026-11-15.md`: все направления A–N сопоставлены с целевыми компонентами и Stage; отдельно закреплены общие контракты, распределение языков/форматов и дополнительные exit checks Stage 0.
- Связи добавлены в целевой архитектурный документ и Stage 0 audit report. Старые требования A–N не удалялись и не сокращались.
- Документы записаны только в рабочую audit-ветку `architecture/stage0-repository-audit-2026-11-14`; `main` не менялся. Production-код и сборочные файлы не затрагивались.
- Выполнена повторная проверка GitHub fetch: целевой план, master plan, crosswalk и audit report доступны в ветке; новые ссылки присутствуют.
- Проверки кода/сборки/CI не запускались в рамках этой документационной операции; PASS не заявляется.
- Stage 0 остаётся IN PROGRESS. Следующая работа — сверка A–N crosswalk с полным component/file/build/test inventory, затем продолжение source audit и PR dependency reconciliation.


## 73. Проверка актуального PR-стека WASM/Input/GFX — 2026-11-15

- Сверены открытые PR #179/#181/#182: #179 меняет `userland/wasm.c` и WASM host tests; #181 добавляет синхронизацию input ring; #182 исправляет публикацию reused app slots в gfx queue. #181 и #182 основаны на стеке #179.
- Скачаны полные GitHub Actions job logs для exact heads. PR #179 (`2fdb2c04`, run `43644011093`) имеет 42/43 успешных checks: host-tests FAIL. PR #181 (`17614b29`, run `43643351332`) имеет build, ARM64 и host-tests FAIL. PR #182 (`49e2914e`, run `43644247467`) имеет те же три группы FAIL.
- Общая причина по compiler logs: `userland/wasm.c:145` использует неопределённые `int32_t` и `uint32_t`. В #179 x86 build/ARM64/QEMU прошли, но host tests всё равно упали; в зависимых #181/#182 общий compile error ломает все три сборочных/test job.
- Добавлен F-062 как blocker открытого PR stack. Он отделён от current-main findings: это не доказательство дефекта pinned main и не основание считать production baseline сломанным.
- Исправление намеренно не внесено: Stage 0 фиксирует причину и зависимость; перед изменением веток нужно согласовать точку исправления общего предка и повторить exact-head проверки. PR не сливались и не переназначались.
- Тестовые результаты получены из GitHub Actions; локальные тесты не запускались. Текущая ветка аудита содержит только документационные изменения.


## #74 — Stage 0: complete open PR graph inventory (2026-11-15)

- Captured all **92 open PRs** and grouped them by 47 base-ref/base-SHA combinations.
- Added `docs/architecture/STAGE0_OPEN_PR_GRAPH_2026-11-15.md` with exact head/base refs and SHA, draft status, integration rules and explicit evidence limits.
- Confirmed `main` remains at `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`; audit work stays on the dedicated audit branch.
- This is metadata inventory only; all 92 diffs, ancestry, review threads and exact-head CI still require classification before integration.
