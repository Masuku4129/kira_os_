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
