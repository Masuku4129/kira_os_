# Прогон 8561b5e (161/merge)

host-tests: success, build: success, vm: failure, arm64: failure

FAIL apps-drvman 32.3 с - в журнале нет «\\[KIRA DRV\\] rl: DMA/MSI требует изоляции устройства через IOMMU; сейчас её нет - драйвер НЕ запущен»
OK   apps-edit 14.7 с 
OK   apps-explorer2 27.2 с 
OK   apps-view 48.2 с 
OK   apps-wasm 45.9 с 
OK   boot-1cpu 7.4 с 
OK   boot-8cpu 7.2 с 
OK   boot-ata 7.5 с 
OK   boot-nvme 7.4 с 
OK   boot-sata 7.4 с 
OK   boot-virtio 7.7 с 
OK   cc-native 69.7 с 
OK   crash-isolation 16.7 с 
OK   drv-edu 28.4 с 
OK   drvget-rl 53.5 с 
FAIL drvman-edu 81.1 с - не дождались «drvman: готово с ошибками» за 50 с
OK   isolation 46.2 с 
OK   limbo-mttcg 33.1 с 
OK   limbo-spam 32.8 с 
OK   mind-levers 168.0 с 
OK   mind-seed 11.5 с 
OK   net-e1000 20.8 с 
OK   net-e1000e 18.6 с 
OK   net-ipv6 32.1 с 
OK   net-virtio 101.5 с 
OK   power-off 6.8 с 
OK   power-reboot 14.1 с 
OK   smp 22.5 с 
OK   soak-8cpu 37.6 с 
FAIL sound-ac97 13.7 с - «sound»: в выводе нет «недоборов 0»
OK   sound-hda 25.5 с 
OK   sound-sb16 13.6 с 
OK   stress-3d 15.9 с 
OK   stress-spam-2 18.2 с 
OK   stress-spam-4 18.4 с 
OK   stress-spam 19.2 с 
OK   usb-uhci 11.5 с 
OK   usb-xhci 11.5 с 
OK   wasm-speed 13.2 с 
OK   arm64-cc 13.7 с 
OK   arm64-crash 4.5 с 
OK   arm64-dma-nc 9.8 с 
FAIL arm64-drv 74.2 с - drv: нет «свободно»
FAIL arm64-drvman 98.5 с - drv: нет «drv-edunc pid \\d+»
OK   arm64-el2-8cpu 1.7 с 
OK   arm64-explorer2 32.3 с 
OK   arm64-fault 1.5 с 
OK   arm64-gicv2 18.5 с 
OK   arm64-gicv3 28.0 с 
OK   arm64-gui 48.7 с 
OK   arm64-isolation 15.5 с 
OK   arm64-overflow 1.6 с 
OK   arm64-power 6.1 с 
OK   arm64-ramfb 4.2 с 
OK   arm64-usb 10.0 с 
