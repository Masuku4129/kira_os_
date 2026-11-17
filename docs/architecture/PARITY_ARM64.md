# Паритет x86-64 / ARM64

Стенд ARM64: `python3 ci/arm64_vm.py ВЫХОД [сценарий...]` (QEMU virt, TCG). Запуск QEMU - только под общим замком
`flock /tmp/claude-0/qemu.lock`. Сборка: `./build_arm64.sh` (manifest.h x86 на время сборки убрать). «Проверено» = есть сценарий в
`ci/arm64_vm.py`, который прошёл на реальном QEMU; всё остальное помечено явно.

## Таблица

| Функция | x86-64 | ARM64 | Сценарий ARM64 |
|---|---|---|---|
| Загрузка через UEFI, свой загрузчик без GRUB | BOOTX64.EFI + kira.iso | BOOTAA64.EFI (PE/COFF, clang+lld-link) + kira-arm64.iso (El Torito, ESP FAT16); дерево устройств берётся из таблицы конфигурации прошивки; MMU и кэш данных выключает сам загрузчик | arm64-uefi (AAVMF/edk2, `acpi=off`) |
| ACPI | разбор FADT/MADT/DSDT | НЕТ: ядро читает только дерево устройств; при включённом ACPI edk2 дерево не отдаёт (загрузчик печатает таблицы и подсказку) | - |
| Карта памяти UEFI | да | НЕТ: память берётся из узлов memory дерева; области RuntimeServices/ACPI NVS из карты UEFI не резервируются (службы времени выполнения не вызываются) | - |
| Загрузка без UEFI | BIOS/GRUB | QEMU -kernel, U-Boot booti (заголовок образа ARM64) | все остальные |
| SMP | AP-трамплин | PSCI CPU_ON, 2..8 ядер, GICv2/GICv3, вход на EL1/EL2 | gicv3, gicv2, el2-8cpu, sched8 |
| Планировщик, очереди на ядро (sched.c) | да | тот же sched.c | sched8 (8 ядер: plasma x8 + burnall, замки очередей в `ai`) |
| Изоляция, права, враждебные программы | да | да | isolation |
| Потоки, futex, мьютекс | да | да | isolation, parity (`test threads`) |
| SYS_PORT_WAIT (ожидание двух портов, тайм-аут) | да | да | parity (`test ipc`) |
| Службы ksvc kira.gfx, kira.fs | да | да | parity (`test gfxport`, `test fsport`) |
| Вещественная арифметика: SSE/x87 - NEON | fpu | fpu (NEON + double, состояние на процесс) | isolation, parity |
| 3D (cube3d + движок k3d, потоки-помощники) | да | да; самопроверка и замер проходят. Ускорение от помощников на TCG шумное (на 2 ядрах хозяина x0.78): это не измерение железа | parity |
| plasma, paint, edit, view, проводник | да | да | gui, explorer2 |
| Экран | VBE/GOP, virtio-gpu | virtio-gpu, ramfb, simple-framebuffer; GOP загрузчиком не используется | gui, ramfb |
| Ввод | PS/2, USB HID, virtio-input | virtio-input (клавиатура, планшет), USB HID через xHCI | gui, usb |
| Сеть | e1000, virtio-net, IPv4/TCP/TLS | e1000, virtio-net, IPv4/TCP/TLS/DNS | gicv3, gicv2, dma-nc |
| IPv6 | SLAAC, ND, ping6, UDP/TCP (net6) | SLAAC, DAD, ND, ping6 проверены; программа net6 против серверов хозяина на ARM64 НЕ проверена | ipv6 |
| Диск | virtio-blk, ATA, AHCI, NVMe | virtio-blk, NVMe; AHCI собран, сценария нет; ATA нет на платформе | gicv3, gicv2 |
| USB-накопитель FAT32 | да | да | usb, dma-nc |
| IOMMU | VT-d, AMD-Vi (в т.ч. virtio-net) | SMMUv3 (первая стадия) для всех virtio-устройств ядра: blk, net, rng, gpu, input; e1000/NVMe/xHCI под SMMU не проверены; некогерентные платформы - SMMU выключен | smmu, smmu-all, smmu-fallback, smmu-nc, smmu-uefi |
| Звук | HDA, AC'97, SB16 | HDA | gui |
| kcc, сборочные задания, драйверы Ring 3 | да | да | cc, drv, drvman |
| WebAssembly, Python, ksh | да | да | isolation, parity |
| Выключение, перезагрузка | ACPI S5 / сброс | PSCI | power |
| Изоляция сбоев ядра | да | да | crash, fault, overflow |
| KASLR (случайный адрес ядра) и политика kptr | да: самоперемещение образа в случайную 2-МБ позицию первого гигабайта (`kaslr_arch.c`), энтропия RDSEED/RDRAND/TSC+PIT, `kira.nokaslr`, адреса образа в журнале как `K+смещение` | НЕТ. Ядро уже PIE и само применяет `R_AARCH64_RELATIVE` (`boot.S`), поэтому технически возможно, но адрес загрузки выбирает QEMU/загрузчик, а своей копии образа и источника энтропии (RNDR, `kaslr-seed` дерева устройств, EFI_RNG) в ядре ARM64 нет; без сценария в `ci/arm64_vm.py` на реальном ARM64 это не проверить, а непроверенный KASLR хуже его отсутствия (риск незаметно сломать загрузку). План: выбор адреса в UEFI-загрузчике ARM64 (`aarch64/uefi`) по `EFI_RNG_PROTOCOL` либо копия образа, как на x86-64; сценарий по образцу `kaslr` | - |
| Ранний W^X (права секций с первой инструкции после включения MMU) | да: ранняя карта `boot.s`, проверка до `vmm_enforce_wx`, сценарий `wx-early` | частично: `wx_arm.c` ставит права по секциям из `main.c` после загрузки ядра (хост-тест `tests/wx_arm_test.c`); есть ли страницы W+X в карте `boot.S` до этого момента, в А4 не исследовалось и проверки до `wx_arm` там нет | - |
| Замер обмена (ipcbench) | да | да (добавлен в build_arm64.sh) | parity |
| Rust-программы (rsdemo) | да | НЕТ: у rustc на машине нет core для aarch64, userland/rust/lib использует x86-вызовы | - |
| atk-gs | да | не применимо (сегмент GS) | - |

## Найденные и исправленные причины

* UEFI: после прошивки счётчик времени уже идёт секунды, а отсчёт тиков начинался после sched_init - ядро видело «5 с без питания
  сторожевого таймера» до первого прерывания. Начало отсчёта перенесено к выбору периода тика (arch/aarch64/main.c).
* UEFI: состояние GIC не считается чистым (запрет всех SPI и SGI/PPI, сброс pending/active, группа 1, EOImode=0).
* Консоль прошивки на последовательном порту не выводит кириллицу: сообщения загрузчика - ASCII.
* Стенд: падения arm64-usb и arm64-dma-nc ("нет usb0") - образ флешки был пустым, потому что на машине нет mkfs.fat
  (make_flash.sh теперь использует mformat и проверяет FAT32; стенд сам проверяет образ). Падение `dns example.com` в
  arm64-gicv3 - у хозяина нет DNS: стенд подставляет example.com в /etc/hosts только внутри QEMU (пространство монтирования);
  гость по-прежнему сам шлёт DNS-запрос.
