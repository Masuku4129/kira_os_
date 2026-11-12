# KIRA OS — журнал продолжения работ

> Снимок состояния на 2026-11-12. Перед использованием сверять SHA и CI с GitHub: статусы быстро меняются.

## Обязательный порядок
1. Сначала полностью прочитать `docs/MASTER_DEVELOPMENT_PLAN.md`.
2. Затем прочитать `AGENTS.md`, этот журнал, `README.md`, `ARCHITECTURE.md` и документы затронутых подсистем.
3. Проверить актуальные ветки, PR, commit SHA и CI. Не полагаться на старые записи как на текущее состояние.
4. Не сливать PR без явного разрешения владельца проекта.

## Текущие рабочие ветки

### PR #42 — ARM64 FDT framebuffer include
- Ветка: `v46/kdrv-catalogue-signature-policy`
- Head: `13258d9f0cb4a464b6ff16605d91a5f78b69526a`
- Исправлено: `arch/aarch64/fb_arm.c` теперь явно включает корневой `../../fdt.h`, а не одноимённый локальный заголовок загрузочного FDT.
- Проверка: run `43181799184`; build, ARM64, host-tests, KCC, audit, GUI и driver-e2e завершились success; итоговый report ещё ожидал завершения на момент снимка.
- PR остаётся draft и не слит.

### PR #95 — KCC context / self-host parser
- Ветка: `ai/kcc-context-more-state`
- Head: `b03a532c1753b736c9d77cd0318a7b2869265e8a`
- Предыдущий head `4fdeb593582fb6a3e83d21e7f43dd6f6c67574cc` провалил self-host stage1: `ir.c:23 expected ')' before '->'`.
- Исправление: `#define IRS irs` вместо `#define IRS (irs)`, чтобы макросы полей раскрывались в `irs->field`, а не в `(irs)->field`.
- Новый PR run: `43182964870`, pending/ожидает runner; исправление пока НЕ подтверждено тестом.
- Не объявлять KCC self-host исправленным до успешного stage1/stage2/stage3 прогона.

### PR #131 — KIRA WASM runtime validation
- Ветка: `fix/wasm-module-header-bounds`
- Head: `d81317a7e41564ae574affa53be168f882ccbcd9`
- Усилены: проверка версии и границ секций; уникальность/порядок секций; типы функций и точные сигнатуры KIRA imports; лимиты памяти/таблицы; индексы функций/экспортов/глобальных; сегменты данных/элементов; тела функций и локальные объявления; запрет memory ops без memory; различение явно заданного max=0 и отсутствующего maximum.
- Добавлен `tests/wasm_loader_bounds.py` с отрицательными случаями и тестом `memory.grow` при max=0; подключён к `tests/run_host_tests.sh`.
- CI run `43183396568` queued на момент снимка; тесты ещё НЕ запускались/не подтверждены.
- Это первая hardening-порция, не полная WebAssembly type-stack verification и не заявление о полной WASM-конформности.
- PR draft, не слит.

## Уже подтверждённые соседние проверки
- PR #125 ARM SMMU/PCI quarantine hardening: run `43177373290` success.
- PR #128 xHCI/DMA/scheduler hardening: run `43175213615` success.
- PR #120 SMMUv3 stage-1 domains: run `42852157626` success.
- PR #126 FDT cell-width validation: runs `43162737639` и `43162737625` success.
- PR #127 KiraFS persistence fail-closed: runs `43158217638` и `43158217643` success.
- PR #129 PCI bus-master gating: runs `43159900195` и `43159900302` success.
- PR #130 bootinfo physical RAM bounds: runs `43164087429` и `43164087483` success.
- Ранее упавший ARM64 timer сценарий PR #121 нужно сверять с более поздними ветками; в PR #128 ARM64 job завершился success.

## Следующие действия
1. Дождаться CI PR #95 на head `b03a532c...`; если self-host падает, читать точный stage1 диагностический вывод и исправлять первопричину.
2. Дождаться CI PR #131 на head `d81317a...`; при падении исправлять реализацию/фикстуры и повторять host-tests, build, ARM64. Не оставлять известные ошибки.
3. После подтверждения этих веток продолжить независимый аудит WASM verifier/runtime (типовая проверка стека, opcode immediates, import/export constraints, sandbox quotas).
4. Продолжать остальные потоки мастер-плана параллельно; не сужать работу до одного PR или CI.
5. Не создавать ISO/update archive из непроверенных или неинтегрированных веток. Финальная поставка — после согласованной интеграции и полного регрессионного прогона.

## Правила доказательности
- Код создан ≠ собран; собран ≠ тесты пройдены; QEMU/Limbo ≠ физическое оборудование.
- Каждый PASS подтверждать конкретным run/job и commit SHA.
- PR остаются неслитыми, пока владелец проекта явно не разрешит merge.
