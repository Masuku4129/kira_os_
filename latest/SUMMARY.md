# Прогон 2b3cb94 (136/merge)

host-tests: success, build: success, vm: failure, arm64: failure

FAIL apps-drvman 20.8 с - в журнале нет «drvman: подтверждение: IOMMU нет: DMA/MSI права заблокированы, пакет не запустится до изоляции устройства»
OK   apps-edit 14.5 с 
OK   apps-explorer2 26.9 с 
OK   apps-view 48.6 с 
OK   apps-wasm 46.8 с 
OK   boot-1cpu 7.4 с 
OK   boot-8cpu 7.4 с 
OK   boot-ata 7.4 с 
OK   boot-nvme 7.7 с 
OK   boot-sata 7.6 с 
OK   boot-virtio 7.7 с 
OK   cc-native 70.0 с 
OK   crash-isolation 17.2 с 
OK   drv-edu 28.2 с 
FAIL drvget-rl 54.2 с - не дождались «rl: версия 1, SHA-256 сверен, запрошено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV, подтверждено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV» за 20 с
FAIL drvman-edu 17.8 с - в журнале нет «drvman: pci \d\d:\d\d\.\d\s+1234:11e8\s+[0-9a-f]{6}\s+.*пакет edunc установлен, не запущен»
OK   isolation 46.2 с 
OK   limbo-mttcg 32.8 с 
OK   limbo-spam 33.7 с 
OK   mind-levers 168.0 с 
OK   mind-seed 11.3 с 
OK   net-e1000 21.0 с 
OK   net-e1000e 18.3 с 
OK   net-ipv6 31.9 с 
OK   net-virtio 101.4 с 
OK   power-off 6.6 с 
OK   power-reboot 14.1 с 
OK   smp 22.2 с 
OK   soak-8cpu 37.7 с 
OK   sound-ac97 13.9 с 
OK   sound-hda 25.5 с 
OK   sound-sb16 13.9 с 
OK   stress-3d 16.0 с 
OK   stress-spam-2 18.2 с 
OK   stress-spam-4 18.2 с 
OK   stress-spam 18.2 с 
OK   usb-uhci 11.4 с 
OK   usb-xhci 11.8 с 
OK   wasm-speed 13.2 с 
OK   arm64-cc 13.8 с 
OK   arm64-crash 4.6 с 
OK   arm64-dma-nc 10.4 с 
FAIL arm64-drv 67.6 с - drv: нет «drv-edunc pid \\d+»
FAIL arm64-drvman 29.2 с - @log drvman: pci \d\d:\d\d\.\d\s+1234:11e8\s+[0-9a-f]{6}\s+.*пакет edunc установлен, не запущен: нет в журнале
OK   arm64-el2-8cpu 1.7 с 
OK   arm64-explorer2 32.5 с 
OK   arm64-fault 1.5 с 
OK   arm64-gicv2 18.7 с 
OK   arm64-gicv3 28.6 с 
OK   arm64-gui 49.0 с 
OK   arm64-isolation 15.8 с 
OK   arm64-overflow 1.6 с 
OK   arm64-power 6.1 с 
OK   arm64-ramfb 4.3 с 
OK   arm64-usb 10.3 с 
