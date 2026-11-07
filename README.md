# Kira OS (ОС Кира)

Микроядро на C и ассемблере без libc: x86_64 (дальше - ARM64), загрузка BIOS и UEFI, SMP, свой планировщик,
KiraFS и FAT, USB (UHCI/xHCI, хабы, флешки), диски virtio/ATA/AHCI/NVMe, графическая оболочка и «Разум» -
подсистема, которая учит нормы машины и управляет ею.

- Сборка: `./build_clang.sh --no-run` (clang, ld.lld, nasm, grub-mkrescue). Запуск: `./build_clang.sh --vnc`.
- Тесты на хосте: `bash tests/run_host_tests.sh` (модели устройств, файловые системы, планировщик, Разум).
- Стенд: `python3 ci/kira_vm.py --all` - QEMU (KVM, если есть), сценарии из `ci/scenarios.py`; на каждый push - GitHub Actions.
- Устройство системы - `ARCHITECTURE.md`, запуск на телефоне в Limbo - `LIMBO.md`.


- Автоматический сборочный цех и email-уведомления: [docs/BUILD_FACTORY.md](docs/BUILD_FACTORY.md).
