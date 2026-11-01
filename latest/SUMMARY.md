# Прогон 0de152b (pkg-H)

host-tests: success, build: success, vm: failure, arm64: failure

OK   apps-edit 14.5 с 
OK   apps-view 41.4 с 
OK   apps-wasm 34.4 с 
OK   boot-1cpu 7.1 с 
OK   boot-8cpu 7.2 с 
OK   boot-ata 7.2 с 
OK   boot-nvme 7.5 с 
OK   boot-sata 7.4 с 
OK   boot-virtio 7.8 с 
FAIL crash-isolation 9.1 с - в журнале нет «\[KIRA CORE\] fault detail: error 0x[0-9A-Fa-f]+, CR2 0x[0-9A-Fa-f]+, RSP 0x[0-9A-Fa-f]+, RBP 0x[0-9A-Fa-f]+, stack: 0x»
OK   isolation 46.1 с 
OK   limbo-mttcg 32.7 с 
OK   limbo-spam 33.3 с 
OK   net-e1000 20.4 с 
OK   net-e1000e 18.3 с 
OK   net-virtio 46.7 с 
OK   power-off 6.7 с 
OK   power-reboot 14.0 с 
OK   smp 22.5 с 
OK   soak-8cpu 37.8 с 
OK   sound-ac97 13.9 с 
OK   sound-hda 25.6 с 
OK   stress-3d 15.6 с 
OK   stress-spam-2 18.4 с 
OK   stress-spam-4 18.2 с 
OK   stress-spam 18.3 с 
OK   usb-uhci 11.3 с 
OK   usb-xhci 11.8 с 
OK   arm64-crash 4.6 с 
OK   arm64-dma-nc 10.3 с 
OK   arm64-el2-8cpu 2.1 с 
OK   arm64-fault 1.6 с 
OK   arm64-gicv2 18.6 с 
OK   arm64-gicv3 26.5 с 
OK   arm64-gui 48.8 с 
FAIL arm64-isolation 67.1 с - test caps: нет «\[KIRA CAPS\] process 'atk-cap' .* denied, no right CAP_GFX»
OK   arm64-overflow 1.7 с 
OK   arm64-power 6.1 с 
OK   arm64-ramfb 4.5 с 
OK   arm64-usb 10.1 с 
