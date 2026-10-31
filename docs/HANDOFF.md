# Kira OS - передача работы (для продолжения другим разработчиком или моделью)

Этот файл - всё, что нужно, чтобы продолжить разработку с того же места. Читать вместе с `docs/PROGRESS.md`
(что сделано и почему) и `ARCHITECTURE.md` (подробности подсистем).

## 1. Правила проекта (обязательны)
- Чистый C (C11, freestanding, без libc) и ассемблер. Никаких заглушек, имитаций и «временных» обходов: функция
  либо делает своё дело, либо честно сообщает, что устройства/подсистемы нет (как `arch/aarch64/notyet.c`).
- Цель - настоящее железо. Адресов железа в коде нет: всё берётся из ACPI (x86-64) или дерева устройств (ARM64).
  Пределы (ядра, память, потоки, программы) выводятся из найденного железа (`scale.c`), а не задаются числом.
- Обе архитектуры развиваются параллельно: общий код - в корне, зависящий от процессора - в `arch/<арх>/`.
  Новый общий код не содержит ассемблера x86/ARM: только через `cpu.h`, `arch_percpu.h`, `arch_sched.h`,
  `arch_vmm.h` своей архитектуры.
- Каждое изменение: сборка с `-Werror` (обе архитектуры), тесты на хосте, сценарии стенда. В отчётах честно
  разделять «проверено стендом» и «не проверено».
- Комментарии и сообщения пользователю - по-русски; журнал ядра (`[KIRA ...]`) - по-английски (команды консоли
  отвечают по-русски).

## 2. Сборка и запуск
Нужно: clang, ld.lld, nasm (x86), llvm-objcopy (ARM), grub-mkrescue + xorriso + mtools (ISO x86), qemu.
- x86-64: `./build_clang.sh --no-run` -> `kira.iso` (BIOS+UEFI), `kira.disk` (KiraFS с программами в /bin).
  Запуск: `./run_qemu.sh --vnc` (см. флаги в начале run_qemu.sh). Termux на телефоне: `./build_clang.sh --android`
  кладёт образы в Download для Limbo x86 (LIMBO.md).
- ARM64: `./build_arm64.sh` -> `kira-arm64.img` (образ с заголовком загрузки ARM64), `kira-arm64.disk`.
  Запуск: `./build_arm64.sh --run` (QEMU virt, 4 ядра, GICv3, virtio-blk).
- Тесты на хосте: `bash tests/run_host_tests.sh` (gcc, ASan/UBSan), в конце `ALL HOST TESTS PASSED`.

## 3. Стенд (CI)
- `.github/workflows/ci.yml`: host-tests, build (x86), vm (13 сценариев x86 под KVM: `ci/kira_vm.py` +
  `ci/scenarios.py`), arm64 (4 машины QEMU virt без KVM: `ci/arm64_vm.py`), report.
- Итоги публикуются в ветку `ci-results`: `latest/SUMMARY.md`, журналы `latest/vm-*/serial.log`, снимки экрана,
  символы ядра `latest/vm-symbols/kira.sym` (для расшифровки адресов в аварийных дампах).
  Прочитать: `git fetch --depth 1 origin +refs/heads/ci-results:refs/remotes/origin/ci-results;
  git show origin/ci-results:latest/SUMMARY.md`.
- Сценарий = список шагов (`cmd`/`must`/`forbid`, `send`, `sleep`, `shot`, `log`, `log_forbid`, `responsive`).
  Запрещённые строки любого сценария x86: `NO_ERR` в ci/scenarios.py (сбой ядра, IRET, GS, переполнение стека...).

## 4. Карта исходников
Общий код (корень):
- Ядро: `sched.c` (планировщик: очереди у каждого ядра, приоритеты, вес = политика Разума, рефлекс вытеснения),
  `kmutex.c`, `spinlock.h` (билетный замок), `percpu.c/h`, `scale.c`, `heap.c`, `kpar.c` (параллельная работа на
  ядрах), `watchdog.c`, `immune.c` (самопроверка SHA-256 кода ядра), `kstack.h`.
- Программы: `proc.c` (процессы и потоки), `syscall.c` (номера и обработка вызовов), `elf.c`, `progs.c` (запуск,
  доверие по manifest.h), `trust.c`, `uheap.c`, `fsrv.c` (файлы песочницы), `kobj.c` (порты, разделяемая память),
  `futex.c`, `uaccess.c`, `userlog.c`.
- Разум: `mind.c` (чистый: нормы, здоровье, кванты, тактика ~40 Гц, предвидение), `mindnet.c` (логистическая
  регрессия, веса на диске), `mind_sys.c` (обвязка: замеры, исполнители, поток `mind-tact`), `life.c`,
  `experience.c`, `devhealth.c`.
- Диски и ФС: `pci.c` (порты x86 или ECAM, назначение BAR), `virtio.c`, `virtio_blk.c`, `nvme.c`, `ahci.c`,
  `ata.c` (только x86), `blk.c`, `kfs.c` (KiraFS v2), `store.c`, `fat.c`, `part.c`, `vfs.c`, `fscmd.c`.
- USB: `usb.c`, `xhci.c`, `uhci.c` (только x86: порты), `usbhub.c`, `usbhid.c`, `usbmsc.c`.
- Экран: `display.c`, `wm.c` (плитки 64x64, параллельная сборка), `gfx2d.c`, `gfxui.c`, `gfxsrv.c` (окна программ),
  `fb.c`, `fbcon.c`, `earlycon.c`, `apps.c`, `explorer.c`, `dock.c`, `icons.c`, `brainfx.c`, `input.c`.
- Консоль x86: `console.c` (все команды); `kernel.c` - порядок запуска x86.
- `fdt.c` - разбор дерева устройств.

`arch/x86_64/`: `boot.s`, `interrupts.s` (входы, GS, SYSCALL), `context.s`, `ap_tramp.s`, `uaccess.s`, `linker.ld`,
`gdt.c`, `idt.c`, `pic.c`, `apic.c`, `ioapic.c`, `irqmode.c`, `smp.c`, `fpu.c`, `exceptions.c`, `syscall_arch.c`,
`proc_arch.c` (TSS, CR3), `uaccess_arch.c` (SMAP), `percpu_arch.c` (GS), заголовки `cpu.h`, `arch_*.h`.
Платформа x86 в корне (порты): `pmm.c` (multiboot), `vmm.c`, `timer.c`, `clock.c` (калибровка TSC), `uart.c`,
`keyboard.c`, `rtc.c`, `acpi.c`, `hwprofile.c`, `mbinfo.c`, `bootmap.c`, `ramext.c`.

`arch/aarch64/`: `boot.S` (заголовок образа, самоперемещение, EL3/EL2 -> EL1, вектора), `context.S`, `fpu.S`,
`mem.S`, `uaccess.S` (LDTR/STTR, вход в EL0), `linker.ld`, `main.c` (PL011, MMU, GIC, таймер, PSCI, SGI, окно
стеков, vmm_map_mmio, PCI из дерева, порядок запуска, консоль), `pmm_arm.c`, `vmm_user.c` (пространства программ,
ASID), `uaccess_arch.c` (PAN, вход в программу), `selftest_arm.c` (команда test), `notyet.c` (подсистемы, которых
на ARM64 ещё нет).

## 5. Состояние на 31.10.2026
x86-64: всё прежнее (оболочка, программы, USB, диски, Разум) + этапы 1-2 плана; все 13 сценариев стенда - OK.
ARM64 (проверено стендом на 4 машинах QEMU virt): загрузка, MMU, GICv2/v3, PSCI до 8 ядер, планировщик, куча,
мьютексы, PCIe (ECAM), virtio-blk и NVMe, KiraFS (запись/чтение файлов), опыт, Разум (все уровни), сторож.
Программы на ARM64 работают (стенд): mt (потоки, futex) и ipc (порты, разделяемая память) - PASS.

## 6. Следующие шаги (по порядку)
1. Программы на ARM64 - сделано (см. PROGRESS). Осталось: враждебные программы для ARM64 (аналоги atk-* на
   ассемблере ARM: запись в память ядра, привилегированная команда, указатель ядра в вызов), проверка FP в EL0
   (аналог fpu.c без long double), cube3d/plasma/paint - вместе с экраном.
2. Экран на ARM64 (ramfb через fw_cfg или virtio-gpu) -> перенести display/wm/gfxsrv, убрать заглушки из notyet.c.
3. USB xHCI на ARM64 (тот же xhci.c через PCI), ввод - virtio-input или USB.
4. Общая консоль: разделить console.c на общую таблицу команд и команды архитектуры, использовать на ARM64.
5. Разделить замок планировщика по ядрам (sched_lock -> замки очередей).
6. Дальше по плану: capabilities для всех вызовов и программы WASM, сеть, виртуальные машины для недоверенного кода.

## 7. Тонкие места (уже стоили отладки)
- ARM64: вход таблицы векторов - ровно 0x80 байт (общий обработчик), иначе прерывания попадают в чужой код.
- ARM64: образ должен быть с заголовком ARM64 (иначе QEMU не передаёт дерево устройств в x0).
- ARM64: вторичное ядро пишет в стек до включения MMU - стек обязательно сбросить из кэша (dc civac).
- ARM64: простаивающие ядра выключают тик - проверку «тик идёт» делать до входа ядра в простой.
- x86: SYSRET на AMD требует базу STAR с RPL 3; вход в ядро проверяет GS_BASE и сбрасывает RFLAGS.AC.
- Вывод в последовательный порт - только через кольцо (uart.c): ожидание порта с выключенными прерываниями
  останавливало ядро до 0,8 с.
- Тесты на хосте подменяют процессор (host_cpu, KIRA_HOST_TEST); `cpu.h` x86 содержит версии для хоста.
