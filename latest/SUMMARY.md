# Прогон 7d82e6e (research/architecture-validation-round2-2026-11-09)

host-tests: success
data-formats: success
storage-pipeline: success
compiler-bench: failure
ipc-lab: success
build: success
vm matrix: failure
arm64: success
global gate: failure

FAIL apps-drvman 32.4 с - «drv»: в выводе нет «rl: версия 1, SHA-256 сверен, запрошено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV, подтверждено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV»
OK   apps-edit 14.6 с 
OK   apps-explorer2 26.8 с 
OK   apps-view 48.5 с 
OK   apps-wasm 45.8 с 
OK   boot-1cpu 7.1 с 
OK   boot-8cpu 7.3 с 
OK   boot-ata 7.4 с 
OK   boot-nvme 7.4 с 
OK   boot-sata 7.4 с 
OK   boot-virtio 7.8 с 
OK   cc-native 70.1 с 
OK   crash-isolation 16.5 с 
FAIL drv-edu 13.3 с - в журнале нет «\[KIRA DRV\] edu: устройство .* 1234:11e8 -> драйвер запущен как drv-edu \(pid \d+\), SHA-256 сверен, права: CAP_MMIO CAP_IRQ CAP_DMA CAP_PCIDEV, прерывание MSI включено»
OK   drvget-rl 57.8 с 
OK   drvman-edu 33.3 с 
OK   isolation 46.1 с 
OK   limbo-mttcg 33.3 с 
OK   limbo-spam 32.8 с 
OK   mind-levers 168.0 с 
OK   mind-seed 11.5 с 
OK   net-e1000 20.7 с 
OK   net-e1000e 18.3 с 
OK   net-ipv6 32.0 с 
OK   net-virtio 101.6 с 
OK   power-off 6.8 с 
OK   power-reboot 13.7 с 
OK   smp 22.5 с 
OK   soak-8cpu 37.7 с 
OK   sound-ac97 14.2 с 
OK   sound-hda 25.3 с 
OK   sound-sb16 13.9 с 
OK   stress-3d 15.6 с 
OK   stress-spam-2 18.3 с 
OK   stress-spam-4 18.3 с 
OK   stress-spam 18.2 с 
OK   usb-uhci 11.6 с 
OK   usb-xhci 12.0 с 
OK   wasm-speed 13.1 с 
OK   arm64-cc 13.8 с 
OK   arm64-crash 4.6 с 
OK   arm64-dma-nc 10.3 с 
OK   arm64-drv 25.8 с 
OK   arm64-drvman 39.0 с 
OK   arm64-el2-8cpu 2.1 с 
OK   arm64-explorer2 32.4 с 
OK   arm64-fault 1.6 с 
OK   arm64-gicv2 18.9 с 
OK   arm64-gicv3 28.7 с 
OK   arm64-gui 48.6 с 
OK   arm64-isolation 15.8 с 
OK   arm64-overflow 1.7 с 
OK   arm64-power 6.1 с 
OK   arm64-ramfb 4.5 с 
OK   arm64-usb 10.1 с 
