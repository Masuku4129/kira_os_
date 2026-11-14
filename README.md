# Kira OS (ОС Кира)

## Обязательный план разработки

Перед любой разработкой, аудитом или изменением кода прочитайте [AGENTS.md](AGENTS.md) и полный [мастер-план KIRA OS](docs/MASTER_DEVELOPMENT_PLAN.md). Это обязательная точка входа для всех участников и ИИ-агентов.

**Реестр файлов и состояния проекта:** [готово](docs/PROJECT_STATUS/01_READY.md) · [в работе](docs/PROJECT_STATUS/02_IN_PROGRESS.md) · [запланировано](docs/PROJECT_STATUS/03_PLANNED.md). Новый/удалённый файл обязан быть отражён в [машиночитаемом реестре](docs/PROJECT_STATUS/status_manifest.json); host suite проверяет отсутствие потерянных файлов.


Микроядро на C и ассемблере без libc: x86_64 (дальше - ARM64), загрузка BIOS и UEFI, SMP, свой планировщик,
KiraFS и FAT, USB (UHCI/xHCI, хабы, флешки), диски virtio/ATA/AHCI/NVMe, графическая оболочка и «Разум» -
подсистема, которая учит нормы машины и управляет ею.

- Сборка: `./build_clang.sh --no-run` (clang, ld.lld, nasm, grub-mkrescue). Запуск: `./build_clang.sh --vnc`.
- Тесты на хосте: `bash tests/run_host_tests.sh` (модели устройств, файловые системы, планировщик, Разум).
- Стенд: `python3 ci/kira_vm.py --all` - QEMU (KVM, если есть), сценарии из `ci/scenarios.py`; на каждый push - GitHub Actions.
- Устройство системы - `ARCHITECTURE.md`, запуск на телефоне в Limbo - `LIMBO.md`.
