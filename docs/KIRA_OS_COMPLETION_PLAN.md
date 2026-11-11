# KIRA OS — план работ до завершения

Источник требований: `docs/KIRA_OS_MASTER_SPEC.md`. Рабочая ветка: `v46/release-completion`.

## Очерёдность

### 0. Зафиксировать baseline
- Проверить HEAD, незакоммиченные/открытые PR и последние Actions runs.
- Получить полный статус каждого job/step/artifact; расследовать текущий ARM64/QEMU failure по serial log.
- Проверить push/PR classifier: обычные изменения, docs-only, CI changes, rename/delete, отсутствующая diff base.
- Записать baseline с commit SHA и реальными failures.

### 1. Стабилизировать и ускорить CI
- Исправить только доказанные ошибки workflow и тестов.
- Измерить wall-clock и длительность steps нескольких запусков.
- Оптимизировать apt/tool cache и повторные установки на основании измерений.
- Проверить permissions, concurrency, dependency graph, artifacts и branch protection.
- Сохранить full suite и fail-closed при любой неопределённости classifier.

### 2. Supply-chain и CVE baseline
- Инвентаризировать manifests, vendored code, toolchain и конечные образы.
- Сформировать SBOM исходников и ISO/KiraFS, license/provenance report.
- Подключить применимые OSV/NVD/GHSA/Grype scanners, CodeQL/static analysis, secret scanning.
- Создать triage реестра CVE с доказательствами применимости и remediation.
- Ввести release gate только после проверки качества baseline.

### 3. Kernel, capabilities и App Isolation
- Составить карту границ привилегий и syscall/IPC surface.
- Аудировать user access, address spaces, IPC/SHM, capability broker и resource quotas.
- Добавить adversarial tests на escape, privilege escalation, malformed IPC, resource exhaustion.
- Провести fuzzing и sanitizers на обеих архитектурах; исправить подтверждённые дефекты.

### 4. Driver Compatibility и DMA
- Инвентаризировать реальные поддерживаемые устройства, форматы и recipes.
- Закрыть подпись издателя, key trust/rotation/revocation и anti-rollback.
- Проверить sandbox driver execution, ABI adaptation, MMIO/IRQ/DMA capability boundaries.
- Реализовать IOMMU path (VT-d/AMD-Vi/SMMU) по платформам либо fail-closed отказ от небезопасных драйверов.
- E2E: detect → source/recipe → verify → build → sandbox → approve → install → rollback.
- Отдельно перечислить неподдерживаемые .ko/.sys сценарии; не заявлять универсальность.

### 5. Secure boot и обновления
- Спроектировать и реализовать собственный x86 BIOS/UEFI loader, убрать GRUB dependency по этапам.
- Проверить ARM64 chain of trust.
- Подписывать образы и обновления; реализовать trusted publishers, rotation/revocation, anti-rollback.
- Проверить A/B update, повреждённый пакет, прерывание питания и восстановление.

### 6. KCC и self-hosting
- Сверить поддерживаемый язык и backend с реальными тестами.
- Развивать frontend/IR/optimizer/x86-64/AArch64 backends/ELF linker path.
- Добавить differential tests и corpus; fuzz parser и optimizer.
- Постепенно переносить сборку программ на KCC, затем доказуемо сокращать внешний toolchain.
- Не объявлять self-hosting до успешной сборки собственного системного комплекта.

### 7. KiraFS и recovery
- Проверить crash consistency, metadata integrity, journal/CoW, VFS boundaries.
- Развить Merkle/CAS/snapshot/A-B только в соответствии с архитектурой.
- Тестировать power-loss, corruption, exhaustion, rollback и сохранность пользовательских данных.

### 8. Mind
- Проверить broker boundary, квоты, журналирование и safe mode.
- Добавить воспроизводимые quality/holdout/adversarial/soak tests.
- Проверить rollback и отсутствие деградации ядра при отказе Mind.

### 9. Сеть, GUI, приложения и остаточные пробелы
- Сверить исторический план с текущим кодом, закрыть подтверждённые сетевые/TLS/IPv6/AML gaps.
- Проверить GUI, ввод, локализацию, файловый менеджер и приложения на обеих архитектурах.
- Не переносить старые TODO в текущий backlog без проверки HEAD.

### 10. Hardware qualification и release
- Составить матрицу физических x86-64/ARM64 устройств и периферии.
- Проверить загрузку, сеть, storage, USB, звук, графику, suspend/power, DMA/IOMMU.
- Собрать release images, symbols, logs, SBOM, CVE report, test matrix и recovery instructions.
- Выпускать только с ясным перечнем подтверждённых возможностей и оставшихся ограничений.

## Правила очереди
- Этап 0 — немедленный приоритет; далее можно вести независимые аудиты параллельно.
- Не менять один и тот же файл конкурентно.
- Любой обнаруженный дефект: воспроизвести → исправить → regression test → CI → сохранить evidence.
- Никаких mock/stub/PASS без исполнения.
- Каждый завершённый пакет — отдельный commit; PR не сливать без required checks.
- Ветка/документы обновляются после фактического результата, а не заранее.
