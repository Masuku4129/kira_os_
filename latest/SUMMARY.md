# Прогон 64c645d (v46/kdrv-tool-ignore)

| Проверка | Результат |
|---|---|
| audit | success |
| host-tests | failure |
| x86_64 build | success |
| x86_64 QEMU matrix | failure |
| ARM64 build | success |
| ARM64 QEMU (full suite only) | missing |
| Mind quality | success |
| GUI regression | success |
| KCC compiler suite | success |
| Driver installer end-to-end | success |

## QEMU result files

- arm64-cc: PASS
- arm64-crash: PASS
- arm64-dma-nc: PASS
- arm64-drv: PASS
- arm64-drvman: PASS
- arm64-el2-8cpu: PASS
- arm64-explorer2: PASS
- arm64-fault: PASS
- arm64-gicv2: PASS
- arm64-gicv3: PASS
- arm64-gui: PASS
- arm64-isolation: PASS
- arm64-overflow: PASS
- arm64-power: PASS
- arm64-ramfb: PASS
- arm64-usb: PASS
- apps-explorer2: PASS
- boot-virtio: PASS
- net-ipv6: PASS
- usb-uhci: PASS
- apps-view: PASS
- boot-ata: PASS
- sound-sb16: PASS
- stress-spam: PASS
- boot-sata: PASS
- mind-levers: PASS
- net-virtio: PASS
- stress-spam-2: PASS
- apps-wasm: PASS
- boot-nvme: PASS
- mind-seed: PASS
- stress-spam-4: PASS
- boot-1cpu: PASS
- cc-native: PASS
- stress-3d: PASS
- wasm-speed: PASS
- boot-8cpu: PASS
- drv-edu: PASS
- limbo-spam: PASS
- power-off: PASS
- drvget-rl: PASS
- limbo-mttcg: PASS
- power-reboot: PASS
- soak-8cpu: PASS
- crash-isolation: PASS
- drvman-edu: PASS
- isolation: PASS
- net-e1000: PASS
- FAIL apps-drvman: FAIL — «drv»: в выводе нет «rl: версия 1, SHA-256 сверен, запрошено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV, подтверждено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV»
- net-e1000e: PASS
- smp: PASS
- sound-hda: PASS
- apps-edit: PASS
- drvman-quiet: PASS
- sound-ac97: PASS
- usb-xhci: PASS

QEMU result files: 56
