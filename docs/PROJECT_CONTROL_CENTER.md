# KIRA OS — центр управления проектом

**Снимок:** 2026-11-13  
**Репозиторий:** https://github.com/Masuku4129/kira_os_  
**Рабочая архитектурная ветка:** `architecture/target-baseline-2026-11`  
**Архитектурный PR:** [#135](https://github.com/Masuku4129/kira_os_/pull/135) — Draft, не объединять без разрешения владельца.

> Этот файл — короткая точка входа и карта навигации. Он не заменяет мастер-план, архитектурные спецификации, RFC, аудит и исходный код. При расхождении динамических сведений приоритет у live GitHub; при расхождении требований — у утверждённого мастер-плана и архитектурных решений.

## 1. Начинать отсюда

1. [AGENTS.md](../AGENTS.md) — обязательный рабочий протокол и запреты.
2. [Полный мастер-план](MASTER_DEVELOPMENT_PLAN.md) — полный объём проекта, направления A–N, 12 параллельных потоков, критерии готовности.
3. [Целевая архитектура](architecture/KIRA_OS_TARGET_ARCHITECTURE.md) — восемь логических уровней и границы подсистем.
4. [Текущий handoff](architecture/CURRENT_STATE_AND_HANDOFF_2026-11-13.md) — точка продолжения; все SHA и CI перепроверять live.
5. [Архитектурный аудит и execution plan](architecture/ARCHITECTURE_AUDIT_AND_EXECUTION_PLAN.md).
6. [Source layout migration](architecture/SOURCE_LAYOUT_MIGRATION.md).
7. [Component inventory](architecture/COMPONENT_INVENTORY.md), [dependency map](architecture/DEPENDENCY_MAP.md), [contract registry](architecture/CONTRACT_REGISTRY.md).
8. [Source audit findings](architecture/SOURCE_AUDIT_FINDINGS.md).
9. [Open PR tracker](architecture/OPEN_PR_ARCHITECTURE_TRACKER.md) — топология веток и интеграционные риски; это dated snapshot, сверять live.
10. [RFC index](architecture/rfc/README.md), [Windows compatibility](compat/WINDOWS_COMPATIBILITY.md), [Linux compatibility](compat/LINUX_COMPATIBILITY.md), [Hypervisor architecture](virtualization/HYPERVISOR_ARCHITECTURE.md).
11. [WORKLOG](WORKLOG.md) — история изменений и доказательства; записи не заменяют актуальную проверку.

## 2. Модель проекта: не путать два масштаба

**Восемь архитектурных уровней:** Hardware/Platform → HAL/Device Model/Drivers → KIRA Kernel → Security → KIRA Mind → System Services → Runtime Environment → User Environment. KIRA Hypervisor — отдельная подсистема для виртуализации целых гостевых ОС. Boot, installer/update/recovery, KCC/toolchain, package/driver/service managers, event/resource/power/observability — сквозные системы.

**14 областей мастер-плана (A–N):**
A Architecture/Platform; B Kernel; C KVR and application runtimes; D Mind; E KCC/toolchain; F Memory/Execution Safety; G Drivers/Driver Manager; H KiraFS/Storage; I Network; J Graphics/Desktop; K System Services/Application Platform; L Boot/Installer/Update; M ISA/Hardware; N Developer Tools/Automation/Quality.

**12 параллельных рабочих потоков** перечислены отдельно в разделе 3 мастер-плана. Это план организации работ, а не альтернативная нумерация областей A–N. Нельзя терять область проекта из-за того, что потоков двенадцать.

## 3. Текущий снимок GitHub (2026-11-13)

- Default branch: `main`; live SHA при проверке: `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`.
- Архитектурная ветка: `architecture/target-baseline-2026-11`; live SHA: `f3d7d43b3434e817254f12a9a15a3097e8e99b97`.
- PR #135: open, Draft; base SHA в PR snapshot: `bbc7f7ab948e72511aed113b69043add10ec8350`; live main уже продвинулся до другого SHA, поэтому перед интеграцией нужно обновить base/проверить merge base и cumulative diff. PR не mergeable на момент чтения.
- На первой странице GitHub API получено 50 открытых PR и 100 refs веток. Это не доказательство общего количества: список веток запрашивался страницей до 100 элементов. Многие ветки связаны с открытыми PR, CI snapshots или вложенными зависимостями.
- Дерево архитектурной ветки: 910 entries; присутствуют legacy root-level C/H файлы и новые каталоги `arch/`, `kernel/`, `services/`, `drivers/`, `userland/`. Перемещение/удаление исходников без проверки build manifests, include graph, tests и всех consumers запрещено.
- Exact-head checks для `f3d7d43...`: GitHub commit status = pending с 0 statuses; check-runs = 0. Следовательно, текущий HEAD не имеет обнаруженного подтверждения CI.
- Последний найденный полный successful run: [43298087855](https://github.com/Masuku4129/kira_os_/actions/runs/43298087855), 30/30 jobs, но он проверял SHA `618ce01344e219991323aca51f9de58c55ed2aab`, а не текущий HEAD. Не переносить PASS на новые коммиты.
- Физические x86-64/ARM64 устройства не проверены этим CI; QEMU/Limbo не считать физической проверкой.

## 4. Обязательный протокол каждой задачи

Перед анализом/изменениями:
1. Зафиксировать запрос пользователя как проверяемые требования: результат, ограничения, запреты, критерии приёмки. Не подменять исходный запрос собственной интерпретацией.
2. Привязать каждый пункт к областям A–N и одному или нескольким из 12 потоков мастер-плана.
3. Прочитать документы из раздела 1, полный мастер-план, AGENTS, README, ARCHITECTURE и документы затрагиваемой подсистемы.
4. Получить live состояние ветки, PR, base/head SHA, changed files, review threads, checks/jobs и зависимости. Не доверять старым снимкам.
5. Проверить исходное дерево, build manifests, public contracts, callers/consumers и рабочее состояние. Не перезаписывать чужие изменения.
6. Составить краткий dependency-ordered план и таблицу трассировки требований.

### Матрица трассировки (обязательна для каждой нетривиальной задачи)

| ID | Требование пользователя (точно по смыслу) | Раздел мастер-плана | Контракт/компонент | Изменение или проверка | Доказательство: команда, среда, SHA, результат | Статус |
|---|---|---|---|---|---|---|
| R-01 | ... | A–N / §... | ... | ... | ... | UNVERIFIED |

Допустимые статусы: **PASS** (все критерии имеют свежие доказательства), **PARTIAL**, **FAIL**, **UNVERIFIED**, **BLOCKED**. Не ставить PASS по чтению кода, старому CI, queued job или заявлению другого агента.

Перед финальным отчётом:
- Сверить каждый пункт запроса с результатом и доказательством.
- Сверить полноту с мастер-планом и целевой архитектурой; отметить затронутые и не затронутые области.
- Проверить diff на лишние изменения, заглушки, мёртвые/дублирующие реализации, нарушения лицензий/безопасности и пропущенные consumers.
- Проверить, что команды действительно запускались, SHA совпадает, а вывод поддерживает заявленный статус.
- Разделить host tests, x86-64 build, ARM64 build, QEMU/OVMF, Limbo и физическое железо.
- Если проверка невозможна, указать точный блокер и оставить UNVERIFIED/BLOCKED.
- Обновить WORKLOG и соответствующие inventory/dependency/contract docs.

## 5. Правила уборки и управления ветками

- Не удалять исходники, каталоги, ветки, теги, workflow artifacts или PR только по названию, возрасту или отсутствию недавних коммитов.
- Перед удалением ветки доказать, что она не является head/base открытого PR, dependency base другой ветки, единственным носителем неперенесённого кода или источником CI/evidence. Получить отдельное подтверждение владельца на destructive cleanup.
- Не закрывать PR и не объединять их без отдельного прямого разрешения.
- Ветки `ci-results-*` считать потенциальными evidence refs до проверки их содержимого и ссылок; не удалять массово.
- Сначала устранять организационный шум навигацией, метаданными, индексами, PR templates и автоматическими проверками, а не физическим удалением файлов.
- Любое перемещение реализации выполнять только dependency-closed срезом: обновить x86-64/ARM64 manifests, includes, consumers, tests, source inventory и architecture guards; старый файл удалить только после доказанного переноса.

## 6. Рабочая очередь

**P0 — точность состояния и доказательств**
- Запустить полный CI на exact current head PR #135 и сохранить результаты для точного SHA.
- Проверить PR #135 против текущего main; зафиксировать реальный merge base и cumulative diff, не менять main.
- Обновить PR tracker, WORKLOG и этот контрольный снимок после получения новых фактов.

**P1 — архитектурные контракты**
- M2: range-aware PCI enumeration, BootInfo unification, Device Model identity/lifecycle, resource ownership.
- M3: object-scoped capability handles, delegation/revocation/expiry/audit, Mind authority boundaries.
- Согласовать PR #136/#137 и IOMMU/DMA gates до включения bus mastering.

**P2 — все остальные потоки**
- Продолжать все 12 параллельных потоков и все 14 областей A–N по зависимостям; не превращать P0 в отмену остального мастер-плана.
- Отдельно сохранять WASM/KVR baseline, experimental KIRA VM, Linux/Windows compatibility и whole-OS Hypervisor как разные направления.

## 7. Безопасные границы

Работа ведётся в отдельной рабочей ветке/PR. `main` не изменять напрямую. Не выполнять merge, release, delete, force-push, массовое переименование или изменение доступа без отдельного разрешения. Реальные реализации и проверяемые тесты обязательны; mock/stub допустимы только в тестах, где они явно моделируют внешний boundary и не выдаются за production.
