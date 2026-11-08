# Прогон 085325a (research/driver-adaptation-lab-2026-11)

host-tests: success
data-lab: success
build: success
vm matrix: failure
arm64: success
global gate: failure

OK   apps-drvman 41.4 с 
OK   apps-edit 14.7 с 
OK   apps-explorer2 27.1 с 
OK   apps-view 48.5 с 
OK   apps-wasm 45.9 с 
OK   boot-1cpu 7.1 с 
OK   boot-8cpu 7.4 с 
OK   boot-ata 7.4 с 
OK   boot-nvme 7.5 с 
OK   boot-sata 7.5 с 
OK   boot-virtio 7.5 с 
OK   cc-native 70.0 с 
OK   crash-isolation 16.5 с 
OK   drv-edu 29.3 с 
OK   drvget-rl 58.0 с 
OK   drvman-edu 33.2 с 
OK   isolation 46.6 с 
OK   limbo-mttcg 33.3 с 
OK   limbo-spam 33.5 с 
OK   mind-levers 168.2 с 
OK   mind-seed 11.4 с 
OK   net-e1000 20.8 с 
OK   net-e1000e 18.4 с 
OK   net-ipv6 32.1 с 
OK   net-virtio 101.7 с 
OK   power-off 6.6 с 
OK   power-reboot 13.9 с 
OK   smp 22.5 с 
OK   soak-8cpu 37.7 с 
OK   sound-ac97 14.4 с 
OK   sound-hda 25.3 с 
OK   sound-sb16 13.5 с 
OK   stress-3d 15.7 с 
OK   stress-spam-2 18.1 с 
OK   stress-spam-4 18.4 с 
FAIL stress-spam 16.1 с - авария ядра:
OK   usb-uhci 11.5 с 
OK   usb-xhci 11.8 с 
OK   wasm-speed 13.2 с 
OK   arm64-cc 13.8 с 
OK   arm64-crash 4.6 с 
OK   arm64-dma-nc 10.3 с 
OK   arm64-drv 25.8 с 
OK   arm64-drvman 38.9 с 
OK   arm64-el2-8cpu 1.7 с 
OK   arm64-explorer2 32.5 с 
OK   arm64-fault 1.5 с 
OK   arm64-gicv2 18.5 с 
OK   arm64-gicv3 28.6 с 
OK   arm64-gui 48.8 с 
OK   arm64-isolation 15.7 с 
OK   arm64-overflow 1.7 с 
OK   arm64-power 6.2 с 
OK   arm64-ramfb 4.3 с 
OK   arm64-usb 10.5 с 
