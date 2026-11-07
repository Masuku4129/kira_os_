# Прогон 7504bda (v46/kdrv-auth-integration-final)

| Проверка | Результат |
|---|---|
| audit | success |
| host-tests | success |
| x86_64 build | success |
| x86_64 QEMU matrix | failure |
| ARM64 build | failure |
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
- FAIL arm64-drvman: FAIL — @log drvman: пакет edu \| edu\s+v\d+\s+лицензия: не записана\s+ЗАПУЩЕН\s+сверка: SHA-256 сходится: нет в журнале
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
- FAIL drvman-edu: FAIL — в журнале нет «drvman: пакет edu \| edu\s+v\d+\s+лицензия: не записана\s+ЗАПУЩЕН\s+сверка: SHA-256 сходится»
- isolation: PASS
- net-e1000: PASS
- FAIL apps-drvman: FAIL — не дождались «rl: версия 1, права: MMIO DMA PCIDEV NETDEV, SHA-256 сверен, источник: https://10\.0\.2\.100/rl/drvget\.rec» за 30 с
- net-e1000e: PASS
- smp: PASS
- sound-hda: PASS
- apps-edit: PASS
- drvman-quiet: PASS
- sound-ac97: PASS
- usb-xhci: PASS

QEMU result files: 56
