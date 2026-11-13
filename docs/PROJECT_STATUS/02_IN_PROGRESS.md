# 02 — В работе / сохранённые ветки

Это всё, что уже существует в отдельных ветках, PR или параллельных линиях, но не является частью текущего main baseline. Такие файлы не потеряны, однако их нельзя считать включёнными в ISO или обновление.

## Найденные расхождения деревьев

Сравнение выполнено по Git tree SHA, а не по названиям веток.

| Источник | Файлов в дереве | Файлов, которых нет в main | Значение |
|---|---:|---:|---|
| v46/unified-release-integration | 1245 | **437** | Большая интеграционная линия: boot/UEFI, IOMMU, драйверы, KCC, Mind, release tooling и сотни тестов/исследований |
| autodrv | 1300 | **492** | Историческая ветка; 436 из этих файлов также есть в v46, ещё **56** отсутствуют в v46 |
| architecture/target-baseline-2026-11 | 876 | **50** | 23 архитектурных/контрактных документа и дополнительные исходники/guards |
| feat/spatial-minimal-desktop-visual-2026-11 | 839 | **10** | Spatial audit/layout contracts, каталоги/очереди программ и UI tests |
| feat/spatial-visual-tokens-2026-11-13 | 841 | **2** | Дополнительные тесты визуальных токенов и build contract |
| release/full-os-artifacts-2026-11-13 | 839 | **0 сверх Spatial base** | Release wrapper не возвращает пропущенный v46 source tree |

### Что именно находится среди 437 файлов v46, отсутствующих в main

| Группа | Количество |
|---|---:|
| research/ | 149 |
| tests/ | 157 |
| drivers/ | 30 |
| arch/ | 18 |
| ci/ | 14 |
| docs/ | 15 |
| kcc/ | 9 |
| Остальные bootinfo, IOMMU, KAS, KLink, kernel, keys, tools, userland и root-level файлы | 45 |

Это не означает, что все 437 файлов нужно слить в main без проверки. Внутри есть рабочие реализации, тесты, исследовательские материалы, артефакты измерений и документы. Их надо разбирать по зависимостям и доказательствам, а не копировать вслепую.

**Отдельно от tree diff:** сравнение build source lists показывает ещё 14 x86-64 и 9 ARM64 source inputs из v46, которые не входят в соответствующие списки сборки main. Это не обязательно 23 отсутствующих файла: например, `pk.c` уже есть в baseline, но не включён в main kernel source list. Реестр различает «файл отсутствует в main tree» и «файл есть, но не собирается в target».

### Что конкретно не попадает в текущую main-сборку

Сверка самих build scripts показала, что это не только дополнительные тесты и документы:

- x86-64: main собирает 98 общих C-источников, v46 — 112. В main build list отсутствуют 14 v46 источников: buildcmd.c, mind_build_core.c, mind_build_grant.c, bootinfo.c, bootinfo_mb2.c, bootinfo_uefi.c, iommu_vtd.c, iommu.c, drivers/iommu/intel_vtd.c, drivers/iommu/vtd_domain.c, pk.c, kdrv_sig.c, kdrv_state.c, wx.c.
- ARM64: main common-kernel list содержит 88 источников, v46 — 97. Отсутствуют 9: bootinfo.c, bootinfo_fdt.c, buildcmd.c, mind_build_core.c, mind_build_grant.c, pk.c, kdrv_sig.c, kdrv_state.c, arch/aarch64/wx_arm.c.

Следствие: текущий main/Spatial ISO не содержит все эти build units. Некоторые отвечают за BootInfo/UEFI, IOMMU/VT-d, KDRV signatures/state, W^X и Mind build. Их нельзя объявлять готовыми только потому, что они компилируются в v46: у части подсистем в RELEASE_GAPS остаются серьёзные блокеры.

### Отдельно: полноценный release/update pipeline v46

В v46 существуют, но отсутствуют в main, следующие файлы:
- ci/package_update.sh
- ci/release_gate.py
- ci/release_image.py
- ci/audit_image.py и ci/audit_inventory.py
- ci/test_release_gate.py и ci/test_release_image.py
- docs/RELEASE_GAPS.md, docs/UNIFIED_RELEASE_PLAN.md, docs/RELEASE_SPLIT.md

Это не просто zip-скрипт. package_update.sh проверяет exact base commit, успешность host/build/VM/ARM64/audit/Mind/GUI/KCC/driver/UEFI/image-audit jobs, наличие x86-64 + ARM64 + UEFI images, binary image audit, evidence logs и только после этого собирает update archive с manifest и SHA-256. Release gate остаётся OPEN/BLOCKED в документе v46; это кандидат на интеграцию, а не готовый релиз.

### 56 файлов, оставшихся только в autodrv относительно v46

Важные группы: BSD compatibility headers/corpus, userland/bsd_rt.c, userland/drvne2kio.c, tools/bsd_*, tests/bsd/*, proc_pid.h, tests/proc_pid_test.c, arch/x86_64/bios/stage1cd.asm, дополнительные KCC tests и документы APP_ISOLATION.md, BACKLOG.md, BSD_COMPAT.md.

Эти 56 путей отдельно сохранены в JSON inventory. Не считать их автоматически устаревшими: сначала определить, что было намеренно исключено, что заменено и что ещё требуется.

### 21 файл из main отсутствует в v46

Особенно важно сохранить:
- AGENTS.md, docs/MASTER_DEVELOPMENT_PLAN.md, docs/WORKLOG.md, docs/KIRA_DESIGN_SYSTEM.md, docs/adr/ADR-0001-kvr-primary-wasm.md;
- drivers/rl/rl.c, drivers/rl/if_rlreg.h, drivers/rl/drvget.rec;
- tests/rl_build.sh, tests/rl_test.c;
- 9 отрицательных KCC fixtures и .github/workflows/watchdog.yml.

Интеграция v46 должна сохранить эти 21 путь и их содержимое, а не заменить ими/ими заменить другие версии без анализа.

## Архитектурная и Spatial работа

- PR #135: архитектурная инвентаризация, целевая архитектура, source audit, dependency map, contract registry, RFC и compatibility/hypervisor specifications.
- PR #139/#141: range-aware PCI enumeration и Device Registry adapter.
- PR #136/#137: security gates вокруг DMA/MSI и PCI bus mastering.
- PR #138–#144: Spatial Minimal UI, damage bounds, context panel, workspaces/notifications и layout.
- PR #146: семантическая палитра/геометрия Spatial UI. PR #147 закрыт без merge как отдельная проверочная копия; его ветку не удалять до сверки изменений.
- PR #145: release artifact workflow на Spatial base. В текущем снимке он не добавляет недостающие 437 файлов v46.
- PR #148: этот трёхсекционный реестр, полный snapshot файлов/веток/PR и CI guard от удаления/добавления неучтённых файлов. PR оставлен Draft до exact-head CI.
- Дополнительно в PR #148 исправляется обнаруженное по VM-логу обрезание длинных строк: увеличен line buffer до 512 байт и добавлен регрессионный host test. Ожидается exact-head host + VM CI.

## Другие незавершённые потоки

- Boot/UEFI/BootInfo и IOMMU/SMMUv3: PR #112, #115, #116, #120, #121, #125.
- WASM validation и экспериментальный KIRA VM: PR #131, #132, #134; KVM остаётся экспериментом, WASM — текущей runtime-базой.
- KCC compiler state/self-hosting: PR #93, #95 и связанные исторические ветки.
- KIRA Mind: PR #76, #78, #81.
- Scheduler: PR #74, #79.
- Driver signing/KDRV/release integration: PR #12, #21–#46 и зависимые ветки.
- ABI/IPC: PR #49, #50.
- Research/technology decisions, KIL и parallel build factory: PR #47, #48, #51, #52, #55, #57, #58, #59.

## Полный реестр и правила сохранности

В branch_inventory_2026-11-13.json сохранены:
- точные SHA и размеры деревьев восьми ключевых веток;
- полный список всех 829 файлов baseline;
- все 437 путей v46-only, 21 main-only, 56 autodrv-only, 50 architecture-only и 10 Spatial-only;
- 58 открытых PR и 49 закрытых без merge PR с head/base SHA.

Ни одна ветка/PR не удалялась. Дополнительно создан branch_heads_2026-11-13.json со всеми **302 ветками** репозитория и точными HEAD SHA (290 уникальных commit SHA). Это полный ref snapshot, но не означает, что Git tree diff уже рассчитан для каждой из 302 веток: детальные path diff пока закреплены для восьми ключевых линий выше. Не удалять ветки, пока их tree diff не будет сверён с реестром. Следующий этап — расширить tree comparison на остальные ветки и проверить CI на точном SHA.

## Полный аудит всех веток (не только v46)

Снимок `all_branch_tree_audit.json` расширен до всех refs репозитория на момент аудита:

- **302** ветки всего, **290** уникальных HEAD-коммитов.
- **211** не-CI веток, **205** уникальных деревьев исходников — каждое дерево прочитано и сравнено с pinned main baseline; ошибок чтения деревьев нет.
- **91** CI-result/preview refs сохранены в общем списке, но исключены из source-tree сравнения.
- **722 уникальных пути** существуют хотя бы в одной не-CI ветке, но отсутствуют в main baseline. Они не удалены: это branch-only инвентарь, пока не принятый в базовую поставку.
- Внутри этих 722 путей: **202** исходника/runtime/архитектурных файлов, **39** build/CI-инструментов, **217** тестов и fixtures, **164** исследовательских материалов, **74** документа и **26** прочих ресурсов. Классификация каждого пути и частота появления по уникальным HEAD сохранены в JSON.
- **77 не-CI веток без PR** сохранены отдельно для разбора (включая main; значит 76 дополнительных веток без PR). Отсутствие PR не означает, что ветку можно удалить.
- Всего найдено 134 PR в текущем снимке API: 59 открытых, 49 закрытых без merge в зафиксированном проектном снимке и 26 merged. PR #148 включён в обновлённый open-PR snapshot.
- Наиболее часто повторяющиеся branch-only группы — release/update pipeline, image/dependency audit, V44 Mind/GUI материалы, BootInfo, KDRV trust и тесты. Большая доля путей повторяется в десятках веток; это не 722 независимых новых функций.

### Правила разнесения по трём разделам

1. **Готово:** только pinned main baseline (829 файлов на SHA `811ca3a`) и подтверждённые build inputs. Сам факт нахождения файла в ветке не переводит его сюда.
2. **В работе:** все 722 branch-only пути из `all_branch_tree_audit.json`, пока не проведена проверка содержимого, зависимостей, лицензии, build integration и тестов. PR/ветки сохраняются; автоматического слияния нет.
3. **Запланировано:** требования `03_PLANNED.md` и мастер-плана, у которых ещё нет доказанной production-реализации. В частности, полноценный универсальный Driver Adaptation Layer и завершённый lifecycle/policy authority KIRA Mind не считаются готовыми.

В CI registry guard сверяет baseline, новые tracked files, build inputs, PR snapshot, веточные refs и контракт публикуемых артефактов. Текущая branch-only карта — снимок на 2026-11-13, а не живой API; её нужно обновлять при добавлении/удалении веток и PR.
