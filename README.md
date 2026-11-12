# Kira OS (ОС Кира)

## Обязательные архитектурные документы

- [Целевая архитектура KIRA OS](docs/architecture/KIRA_OS_TARGET_ARCHITECTURE.md) — конечная модель, включая Windows/Linux application compatibility и отдельный Hypervisor.
- [Архитектурный аудит и порядок работ](docs/architecture/ARCHITECTURE_AUDIT_AND_EXECUTION_PLAN.md) — ближайшая очередь и критерии завершения.
- [Инвентаризация компонентов](docs/architecture/COMPONENT_INVENTORY.md) — предварительная карта исходников и статусов.
- [Карта зависимостей](docs/architecture/DEPENDENCY_MAP.md) — целевые и наблюдаемые связи между подсистемами.
- [Реестр контрактов](docs/architecture/CONTRACT_REGISTRY.md) — ABI/API, владельцы, статусы и критерии утверждения.
- [Source-audit findings](docs/architecture/SOURCE_AUDIT_FINDINGS.md) — конкретные наблюдения по коду и риски.
- [Открытые PR и зависимости](docs/architecture/OPEN_PR_ARCHITECTURE_TRACKER.md) — веточная топология и интеграционные gates.
- [Совместимость Windows](docs/compat/WINDOWS_COMPATIBILITY.md), [совместимость Linux](docs/compat/LINUX_COMPATIBILITY.md) и [матрица приложений](docs/compat/COMPATIBILITY_TEST_MATRIX.md).
- [Архитектура гипервизора](docs/virtualization/HYPERVISOR_ARCHITECTURE.md) — отдельный контур запуска целых гостевых ОС.

## Обязательный план разработки

Перед любой разработкой, аудитом или изменением кода прочитайте [AGENTS.md](AGENTS.md) и полный [мастер-план KIRA OS](docs/MASTER_DEVELOPMENT_PLAN.md). Это обязательная точка входа для всех участников и ИИ-агентов.


Микроядро на C и ассемблере без libc: x86_64 (дальше - ARM64), загрузка BIOS и UEFI, SMP, свой планировщик,
KiraFS и FAT, USB (UHCI/xHCI, хабы, флешки), диски virtio/ATA/AHCI/NVMe, графическая оболочка и «Разум» -
подсистема, которая учит нормы машины и управляет ею.

- Сборка: `./build_clang.sh --no-run` (clang, ld.lld, nasm, grub-mkrescue). Запуск: `./build_clang.sh --vnc`.
- Тесты на хосте: `bash tests/run_host_tests.sh` (модели устройств, файловые системы, планировщик, Разум).
- Стенд: `python3 ci/kira_vm.py --all` - QEMU (KVM, если есть), сценарии из `ci/scenarios.py`; на каждый push - GitHub Actions.
- Устройство системы - `ARCHITECTURE.md`, запуск на телефоне в Limbo - `LIMBO.md`.
