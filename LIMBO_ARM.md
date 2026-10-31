# Kira OS ARM64 в Limbo ARM (телефон)

Сборка в Termux: `./build_arm64.sh` -> `kira-arm64.img` (ядро) и `kira-arm64.disk` (диск с программами).
Скопируйте оба файла в Download.

Настройки машины в Limbo ARM (ARM64):
- Machine: `virt`, CPU: `cortex-a72` (или `max`), ядер: 4, RAM: 1024 MB
- Kernel: `kira-arm64.img` (Initrd и DTB - пусто: дерево устройств даёт сам QEMU)
- Hard disk A: не задавать (диск подключается строкой ниже)
- Display: VNC или SDL; видеоадаптер - none (экран даёт virtio-gpu)
- Дополнительные параметры QEMU (Extra params):
  -drive file=/sdcard/Download/kira-arm64.disk,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0
  -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-tablet-pci
  (сеть: добавить -netdev user,id=n0 -device e1000,netdev=n0; команды net и ping 10.0.2.2)

KVM в Limbo на Android обычно недоступен (нужен root и поддержка в ядре телефона): машина работает эмуляцией TCG.
Если экран чёрный - в журнале последовательного порта (Limbo: Serial / Console) будет строка `[KIRA GPU] ...`
или `[KIRA DISPLAY] No screen`; пришлите её.
