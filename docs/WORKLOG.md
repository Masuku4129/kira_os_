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
- Рабочая ветка PR #42: `v46/kdrv-catalogue-signature-policy`; текущий head после изменений: `6c144256a4143ca2a5fadd2a04b849be2b950b60`.
- В `arch/aarch64/fdt.c` исправлены три ошибки парсера загрузочного FDT:
  1. `reserved_count` использовался до инициализации при добавлении firmware reserve-map entries; теперь весь выходной `kira_fdt_info` обнуляется до парсинга.
  2. `#address-cells/#size-cells` из дочерней шины протекали в последующие sibling nodes; значения восстанавливаются при `FDT_END_NODE`.
  3. RAM-узлы вида `memory@<unit-address>` не распознавались, кроме частного имени `memory@0`; теперь распознаётся префикс `memory@`.
- Дополнительно reserve map теперь обязан иметь корректный нулевой терминатор в пределах FDT blob.
- Добавлен `tests/fdt_arm_parser_test.c`: входная структура заранее заполнена 0xA5; проверяются reserve map, восстановление ширины cell после вложенной шины и RAM node с unit-address. Тест подключён к `tests/run_host_tests.sh`.
- CI для head `6c144256...`: workflow run `43207377853` находился в состоянии pending при последней проверке; выполнение host test и ARM64 build ещё не подтверждено. PASS не заявлять.
- Следующие независимые направления: аудит WASM verifier/type-stack и quotas; KDRV production callback coverage и cryptographic publisher trust; SMMUv3 runtime/teardown; kernel scheduler/process lifecycle; native/compatibility runtime; KCC, Mind, KiraFS/network/GUI/build/recovery. PR не объединять без разрешения владельца.
