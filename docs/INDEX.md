# Указатель документов Kira OS

Обновлено: 2026-11-18

С чего начать: человеку — [README](../README.md), затем [PLAN.md](PLAN.md) (что делаем и зачем) и [SYSTEMS.md](SYSTEMS.md) (что есть в системе). Агентам и разработчикам — ещё [AGENTS.md](../AGENTS.md).

Статусы: **основной** — поддерживается и проверяется скриптом; **справочник** — описание конкретной подсистемы, обновляется вместе с ней; **история** — снимок прошлого, источник подробностей, может устареть.

| Документ | Назначение | Статус |
|---|---|---|
| [../README.md](../README.md) | Что такое Kira OS, как собрать и запустить | основной |
| [PLAN.md](PLAN.md) | Единый план работ: зачем, альтернативы, критерии | основной |
| [SYSTEMS.md](SYSTEMS.md) | Каталог подсистем: назначение, файлы, проверка, статус | основной |
| [INDEX.md](INDEX.md) | Этот указатель | основной |
| [BACKLOG.md](BACKLOG.md) | Подробный список задач и их состояние | основной |
| [QEMU_100.md](QEMU_100.md) | Что осталось до 100% в QEMU | основной |
| [INDUSTRIAL_READINESS.md](INDUSTRIAL_READINESS.md) | Аудит готовности к промышленному выпуску, открытые вопросы | основной |
| [WORKFLOW.md](WORKFLOW.md) | Правила разработки и выпуска | основной |
| [STRUCTURE.md](STRUCTURE.md) | Восемь уровней архитектуры и проверка зависимостей | справочник |
| [architecture/levels.map](architecture/levels.map) | Карта «файл → уровень» | справочник |
| [architecture/TECH_DECISIONS.md](architecture/TECH_DECISIONS.md) | Решения по технологиям с обоснованием | справочник |
| [architecture/REQUIRED_TECH_PLAN.md](architecture/REQUIRED_TECH_PLAN.md) | План обязательных технологий владельца | справочник |
| [architecture/FORMATS_AND_LANGUAGES.md](architecture/FORMATS_AND_LANGUAGES.md) | Форматы и языки программирования | справочник |
| [architecture/KIRA_IR.md](architecture/KIRA_IR.md) | Промежуточное представление компилятора | справочник |
| [architecture/KASLR_WX.md](architecture/KASLR_WX.md) | KASLR и ранний W^X образа ядра: устройство, альтернативы, незакрытое | справочник |
| [architecture/PARITY_ARM64.md](architecture/PARITY_ARM64.md) | Паритет возможностей x86 и ARM64 | справочник |
| [architecture/ADR-0001-ir-mlir-zig-spark.md](architecture/ADR-0001-ir-mlir-zig-spark.md) | Решение: IR, MLIR, Zig, SPARK | справочник |
| [adr/ADR-0001-kvr-primary-wasm.md](adr/ADR-0001-kvr-primary-wasm.md) | Решение: формат KVR и WASM | справочник |
| [DRIVERS.md](DRIVERS.md) | Драйверы в пользовательском пространстве | справочник |
| [BSD_COMPAT.md](BSD_COMPAT.md) | Слой совместимости с BSD-драйверами | справочник |
| [KCC.md](KCC.md) | Собственный компилятор KCC | справочник |
| [AUDIT.md](AUDIT.md) | Журнал аудита: цепочка хэшей, подписанные точки, проверка и подмена | справочник |
| [BLACKBOX.md](BLACKBOX.md) | Чёрный ящик: запись состояния и сбоев | справочник |
| [LATENCY.md](LATENCY.md) | Время реакции: что меряет ядро и какие границы держим | справочник |
| [SECURE_BOOT.md](SECURE_BOOT.md) | Доверенная загрузка: как выпустить подписанный образ, завести ключи, как устроена цепочка | справочник |
| [RCON.md](RCON.md) | Удалённая консоль по сети | справочник |
| [APP_ISOLATION.md](APP_ISOLATION.md) | Изоляция приложений | справочник |
| [V44_SECURITY_MODEL.md](V44_SECURITY_MODEL.md) | Модель безопасности (версия 44) | справочник |
| [DEPENDENCIES.md](DEPENDENCIES.md) | Аудит зависимостей и сторонних компонентов | справочник |
| [UI_DESIGN.md](UI_DESIGN.md) | Дизайн оболочки: принципы и стандарты, палитра, состояния, Проводник, инвентаризация меню, снимки до и после | основной |
| [WM_PERF.md](WM_PERF.md) | Плавность интерфейса при перемещении окон (А14): метрики, замеры до и после, причины, альтернативы | справочник |
| [KIRA_DESIGN_SYSTEM.md](KIRA_DESIGN_SYSTEM.md) | Концепция интерфейса (постоянный слой, контекстные поверхности); визуальный язык теперь по UI_DESIGN.md | справочник |
| [BUILDER_PLAN.md](BUILDER_PLAN.md) | План «заданий на сборку» для Разума | история |
| [MASTER_DEVELOPMENT_PLAN.md](MASTER_DEVELOPMENT_PLAN.md) | Прежний мастер-план | история |
| [UNIFIED_RELEASE_PLAN.md](UNIFIED_RELEASE_PLAN.md) | Прежний план единого выпуска | история |
| [RELEASE_GAPS.md](RELEASE_GAPS.md) | Прежний список блокеров выпуска | история |
| [WORKLOG.md](WORKLOG.md) | Журнал продолжения работ (снимок 2026-11-12) | история |
| [ANALYSIS_2026-11-15.md](ANALYSIS_2026-11-15.md) | Анализ системы 2026-11-15 | история |
| [../AGENTS.md](../AGENTS.md) | Обязательные правила для ИИ-агентов | основной |
| [../ARCHITECTURE.md](../ARCHITECTURE.md) | Устройство системы (исторические ограничения внутри) | справочник |
| [../LIMBO.md](../LIMBO.md) | Запуск на телефоне в Limbo x86 | справочник |
| [../LIMBO_ARM.md](../LIMBO_ARM.md) | Запуск в Limbo ARM | справочник |
| [../LICENSE](../LICENSE) | Лицензия: все права защищены | основной |
| [../SECURITY.md](../SECURITY.md) | Как сообщить об уязвимости, известные ограничения | основной |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Правила участия в разработке | основной |

Правило: новый документ добавляется в этот указатель в том же коммите. Проверка: `python3 ci/check_docs.py`.
