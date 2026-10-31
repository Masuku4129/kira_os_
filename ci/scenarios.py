"""Сценарии стенда. Шаги:
  {"cmd": "команда", "must": [regex...], "forbid": [regex...], "timeout": с}  - выполнить и проверить вывод
  {"send": "команда"}                    - отправить, не дожидаясь приглашения (запуск программ под нагрузкой)
  {"sleep": с}                           - подождать (журнал при этом читается и проверяется на аварии)
  {"shot": "имя"}                        - снимок экрана
  {"log": [regex...]}, {"log_forbid": [regex...]} - проверки всего журнала с начала
  {"responsive": "команда", "within": с} - система отвечает на команду не дольше заданного (стабильность под нагрузкой)
Сроки - для KVM; без KVM стенд умножает их (--timeout-scale)."""

FLASH = "ci-out/flash.img"          # создаёт ci/make_flash.sh (FAT32 + файл hello.txt)

BOOT_LOG = [r"\[KIRA SCHED\] Preemptive scheduler online", r"\[KIRA MIND\] Mind online", r"\[KIRA CONSOLE\] Command line ready",
            r"\[KIRA FS\] KiraFS mounted"]
NO_ERR = [r"kernel-mode fault", r"\[KIRA IRET\]", r"\[KIRA GS\]", r"\[KIRA VMM ERROR\]", r"Kernel thread stack overflow", r"bad free|double free", r"\[KIRA SMP\].*did not answer"]

BASIC = [
    {"log": BOOT_LOG},
    {"cmd": "test mem", "must": [r"heap: PASS", r"PMM: PASS", r"VMM: PASS"]},
    {"cmd": "disk", "must": [r"\[системный\]"]},
    {"cmd": "df", "must": [r"KiraFS"]},
    {"cmd": "status", "must": [r"."]},
    {"cmd": "ls /bin", "must": [r"plasma", r"cube3d"]},
    {"log_forbid": NO_ERR},
]


def sc(name, about, steps, **kw):
    d = {"name": name, "about": about, "steps": steps}
    d.update(kw)
    return d


ALL = [
    sc("boot-virtio", "загрузка: virtio-blk, 4 ядра, базовые проверки", BASIC + [
        {"log": [r"Processors online: 4", r"parallel helpers: 3"]},
        {"cmd": "ps", "must": [r"mind-tact", r"par-0"]},
        {"cmd": "ai cap", "must": [r"предел загрузки ядер фоном сейчас 50%"]},
        {"cmd": "ai cap 70", "must": [r"70%"]},
        {"cmd": "ai", "must": [r"предвидение: риск опоздания экрана", r"модель обучена на \d+ с"]},
        {"cmd": "ai model", "must": [r"фоновые программы: -?\d+\.\d\d", r"обучено на \d+ с"]},
    ]),
    sc("boot-ata", "загрузка: IDE (как в Limbo)", BASIC, disk="ata", smp=2),
    sc("boot-sata", "загрузка: AHCI (SATA) на плате q35", BASIC + [{"log": [r"\[KIRA AHCI\] sata0"]}], disk="sata", machine="q35"),
    sc("boot-nvme", "загрузка: NVMe", BASIC + [{"log": [r"\[KIRA NVMe\] nvme0n1"]}], disk="nvme"),
    sc("boot-1cpu", "одно ядро, 256 МБ", BASIC, smp=1, mem="256M"),
    sc("boot-8cpu", "восемь ядер, 4 ГБ (память выше 1 ГБ)", BASIC + [{"log": [r"Processors online: 8", r"Mapped RAM above 1 GB: [1-9]"]}], smp=8, mem="4G"),
    sc("isolation", "изоляция: адресные пространства, враждебные программы, IPC, потоки, FPU", [
        {"log": BOOT_LOG},
        {"cmd": "test isolation"}, {"sleep": 6},
        {"cmd": "test atk"}, {"sleep": 6},
        {"cmd": "test ipc"}, {"sleep": 8},
        {"cmd": "test threads"}, {"sleep": 6},
        {"cmd": "test fpu"}, {"sleep": 8},
        {"log": [r"address spaces are isolated \(PASS\)", r"kernel GS intact after the attack \(PASS\)",
                 r"kernel pointer passed to write\(\) was rejected \(PASS\)", r"mt: .* - PASS|- PASS"]},
        {"log_forbid": [r"\(FAIL\)", r"isolation broken", r"ipc (server|client): FAIL", r"mt: FAIL"] + NO_ERR},
        {"responsive": "status", "within": 5},
    ]),
    sc("soak-8cpu", "8 ядер, 30 с жизни с фоновой нагрузкой: зависаний и аварий нет", [
        {"log": BOOT_LOG}, {"sleep": 8}, {"send": "plasma"}, {"sleep": 10}, {"cmd": "test smp"}, {"sleep": 12},
        {"responsive": "status", "within": 3},
        {"log_forbid": NO_ERR + [r"KERNEL PANIC"]},
    ], smp=8, mem="4G"),
    sc("smp", "проверка ядер: расчёт, замок, вытеснение на всех ядрах", [
        {"log": [r"Processors online: 8"]},
        {"cmd": "test smp", "must": [r"started \d+ worker"]},
        {"sleep": 15},
        {"log": [r"RESULT: PASS"]},
        {"log_forbid": [r"RESULT: FAIL", r"MOVED to another core", r"LOST UPDATES"]},
        {"responsive": "status", "within": 3},
        {"log_forbid": NO_ERR},
    ], smp=8, mem="2G"),
    sc("usb-xhci", "USB xHCI: хаб, клавиатура, планшет, флешка FAT32 -> /usb0", [
        {"sleep": 4},
        {"cmd": "usb", "must": [r"xHCI", r"хаб", r"клавиатура", r"планшет"]},
        {"cmd": "df", "must": [r"usb0"]},
        {"cmd": "cat /usb0/hello.txt", "must": [r"kira-ci"]},
        {"cmd": "cp /bin/plasma /usb0/plasma.bin", "forbid": [r"ошибк|error"]},
        {"cmd": "ls /usb0", "must": [r"plasma.bin"]},
        {"cmd": "umount usb0"},
        {"log_forbid": NO_ERR},
    ], extra=["-device", "qemu-xhci,id=xhci", "-device", "usb-hub,bus=xhci.0,port=1",
              "-device", "usb-kbd,bus=xhci.0,port=1.2", "-device", "usb-tablet,bus=xhci.0,port=2"], flash=FLASH, hub=True),
    sc("usb-uhci", "USB UHCI (как в Limbo): планшет, клавиатура, флешка", [
        {"sleep": 4},
        {"cmd": "usb", "must": [r"UHCI", r"клавиатура|планшет"]},
        {"cmd": "cat /usb0/hello.txt", "must": [r"kira-ci"]},
        {"log_forbid": NO_ERR},
    ], extra=["-usb", "-device", "usb-tablet", "-device", "usb-kbd"], flash=FLASH),
    sc("stress-spam", "20 запусков plasma подряд: система отвечает, ядра загружены равномерно, Разум держит отклик", [
        {"log": BOOT_LOG},
        *[{"send": "plasma"} for _ in range(20)],
        {"sleep": 10}, {"shot": "spam"},
        {"log": [r"не допущен"]},                         # бюджет Разума: 20 копий за секунду не проходят
        {"responsive": "status", "within": 3},
        {"cmd": "ps", "must": [r"plasma"]},
        {"cmd": "smp"},
        {"cmd": "ai", "must": [r"замок планировщика: ядра ждут его \d+,\d%"], "timeout": 10},
        {"responsive": "status", "within": 3},
        {"log_forbid": NO_ERR},
    ], smp=8, mem="2G"),
    sc("limbo-spam", "как Limbo (эмуляция одним потоком, 4 ядра, IDE): 8 plasma - Разум держит экран (темп фона, сборка на одном ядре)", [
        {"log": BOOT_LOG},
        *[{"send": "plasma"} for _ in range(8)],
        {"sleep": 25}, {"shot": "limbo-spam"},
        {"responsive": "status", "within": 10},
        {"cmd": "ai", "must": [r"тактика: фон", r"замок планировщика: ядра ждут его", r"композитор: .*параллельно [1-9]\d* нс, на одном ядре [1-9]"], "timeout": 20},
        {"cmd": "ai why", "timeout": 20},
        {"cmd": "ai levers", "must": [r"темп фоновых окон", r"сборка кадра"], "timeout": 20},
        {"log": [r"фоновые окна не чаще \d+ кадров/с"]},   # режим сборки выбирает замер (оба режима измерены - выше)
        {"log_forbid": NO_ERR + [r"фон ужат до [0-4]% ядер"]},   # фон не морится голодом при свободных ядрах
    ], smp=4, mem="1G", disk="ata", limbo=True),
    sc("stress-3d", "cube3d + plasma x4 + burnall: отклик консоли", [
        {"send": "3d"}, *[{"send": "plasma"} for _ in range(4)], {"send": "burnall"},
        {"sleep": 8}, {"shot": "load"},
        {"responsive": "status", "within": 3},
        {"log": [r"cube3d: selftest PASS"]},
        {"cmd": "ai", "must": [r"замок планировщика: ядра ждут его \d+,\d%"], "timeout": 10},
        {"log_forbid": NO_ERR},
    ], smp=4, mem="1G"),
]
