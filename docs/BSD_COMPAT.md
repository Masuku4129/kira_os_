# Слой совместимости BSD («автодрайверы»): основание

Цель: Кира сама находит драйвер устройства в открытых BSD-деревьях, скачивает его, собирает KCC на устройстве и запускает как драйвер Ring 3,
без рецептов под конкретное устройство. Пробный запуск и откат — до постоянной установки; постоянная установка — только после подтверждения человека
(защиты DMA для большинства устройств пока нет, подписей у чужих исходников нет).

## Что нужно драйверу BSD
Стандартный драйвер OpenBSD (`sys/dev/pci/if_*.c`, `sys/dev/ic/*.c`) ждёт ядро OpenBSD: `sys/param.h`, `sys/systm.h`, `sys/device.h` (autoconf, softc),
`sys/mbuf.h` (цепочки пакетов), `net/if.h` (ifnet, ifq), `net/if_media.h` (ifmedia), `dev/mii/*` (PHY), `machine/bus.h` (bus_space, bus_dma),
`dev/pci/*` (pci_mapreg_map, pci_intr_*), `sys/timeout.h`, `sys/malloc.h`. Измерение на `ne2k` (QEMU ne2k_pci): около 50 разных вызовов на ~2100 строк.

## Устройство слоя (дорожка B)
- `kcc/include/bsd/...` — заголовки с именами BSD (`<sys/param.h>`, `<net/if.h>`, `<machine/bus.h>` ...). Это НАШ код, BSD-лицензия, чужого кода в нём нет.
- `userland/bsd_rt.c` — рантайм в процессе драйвера: mbuf, ifnet/ifq поверх колец netdev (`kdrv_bsd.h`), timeout, autoconf-обвязка (`struct device`, softc),
  ifmedia, bus_space (порты и память), основной цикл (прерывание/опрос → `*_intr`, таймеры, передача `if_start`).
- `tools/bsd_pcidevs.py` (позже `drvget`) — `pcidevs.h` ТОЛЬКО из тех PCI_VENDOR_*/PCI_PRODUCT_*, что упомянуты в исходниках драйвера, значения из `pcidevs` дерева.
- Поиск драйвера по ID (vendor:device): `pcidevs` → символ `PCI_PRODUCT_*` → драйвер, где символ упомянут в таблице устройств.
- Лицензия и подозрительные файлы проверяются как у `drvget` (BSD/MIT/ISC; без GPL и бинарников).

## Корпус и счётчик совместимости
`drivers/bsdcorpus/manifest.txt` закрепляет коммит OpenBSD и SHA-256 каждого файла; `tools/bsd_corpus.py fetch` скачивает в `build-bsd/corpus` (в репозиторий чужой код не кладём).
`tests/bsd/meter.sh` собирает каждый набор слоем и пишет «собирается / не собирается» с первой ошибкой. Порядок наборов — по возможности проверки в QEMU:
1. `ne2k` (ne2k_pci: порты, без DMA, без PHY-цепочки) — основание слоя: mbuf, ifnet, bus_space, timeout, autoconf.
2. `pcn` (AMD PCnet: DMA, кольца) — bus_dma.
3. `fxp` (Intel 82557/8/9: MII, DMA) — MII/ifmedia.
4. `dc` / `tulip`, затем остальные из pcidevs.

## Что остаётся честным ограничением
Прерывания INTx ядро драйверам не даёт (только MSI) — драйверы без MSI опрашиваются по таймеру. Один сетевой адаптер на стек. Защиты DMA (IOMMU) нет для большинства устройств.

## Состояние (ne2k)

Работает на QEMU `-device ne2k_pci` без правок исходников OpenBSD: `if_ne_pci.c + ne2000.c + dp8390.c + rtl80x9.c` из закреплённого корпуса,
`tools/bsd_build.sh` собирает ELF, пакет `bsdne` (права MMIO, PCIDEV, NETDEV) запускается менеджером драйверов, ядро получает карту,
DHCP и ping идут через неё (сценарий `drv-bsd-ne2k`). Порты - системный вызов `KDRV_SYS_IO` (сценарий `drv-io`).
`tests/bsd/meter.sh` - счётчик: сколько файлов корпуса компилируется через слой.
Уроки счётчика: без `BYTE_ORDER/LITTLE_ENDIAN` в слое `#if BYTE_ORDER == BIG_ENDIAN` молча истинно - драйвер переставлял байты.
Не сделано: сборка этого драйвера компилятором KCC (пока clang), автоматический поиск по ID в drvget (есть `tools/bsd_pcidevs.py find/ids`),
наборы pcn, fxp, dc; прерывания только опросом (INTx для Ring 3 нет); таблица групповых адресов ядра не передаётся (принимаются все).
