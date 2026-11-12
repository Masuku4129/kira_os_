# Прогон 55328f6 (fix/wasm-module-header-bounds)

host-tests: failure, build: success, vm: failure, arm64: success

OK   apps-drvman 41.4 с 
OK   apps-edit 14.7 с 
OK   apps-explorer2 27.0 с 
OK   apps-view 48.2 с 
FAIL apps-wasm 11.3 с - в журнале нет «wtest:\ fib\(24\)=46368\ primes<20000=2262\ sort=ad642057dcc4b702\ switch=4148393079\ ops=ba422c8d525a9aea\ sext=269715»
OK   boot-1cpu 7.3 с 
OK   boot-8cpu 7.3 с 
OK   boot-ata 7.4 с 
OK   boot-nvme 7.7 с 
OK   boot-sata 7.5 с 
OK   boot-virtio 8.1 с 
OK   cc-native 70.2 с 
OK   crash-isolation 16.5 с 
FAIL drv-edu 13.0 с - в журнале нет «\[KIRA DRV\] edu: устройство .* 1234:11e8 -> драйвер запущен как drv-edu \(pid \d+\), SHA-256 сверен, права: CAP_MMIO CAP_IRQ CAP_DMA CAP_PCIDEV, прерывание MSI включено»
OK   drvget-rl 58.8 с 
OK   drvman-edu 33.4 с 
OK   isolation 46.1 с 
OK   limbo-mttcg 33.3 с 
OK   limbo-spam 32.8 с 
OK   mind-levers 168.1 с 
OK   mind-seed 11.2 с 
OK   net-e1000 20.8 с 
OK   net-e1000e 18.4 с 
OK   net-ipv6 32.1 с 
OK   net-virtio 101.3 с 
OK   power-off 6.9 с 
OK   power-reboot 14.0 с 
OK   smp 22.1 с 
OK   soak-8cpu 37.7 с 
OK   sound-ac97 14.1 с 
OK   sound-hda 25.6 с 
OK   sound-sb16 13.7 с 
OK   stress-3d 15.9 с 
OK   stress-spam-2 18.3 с 
OK   stress-spam-4 18.3 с 
OK   stress-spam 18.5 с 
OK   usb-uhci 11.7 с 
OK   usb-xhci 12.0 с 
FAIL wasm-speed 13.0 с - в журнале нет «wgfx: frame0=0000000054f339aa frame30=000000000943d884»
OK   arm64-cc 14.0 с 
OK   arm64-crash 4.6 с 
OK   arm64-dma-nc 10.3 с 
OK   arm64-drv 25.9 с 
OK   arm64-drvman 38.9 с 
OK   arm64-el2-8cpu 1.7 с 
OK   arm64-explorer2 32.5 с 
OK   arm64-fault 1.6 с 
OK   arm64-gicv2 18.8 с 
OK   arm64-gicv3 28.2 с 
OK   arm64-gui 48.8 с 
OK   arm64-isolation 15.8 с 
OK   arm64-overflow 1.7 с 
OK   arm64-power 6.2 с 
OK   arm64-ramfb 4.3 с 
OK   arm64-usb 10.4 с 
