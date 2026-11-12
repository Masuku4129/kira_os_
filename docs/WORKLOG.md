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
- Head: `aa1700b2cf9770eabeff2e480f714aaeb8e74682` (workflow concurrency refresh; code unchanged).
- Это накопительная ветка примерно с 80 файлами: BIOS/UEFI BootInfo и handoff, ARM64 FDT/GIC, ACPI PCI routing, IOMMU/KDRV trust, каталог драйверов, installer и release-gap документы. В ней также исправлено подключение корневого FDT header в `arch/aarch64/fb_arm.c`.
- Предыдущая проверка на head `13258d9...`: push run `43181790698` — build, ARM64, host-tests, KCC, audit, GUI, driver-e2e и все 10 VM jobs завершились success; report job оставался queued. На новом head добавлены failure-state cleanup в `kdrv_manager.c` и host lifecycle tests (`tests/kdrv_manager_test.c`, подключён в `tests/run_host_tests.sh`). Старые runs `43198527512` (pull_request) и `43198515530` (push) относятся к предыдущему head `6ace6455...`; на новом head свежий run пока не появился. Изменения ещё НЕ подтверждены CI. Не считать release gate закрытым до зелёного CI на новом head и итогового report.
- PR draft, не слит. Не объединять без разрешения владельца проекта.
- Граница Driver Manager: `drvget`/`drvman` реально включены в x86-64 и ARM64 userland build; `drvget` — ограниченная установка проверенных C/H source recipes по разрешённым лицензиям, а не универсальная конвертация Linux/Windows драйверов. `drvget_core.c` покрыт host tests и end-to-end сценарием TLS server → install → explicit confirm → KCC build → `/drv`. `drivers/kdrv/kdrv_manager.c`, `kdrv_catalogue.c` и `kdrv_trust.c` имеют отдельные host tests, но сами модули не подключены к production kernel build. `userland/drvcompat.c` остаётся отдельным callback-orchestration контрактом: не собран и не протестирован. Реальный source-recipe installer есть; универсальный foreign ABI/binary translator пока НЕ реализован.
- Граница IOMMU в этой ветке: новые `drivers/iommu/{arm_smmu,intel_vtd,amd_vi,*_domain,iommu_discovery}.*` не входят в `build_clang.sh`/`build_arm64.sh`; ARM64 `main.c` не вызывает новый SMMUv3 runtime. Host suite покрывает старый `iommu_vtd.c` parser и KDRV tests, но не эти новые runtime-модули. Это отдельный блокер интеграции.
- Отдельная ветка PR #125 ниже уже содержит другой путь интеграции SMMUv3 в ARM64 PCI startup и прошла CI на своём head; это не означает, что её код уже включён в PR #42 или main.

### PR #46 — unified release / native UEFI integration
- Ветка: `v46/unified-release-integration`; base: `v46/kdrv-catalogue-signature-policy`; head после изменения concurrency: `bfb77c809337025044276dad4850b7ca3f76e1f9`. PR draft, GitHub сообщает mergeable=false; не сливать.
- Предыдущий полный run `42575383245`: build, ARM64, KCC, GUI, driver-e2e, Mind, audit и UEFI OVMF success; 9 из 10 VM shards success; shard 4 failed на `wasm-speed`, `stress-3d`, `cc-native` с watchdog hang. Host-tests failed только на diff-lint: в research harness были пустой неиспользуемый `kdrv_sha_seed`, мёртвая ветка `if (argc > a + 2 && 0)`, плюс тестовый Rust ABI shim ошибочно попадал под production no-stubs scan.
- Удалены пустой helper и мёртвая ветка; `ci/lint.sh` теперь исключает `research/` из production stub scan, сохраняя проверку production C/H/ASM/Python/Shell. Последний run `43198443728` queued на head `bfb77c80...`; исправления ещё не подтверждены CI.
- VM shard 4: watchdog показывает kernel thread `core` sleeping at its wake tick with `mutexes_held=2`; все остальные ядра idle, watchdog прекращает исполнение. Это не признано решённым: нужно установить, какая ветка основного цикла оставляет два kmutex удержанными или почему scheduler не пробуждает core, затем повторить три сценария. Не маскировать timeout и не ослаблять тесты.
- UEFI OVMF прошёл в старом run, но это evidence только для того head; physical firmware/hardware validation отсутствует. Новый head требует полного CI.

### PR #95 — KCC context / self-host parser
- Ветка: `ai/kcc-context-more-state`
- Head: `f738656ef9a48c8766ef32ee2360beb03e0edbbb`
- Предыдущий head `4fdeb593582fb6a3e83d21e7f43dd6f6c67574cc` провалил self-host stage1: `ir.c:23 expected ')' before '->'`.
- Исправление: `#define IRS irs` вместо `#define IRS (irs)`, чтобы макросы полей раскрывались в `irs->field`, а не в `(irs)->field`.
- PR run `43201178221` queued; прямой self-host stage1/stage2/stage3 результат ещё не получен.
- Не объявлять KCC self-host исправленным до успешного stage1/stage2/stage3 прогона.

### PR #131 — KIRA WASM runtime validation
- Ветка: `fix/wasm-module-header-bounds`
- Head: `968b6b6b50452dbc19c6bd58fc0966115f759633`
- Усилены: проверка версии и границ секций; уникальность/порядок секций; типы функций и точные сигнатуры KIRA imports; лимиты памяти/таблицы; индексы функций/экспортов/глобальных; сегменты данных/элементов; тела функций и локальные объявления; запрет memory ops без memory; различение явно заданного max=0 и отсутствующего maximum.
- Добавлен `tests/wasm_loader_bounds.py` с отрицательными случаями и тестом `memory.grow` при max=0; подключён к `tests/run_host_tests.sh`.
- На текущем head создан push run `43201211841` pending; PR run ещё не появился. Последние изменения добавили строгую проверку LEB128, немедленных полей `call_indirect`/typed select/memory instructions и полное сравнение сигнатур `call_indirect`. Тесты ещё НЕ запускались/не подтверждены.
- Это первая hardening-порция, не полная WebAssembly type-stack verification и не заявление о полной WASM-конформности.
- Важное архитектурное различие: этот код — совместимый WebAssembly interpreter/JIT внутри Ring 3, НЕ собственная KIRA VM/ISA. Собственный portable KIRA bytecode, verifier, module ABI и native bridge из мастер-плана остаются отдельной незакрытой работой.
- PR draft, не слит.

### PR #81 — Mind memory-pressure foresight
- Ветка: `ai/mind-memory-pressure-forecast-v2`; base: `ai/mind-validity-feature` (зависит от PR #78, не является самостоятельной интеграцией в main).
- Head: `7465b0dfc67496ea58e93aca966d3a3e55ec5dec`. Добавлен отдельный 4-секундный прогноз давления памяти с собственным обучением/метками; пропуски временных отсчётов цензурируют незавершённые метки; память не меняется автоматически, прогноз наблюдательный.
- Дополнительно подключён к Mind UI и системной диагностике: `mind_view_t` публикует риск/доверие/число примеров/точность/полноту/число прогнозов; страница «Прогноз» показывает 4-секундный риск достижения 90% ОЗУ; страница «Обучение» показывает состояние memory-модели; `ai forecast` и сводка ядра показывают отдельную статистику памяти.
- Host tests уже покрывают метки через 4 секунды, отсутствие RAM telemetry, пропуск секунды, сохранение модели и запрет доверенного риска до достаточного обучения. Новые изменения UI/снимка пока не подтверждены CI.
- Дополнительно исправлена семантика метки: если текущая ОЗУ уже ≥90%, это не считается прогнозом будущего пересечения; модель не создаёт новые обучающие примеры из уже случившегося события, `mem_risk_valid` выключается, UI/консоль прямо сообщают, что порог уже достигнут. Добавлен тест этого случая.
- Уточнено качество входных данных: после boot/пропуска телеметрии первый валидный замер не используется ни для прогноза, ни для обучающей метки, потому что рост ОЗУ ещё неизвестен. Прогноз и обучение возобновляются только после второго последовательного замера; добавлены проверки разрыва временного ряда.
- Последний run `43201572080` pending на предыдущем head `4843fe22...`; head `7465b0df...` требует свежей проверки. Сборка, host-tests и VM/UI проверки ещё не завершены. Не заявлять PASS.

## Уже подтверждённые соседние проверки
- PR #125 ARM SMMU/PCI quarantine hardening: head `6074a9f3eeb8a51e63dccaeff3caaff103ddaa33`, base `autodrv`, non-draft and mergeable; run `43177373290` success across build, ARM64, host-tests, KCC, driver-e2e, audit, GUI, Mind, UEFI OVMF and all 10 VM jobs. This branch integrates SMMUv3 domains into ARM64 PCI startup; physical SMMUv3 validation is still absent and PR is not merged.
- PR #128 xHCI/DMA/scheduler hardening: run `43175213615` success.
- PR #120 SMMUv3 stage-1 domains: run `42852157626` success.
- PR #126 FDT cell-width validation: runs `43162737639` и `43162737625` success.
- PR #127 KiraFS persistence fail-closed: runs `43158217638` и `43158217643` success.
- PR #129 PCI bus-master gating: runs `43159900195` и `43159900302` success.
- PR #130 bootinfo physical RAM bounds: runs `43164087429` и `43164087483` success.
- Ранее упавший ARM64 timer сценарий PR #121 нужно сверять с более поздними ветками; в PR #128 ARM64 job завершился success.

### PR #131 — data-segment offset type-check call
- Независимый просмотр текущего `userland/wasm.c` обнаружил compile-time ошибку: `const_expr` получил обязательный третий аргумент `expected_type`, но data-section parser вызывал его только с двумя аргументами.
- Исправлено: data segment offset теперь проверяется как обязательный `i32` (`const_expr(&s, se, 0x7F)`), как требует WebAssembly MVP.
- Commit: `7c777f470294a5cf491e7d02b490ce98ab23ae37` на `fix/wasm-module-header-bounds`. CI на новом head ещё не получен; compile и host suite не подтверждены.
- Уже существующий негативный fixture `constant-expression-data-offset-type-mismatch` должен проверять отказ от f32 offset; после нового CI нужно убедиться, что fixture доходит до ожидаемой ошибки.

### Продолжение 2026-11-12, дополнительная проверка KVR/KCC
- PR #131, ветка `fix/wasm-module-header-bounds`: CI run `43183595676` (head до последних исправлений) — build success, ARM64 success, host-tests failure. Реальная причина: fixture `constant-expression-global-index-out-of-range` не содержал функции/экспорт и runtime завершался раньше сообщением `no exported main/_start`; fixture дополнен типом, функцией и экспортом. Последний head: `968b6b6b50452dbc19c6bd58fc0966115f759633`; CI run `43201211841` (push) pending. Не объявлять исправленным до полного прогона.
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
- Все эти исправления пока НЕ подтверждены повторным QEMU shard-4. Последний PR #46 head `aac05e9c1511776d40f5fbad2987e22b6d111d0e`; CI run `43200848712` pending.

### CI throughput — duplicate branch/PR runs
- Устранён источник лишней нагрузки: CI запускался одновременно на `push` и `pull_request` для одной feature-ветки, а concurrency groups различались по event/PR number.
- На `main`, `autodrv`, `v46/release-completion`, `v46/kdrv-catalogue-signature-policy` и `v46/unified-release-integration` concurrency group теперь использует source branch (`pull_request.head.ref || ref_name`), поэтому push и PR события одной ветки конкурируют за один актуальный запуск.
- На `main` push-trigger ограничен `main/stable`; feature-ветки проходят через PR checks, docs-only изменения не запускают полный стенд. На integration-ветках push-trigger сохранён для прямых изменений, но дубли сводятся к одной группе.
- Это уменьшает дублирование, но не устраняет очередь GitHub runner полностью. Новые workflow commits требуют завершённого CI для подтверждения корректности.

## Следующие действия
1. Дождаться CI PR #46 на head `aac05e9c...`; обязательно проверить полный QEMU matrix и shard 4 (`wasm-speed`, `stress-3d`, `cc-native`) после watchdog/scheduler исправлений.
2. Дождаться CI PR #95 на head `f738656e...`; если self-host падает, читать точный stage1 диагностический вывод и исправлять первопричину.
3. Дождаться CI PR #131 на head `968b6b6b...`; при падении исправлять реализацию/фикстуры и повторять host-tests, build, ARM64. Не оставлять известные ошибки.
4. Дождаться CI PR #81 на head `7465b0df...`; проверить Mind forecast UI, telemetry validity и memory label semantics.
5. После подтверждения веток продолжить независимый аудит WASM type-stack verifier, Driver Manager production compatibility backend, ARM64 SMMUv3 runtime и остальных потоков мастер-плана параллельно.
6. Не создавать ISO/update archive из непроверенных или неинтегрированных веток. Финальная поставка — после согласованной интеграции и полного регрессионного прогона.

## Правила доказательности
- Код создан ≠ собран; собран ≠ тесты пройдены; QEMU/Limbo ≠ физическое оборудование.
- Каждый PASS подтверждать конкретным run/job и commit SHA.
- PR остаются неслитыми, пока владелец проекта явно не разрешит merge.
