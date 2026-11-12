# Прогон aa5378d (v46/kira-vm-core-v1)

| Проверка | Результат |
|---|---|
| audit | success |
| host-tests | cancelled |
| x86_64 build | success |
| x86_64 QEMU matrix | failure |
| ARM64 build | failure |
| ARM64 QEMU (full suite only) | missing |
| Mind quality | success |
| GUI regression | success |
| KCC compiler suite | success |
| Driver installer end-to-end | success |
| UEFI OVMF boot | success |

## QEMU result files

- FAIL arm64-cc: FAIL — cc bad.c bad: нет «bad\.c:9: error: »
- arm64-crash: PASS
- arm64-dma-nc: PASS
- arm64-drv: PASS
- FAIL arm64-drvman: FAIL — @log drvman: драйвер edu запущен на устройстве \d\d:\d\d\.\d \(1234:11e8\): нет в журнале
- arm64-el2-8cpu: PASS
- arm64-explorer2: PASS
- arm64-fault: PASS
- arm64-gicv2: PASS
- arm64-gicv3: PASS
- FAIL arm64-gui: FAIL — cat /data/edit/note.txt: нет «hello kira»
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
- FAIL drv-edu: FAIL — в журнале нет «\[KIRA DRV\] edu: возвращено памяти DMA и колец 0 из 0 стр\..*утечек нет»
- limbo-spam: PASS
- power-off: PASS
- FAIL drvget-rl: FAIL — авария ядра:
- limbo-mttcg: PASS
- power-reboot: PASS
- soak-8cpu: PASS
- crash-isolation: PASS
- FAIL drvman-edu: FAIL — авария ядра:
- isolation: PASS
- net-e1000: PASS
- FAIL apps-drvman: FAIL — авария ядра:
- net-e1000e: PASS
- smp: PASS
- sound-hda: PASS
- apps-edit: PASS
- FAIL drvman-quiet: FAIL — авария ядра:
- sound-ac97: PASS
- usb-xhci: PASS

QEMU result files: 56
