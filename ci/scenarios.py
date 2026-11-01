"""Сценарии стенда. Шаги:
  {"cmd": "команда", "must": [regex...], "forbid": [regex...], "timeout": с}  - выполнить и проверить вывод
  {"send": "команда"}                    - отправить, не дожидаясь приглашения (запуск программ под нагрузкой)
  {"sleep": с}                           - подождать (журнал при этом читается и проверяется на аварии)
  {"shot": "имя"}                        - снимок экрана
  {"log": [regex...]}, {"log_forbid": [regex...]} - проверки всего журнала с начала
  {"responsive": "команда", "within": с} - система отвечает на команду не дольше заданного (стабильность под нагрузкой)
Сроки - для KVM; без KVM стенд умножает их (--timeout-scale)."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfiles import SUMS

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


# Диск на прерываниях: запись и чтение файла, затем счётчик сообщений MSI в команде disk
DISK_IRQ = [{"cmd": "write /irqtest.txt kira-irq"}, {"cmd": "cat /irqtest.txt", "must": [r"kira-irq"]},
            {"cmd": "disk", "must": [r"прерываний устройств \(MSI\): [1-9]"]}]

HTTP_ONCE = os.path.abspath(os.path.join(os.path.dirname(__file__), "http_once.py"))

ALL = [
    sc("boot-virtio", "загрузка: virtio-blk, 4 ядра, базовые проверки", BASIC + [
        {"log": [r"Processors online: 4", r"parallel helpers: 3", r"virtio-blk, .*completion by MSI-X interrupt"]},
        *DISK_IRQ,
        {"cmd": "ps", "must": [r"mind-tact", r"par-0"]},
        {"cmd": "ai cap", "must": [r"предел загрузки ядер фоном сейчас 50%"]},
        {"cmd": "ai cap 70", "must": [r"70%"]},
        {"cmd": "ai", "must": [r"предвидение: риск опоздания экрана", r"модель обучена на \d+ с"]},
        {"cmd": "ai model", "must": [r"фоновые программы: -?\d+\.\d\d", r"обучено на \d+ с"]},
    ]),
    sc("boot-ata", "загрузка: IDE (как в Limbo)", BASIC, disk="ata", smp=2),
    sc("boot-sata", "загрузка: AHCI (SATA) на плате q35", BASIC + [{"log": [r"\[KIRA AHCI\] sata0", r"completion by MSI interrupt"]}] + DISK_IRQ, disk="sata", machine="q35"),
    sc("boot-nvme", "загрузка: NVMe", BASIC + [{"log": [r"\[KIRA NVMe\] nvme0n1", r"NVMe .*completion by interrupt"]}] + DISK_IRQ, disk="nvme"),
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
        {"cmd": "ai levers", "must": [r"темп фоновых окон", r"сборка кадра", r"12\. ядро фокуса"], "timeout": 20},
        {"log": [r"фоновые окна не чаще \d+ кадров/с"]},   # режим сборки выбирает замер (оба режима измерены - выше)
        {"log_forbid": NO_ERR + [r"фон ужат до [0-4]% ядер"]},   # фон не морится голодом при свободных ядрах
    ], smp=4, mem="1G", disk="ata", limbo=True),
    sc("net-e1000", "сеть: Intel e1000 (как в Limbo), DHCP от QEMU, ping шлюза", [
        {"log": BOOT_LOG},
        {"sleep": 8},
        {"log": [r"\[KIRA NET\] e1000 .*MAC 52:54:00", r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24, gateway 10\.0\.2\.2"]},
        {"cmd": "net", "must": [r"канал есть", r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"ответ от 10\.0\.2\.2", r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"cmd": "http 10.0.2.100 /kira", "must": [r"HTTP/1\.0 200 OK", r"kira-tcp-ok GET /kira HTTP/1\.0",
                                                    r"получено 3\d\d\d байт от 10\.0\.2\.100:80 .*сервер закрыл соединение"], "timeout": 40},
        {"cmd": "dns example.com", "must": [r"example\.com -> \d+\.\d+\.\d+\.\d+"], "timeout": 20},
        {"cmd": "fetch 10.0.2.100 /prog"}, {"sleep": 3},                 # программа Ring 3: сеть через системные вызовы
        {"log": [r"fetch: 10\.0\.2\.100/prog - HTTP/1\.0 200 OK - 3\d\d\d bytes in \d+ ms \(server closed\), saved"]},
        {"cmd": "cat /data/fetch/last.txt", "must": [r"kira-tcp-ok GET /prog HTTP/1\.0"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE, "-device", "e1000,netdev=n0"]),
    sc("apps-edit", "редактор: окно, набор на клавиатуре, сохранение в песочницу, файл виден в /data/edit", [
        {"log": BOOT_LOG},
        {"combo": ["alt", "shift"]}, {"keys": "ghbdtn\n"}, {"sleep": 1},      # кириллица в строке терминала (Alt+Shift)
        {"log": [r"неизвестная команда: привет"]},
        {"combo": ["alt", "shift"]},
        {"cmd": "run edit"}, {"sleep": 2},
        {"keys": "hello kira\nline two", "then": ["f2"]}, {"sleep": 2}, {"shot": "edit"},
        {"log": [r"edit: note\.txt - сохранено: 19 байт"]},
        {"cmd": "cat /data/edit/note.txt", "must": [r"hello kira", r"line two"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M"),
    sc("apps-view", "просмотр: PNG (фильтры, dynamic/stored), BMP, PPM, текст, двоичный - разбор в Ring 3, суммы пикселей против эталона", [
        {"log": BOOT_LOG},
        *[x for f, s in [("/pic.png", SUMS["/pic.png"]), ("/rgba.png", SUMS["/rgba.png"]), ("/pic.bmp", SUMS["/pic.bmp"]), ("/pic.ppm", SUMS["/pic.ppm"])]
          for x in ({"cmd": "view " + f}, {"sleep": 2}, {"log": [r"view: %s - .* sum %08x" % (re.escape(f), s)]},
                    {"shot": "view-" + f.strip("/").replace(".", "-")}, {"keys": "q"}, {"sleep": 1})],   # q - закрыть окно
        {"cmd": "view /note.txt"}, {"sleep": 2}, {"shot": "view-text"}, {"keys": "q"}, {"sleep": 1},
        {"cmd": "view /blob.bin"}, {"sleep": 2},
        {"log": [r"view: /note\.txt - текст, 31 байт", r"view: /blob\.bin - двоичный, 512 байт"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="1G", files=True),
    sc("apps-wasm", "WebAssembly: тот же /wasm/wtest.wasm, что у ARM64, - результат равен эталону, собранному обычным компилятором", [
        {"log": BOOT_LOG},
        {"cmd": "wasm /wasm/wtest.wasm"}, {"sleep": 4},
        {"log": [r"wasm: /wasm/wtest\.wasm - \d+ functions", r"wtest:\ fib\(24\)=46368\ primes<20000=2262\ sort=ad642057dcc4b702\ switch=4148393079\ ops=ba422c8d525a9aea\ sext=269715", r"wasm: finished with code 0"]},
        {"log_forbid": NO_ERR + [r"wasm: stopped"]},
    ], smp=2, mem="512M"),
    sc("sound-hda", "звук: Intel HDA (как Sound: hda в Limbo) - кодек, путь к выходу, тон через DMA", [
        {"log": BOOT_LOG + [r"\[KIRA HDA\] Sound: codec .* output path pin \d+ <- DAC \d+"]},
        {"cmd": "sound", "must": [r"выход: вывод \d+ <- ЦАП \d+"]},
        {"cmd": "beep 440 400", "must": [r"DMA прошёл [1-9]\d* байт - карта играет"], "timeout": 15},
        {"log": [r"\[KIRA AUDIO\] Mixer online"]},
        {"cmd": "melody"}, {"sleep": 5},                                   # программа: звук через k_snd_* и микшер ядра
        {"log": [r"melody: 8 notes, 76800 frames queued"]},
        {"cmd": "sound", "must": [r"микшер: работает", r"от программ смешано (7[6-9]|[89]\d)\d\d\d"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-device", "intel-hda", "-device", "hda-output,audiodev=a0"]),
    sc("net-virtio", "сеть: virtio-net (MSI-X будит поток сети), DHCP, ping, программа fetch", [
        {"log": BOOT_LOG}, {"sleep": 8},
        {"log": [r"\[KIRA NET\] virtio-net: MAC 52:54:00.*MSI-X", r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
        {"cmd": "net", "must": [r"карта virtio-net, канал есть", r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"cmd": "fetch 10.0.2.100 /virtio"}, {"sleep": 3},
        {"log": [r"fetch: 10\.0\.2\.100/virtio - HTTP/1\.0 200 OK - 3\d\d\d bytes .*server closed"]},
        {"cmd": "serve"}, {"sleep": 2}, {"log": [r"serve: listening on port 8080"]},           # Кира - сервер: запрос с хозяина
        {"host_http": [18080, "/hello"]}, {"host_http": [18080, "/again"]}, {"sleep": 4},   # строку о запросе serve пишет после закрытия соединения
        {"log": [r"\[HOST\] http /hello: 19\d\d\d bytes, first line 'kira-server-ok /hello', window lines 900",
                 r"\[HOST\] http /again: 19\d\d\d bytes, first line 'kira-server-ok /again'", r"serve: GET /again -> 200"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE + ",hostfwd=tcp:127.0.0.1:18080-:8080",
                                 "-device", "virtio-net-pci,netdev=n0"]),
    sc("limbo-mttcg", "Limbo с галочкой Enable MTTCG (эмуляция потоком на ядро): 8 plasma - сравнить сборку и отклик с limbo-spam", [
        {"log": BOOT_LOG},
        *[{"send": "plasma"} for _ in range(8)],
        {"sleep": 25}, {"shot": "limbo-mttcg"},
        {"responsive": "status", "within": 10},
        {"cmd": "ai", "must": [r"замок планировщика: ядра ждут его", r"композитор: .*параллельно [1-9]\d* нс, на одном ядре [1-9]"], "timeout": 20},
        {"cmd": "smp", "timeout": 20},
        {"log_forbid": NO_ERR},
    ], smp=4, mem="1G", disk="ata", limbo=True, mttcg=True),
    sc("stress-3d", "cube3d + plasma x4 + burnall: отклик консоли", [
        {"send": "3d"}, *[{"send": "plasma"} for _ in range(4)], {"send": "burnall"},
        {"sleep": 8}, {"shot": "load"},
        {"responsive": "status", "within": 3},
        {"log": [r"cube3d: selftest PASS"]},
        {"cmd": "ai", "must": [r"замок планировщика: ядра ждут его \d+,\d%"], "timeout": 10},
        {"log_forbid": NO_ERR},
    ], smp=4, mem="1G"),
    sc("power-off", "выключение: ACPI S5 из FADT/DSDT (_S5) - машина выключается сама, без -no-reboot-уловок", [
        {"log": BOOT_LOG},
        {"exit": "poweroff", "within": 20},
        {"log": [r"\[KIRA ACPI\] powering off \(S5\)"]},
        {"log_forbid": NO_ERR + [r"did not power off"]},
    ], smp=2, mem="512M"),
    sc("power-reboot", "перезагрузка на q35 (регистр сброса FADT / 0xCF9): система поднимается снова, файл, записанный до сброса, на месте", [
        {"log": BOOT_LOG},
        {"cmd": "write /reboot.txt kira-before-reboot"},
        {"send": "reboot"},
        {"expect": r"\[KIRA ACPI\] rebooting", "timeout": 10},
        {"expect": r"\[KIRA CONSOLE\] Command line ready", "timeout": 120},
        {"sleep": 2},
        {"cmd": "cat /reboot.txt", "must": [r"kira-before-reboot"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", machine="q35", reboot=True),
    sc("crash-isolation", "изоляция сбоев ядра: поток ядра ломается - гибнет только он; ядро 2 «отказывает» (машинная проверка) - выходит из работы, система живёт", [
        {"log": BOOT_LOG},
        {"cmd": "test crash thread"}, {"sleep": 2},
        {"log": [r"\[KIRA CORE \d+\] kernel-mode fault #14 at RIP 0x[0-9A-Fa-f]+, thread 'smpcrash'\. Thread terminated"]},
        {"cmd": "test crash 2"}, {"sleep": 3},
        {"log": [r"\[KIRA CORE 2\] machine check #18", r"\[KIRA CORE 2\] out of service"]},
        {"responsive": "status", "within": 5},
        {"cmd": "smp", "timeout": 10},                                         # состояние ядер после отказа 2-го
        {"send": "plasma"}, {"sleep": 4}, {"responsive": "status", "within": 5},
        {"log_forbid": [x for x in NO_ERR if x != r"kernel-mode fault"]},
    ], smp=4, mem="512M"),
]
