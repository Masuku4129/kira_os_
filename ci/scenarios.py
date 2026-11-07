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
HTTPS_ONCE = os.path.abspath(os.path.join(os.path.dirname(__file__), "https_once.py"))
WGFX = ("0000000054f339aa", "000000000943d884")       # суммы кадров wgfx.wasm - как у эталона, собранного обычным компилятором
TLS_CHAIN = os.path.abspath("ci-out/tlschain")       # создаёт стенд (tls_chain=True): ci/mkchain.py
_G = ",guestfwd=tcp:10.0.2.100:%d-cmd:python3 " + HTTPS_ONCE + " %s"
OCSP_FWD = (_G % (8444, "--chain " + TLS_CHAIN + " --prefix ed_") + _G % (8445, "--chain " + TLS_CHAIN + " --prefix pss_") +
            _G % (8446, "--chain " + TLS_CHAIN + " --ocsp good") + _G % (8447, "--chain " + TLS_CHAIN + " --ocsp revoked") +
            _G % (8448, "--bridge 18449"))        # 8448: постоянный openssl s_server (daemons) - билеты сессий
RESUME_DAEMON = ["openssl", "s_server", "-quiet", "-accept", "18449", "-cert", "{chain}/leaf.pem", "-key", "{chain}/leaf.key",
                 "-cert_chain", "{chain}/inter.pem", "-tls1_3", "-www"]

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
        {"cmd": "test caps"}, {"sleep": 4},
        {"cmd": "test ipc"}, {"sleep": 8},
        {"cmd": "test threads"}, {"sleep": 6},
        {"cmd": "test fpu"}, {"sleep": 8},
        {"log": [r"address spaces are isolated \(PASS\)", r"kernel GS intact after the attack \(PASS\)",
                 r"kernel pointer passed to write\(\) was rejected \(PASS\)", r"mt: .* - PASS|- PASS",
                 r"atk-cap: 15 calls without rights refused, harmless calls work \(PASS\)", r"\[KIRA CAPS\] process 'atk-cap' .* denied, no right CAP_GFX"]},
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
        {"cmd": "cp /bin/plasma /usb0/plasma.bin", "must": [r"том подключён только для чтения"]},
        {"cmd": "ls /usb0", "must": [r"hello.txt", r"big.bin"], "forbid": [r"plasma.bin"]},
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
        {"log": [r"\[KIRA NET\] e1000 \(8086:100e\).*no MSI on this card - polled every tick"]},
        {"log": [r"\[KIRA NET\] IPv6: fec0::5054:ff:fe12:3456 \(SLAAC /64, DAD passed\) ready"]},   # RA приходит на групповой MAC: фильтр MTA карты
        {"cmd": "ping6 fec0::2 2", "must": [r"ответ от fec0::2", r"отправлено 2, получено [1-2]"], "timeout": 20},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,ipv4=on,ipv6=on,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE, "-device", "e1000,netdev=n0"]),
    sc("net-e1000e", "сеть: Intel 82574 (e1000e) на q35 - приём по прерыванию MSI, DHCP, TCP, программа fetch", [
        {"log": BOOT_LOG}, {"sleep": 8},
        {"log": [r"\[KIRA NET\] e1000 \(8086:10d3\).*receive by MSI interrupt", r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
        {"cmd": "fetch 10.0.2.100 /e1000e"}, {"sleep": 3},
        {"log": [r"fetch: 10\.0\.2\.100/e1000e - HTTP/1\.0 200 OK - 3\d\d\d bytes .*server closed"]},
        {"cmd": "net", "must": [r"канал есть", r"прерываний приёма [1-9]\d*"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", machine="q35", extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE, "-device", "e1000e,netdev=n0"]),
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
    sc("apps-explorer2", "двухпанельный проводник (команда files): клавиатурой Tab, F5 копия, F6 перенос, F7 каталог, F2 имя, F8 удаление с подтверждением; результат проверяется командами ls/cat", [
        {"log": BOOT_LOG},
        {"cmd": "mkdir /fm-a"}, {"cmd": "mkdir /fm-b"}, {"cmd": "write /fm-a/one.txt kira-one"}, {"cmd": "write /fm-a/two.txt kira-two"},
        {"cmd": "files /fm-a /fm-b"}, {"sleep": 3}, {"shot": "explorer2-open"},
        {"log": [r"\[KIRA FILES\] explorer: /fm-a \| /fm-b"]},
        {"keys": "", "then": ["down", "f5"]}, {"sleep": 2}, {"shot": "explorer2-copy"},                    # курсор на one.txt: копия в правую панель
        {"log": [r"\[KIRA FILES\] copy /fm-a/one\.txt -> /fm-b/one\.txt: 9 байт"]},
        {"cmd": "cat /fm-b/one.txt", "must": [r"kira-one"]}, {"cmd": "cat /fm-a/one.txt", "must": [r"kira-one"]},
        {"keys": "", "then": ["down", "f6"]}, {"sleep": 2},                                              # two.txt: перенос
        {"log": [r"\[KIRA FILES\] move /fm-a/two\.txt -> /fm-b/two\.txt: 9 байт"]},
        {"cmd": "cat /fm-b/two.txt", "must": [r"kira-two"]},
        {"cmd": "ls /fm-a", "must": [r"one\.txt"], "forbid": [r"two\.txt"]},
        {"keys": "", "then": ["tab", "f7"]}, {"sleep": 1}, {"shot": "explorer2-mkdir"},                   # правая панель: новый каталог
        {"keys": "sub\n"}, {"sleep": 2},
        {"log": [r"\[KIRA FILES\] mkdir /fm-b/sub"]},
        {"keys": "", "then": ["down", "f8"]}, {"sleep": 1}, {"shot": "explorer2-delete"},               # one.txt: подтверждение
        {"keys": "", "then": ["ret"]}, {"sleep": 2},
        {"log": [r"\[KIRA FILES\] delete /fm-b/one\.txt"]},
        {"cmd": "ls /fm-b", "must": [r"sub", r"two\.txt"], "forbid": [r"one\.txt"]},
        {"keys": "", "then": ["f2", "backspace", "backspace", "backspace"]}, {"keys": "dat\n"}, {"sleep": 2}, {"shot": "explorer2-rename"},   # two.txt -> two.dat
        {"log": [r"\[KIRA FILES\] rename /fm-b/two\.txt -> /fm-b/two\.dat"]},
        {"cmd": "cat /fm-b/two.dat", "must": [r"kira-two"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M"),
    sc("apps-view", "просмотр: PNG (фильтры, dynamic/stored), BMP, PPM, GIF, JPEG базовый и прогрессивный (отказ при буфере >4 МБ), текст, двоичный - разбор в Ring 3, суммы пикселей против эталона", [
        {"log": BOOT_LOG},
        *[x for f, s in [("/pic.png", SUMS["/pic.png"]), ("/rgba.png", SUMS["/rgba.png"]), ("/pic.bmp", SUMS["/pic.bmp"]), ("/pic.ppm", SUMS["/pic.ppm"]), ("/pic.gif", SUMS["/pic.gif"]), ("/lace.gif", SUMS["/lace.gif"])]
          for x in ({"cmd": "view " + f}, {"sleep": 2}, {"log": [r"view: %s - .* sum %08x" % (re.escape(f), s)]},
                    {"shot": "view-" + f.strip("/").replace(".", "-")}, {"keys": "q"}, {"sleep": 1})],   # q - закрыть окно
        {"cmd": "view /pic.jpg"}, {"sleep": 2}, {"shot": "view-jpeg"}, {"keys": "q"}, {"sleep": 1},
        {"cmd": "view /prog.jpg"}, {"sleep": 2}, {"shot": "view-jpeg-progressive"}, {"keys": "q"}, {"sleep": 1},
        {"cmd": "view /prog-rst.jpg"}, {"sleep": 2}, {"keys": "q"}, {"sleep": 1},
        {"cmd": "view /big-prog.jpg"}, {"sleep": 2}, {"keys": "q"}, {"sleep": 1},
        {"log": [r"view: /pic\.jpg - JPEG 97x61, YCbCr 2x2", r"view: /prog\.jpg - JPEG 97x61, YCbCr 2x2, прогрессивный, проходов 10",
                 r"view: /prog-rst\.jpg - JPEG 83x70, YCbCr 2x2, прогрессивный", r"view: /big-prog\.jpg - JPEG: прогрессивный - коэффициенты не помещаются в память"]},
        {"cmd": "view /note.txt"}, {"sleep": 2}, {"shot": "view-text"}, {"keys": "q"}, {"sleep": 1},
        {"cmd": "view /blob.bin"}, {"sleep": 2},
        {"log": [r"view: /note\.txt - текст, 31 байт", r"view: /blob\.bin - двоичный, 512 байт"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="1G", files=True),
    sc("apps-dock", "левая панель (док): значок появляется при открытии окна программы Ring 3 и исчезает при закрытии (закреплённые остаются), русские названия из единой таблицы; снимки панели", [
        {"log": BOOT_LOG},
        {"cmd": "run plasma"}, {"sleep": 3},                          # закреплена по умолчанию: значок и так есть, окно его «зажигает»
        {"log": [r"\[KIRA DOCK\] открыта: plasma «Плазма» \(закреплена\)"]},
        {"cmd": "view /note.txt"}, {"sleep": 2}, {"shot": "dock-view-open"},     # временный значок «Просмотр» ниже черты, плазма позади
        {"log": [r"\[KIRA DOCK\] открыта: view «Просмотр» \(временный значок\)"]},
        {"cmd": "wasm /wasm/wgfx.wasm"}, {"sleep": 1.5}, {"shot": "dock-wasm-open"},   # ещё один временный значок: «Запуск WebAssembly»
        {"log": [r"\[KIRA DOCK\] открыта: wasm «Запуск WebAssembly» \(временный значок\)"]},
        {"sleep": 14},                                                 # wgfx сам завершается: значок убран
        {"log": [r"\[KIRA DOCK\] закрыта: wasm «Запуск WebAssembly» \(значок убран\)"]},
        {"keys": "q"}, {"sleep": 2},                                   # q получает окно сверху - просмотр закрывается
        {"log": [r"\[KIRA DOCK\] закрыта: view «Просмотр» \(значок убран\)"]},
        {"shot": "dock-view-closed"},
        {"keys": "q"}, {"sleep": 2},                                   # теперь сверху плазма: закрытие закреплённой оставляет значок
        {"log": [r"\[KIRA DOCK\] закрыта: plasma «Плазма» \(значок остаётся: закреплена\)"]},
        {"shot": "dock-plasma-closed"},
        {"log_forbid": NO_ERR + [r"wasm: stopped"]},
    ], smp=2, mem="1G", files=True),
    sc("apps-wasm", "WebAssembly: тот же /wasm/wtest.wasm, что у ARM64, - результат равен эталону, собранному обычным компилятором", [
        {"log": BOOT_LOG},
        {"cmd": "wasm /wasm/wtest.wasm"}, {"sleep": 4},
        {"log": [r"wasm: /wasm/wtest\.wasm - \d+ functions", r"wtest:\ fib\(24\)=46368\ primes<20000=2262\ sort=ad642057dcc4b702\ switch=4148393079\ ops=ba422c8d525a9aea\ sext=269715", r"wasm: finished with code 0"]},
        {"cmd": "wasm /wasm/ftest.wasm"}, {"sleep": 6},                    # f32/f64: тот же результат, что у эталона
        {"log": [r"ftest: mandel inside=662 iters=146978 pi=400921f61616ca3a f32sum=000000004004dfe3 hash=225e25f3d90cf161"]},
        {"cmd": "wasm /wasm/wmix.wasm"}, {"sleep": 5}, {"cmd": "wasm /wasm/wmix0.wasm"}, {"sleep": 6},     # вызовы, i64, br_table, память, f64: -O2 и -O0
        {"log": [r"wasm: /wasm/wmix\.wasm - \d+ functions", r"wasm: /wasm/wmix0\.wasm - \d+ functions",
                 r"wmix: rec=d5041c4ef1eb0478 ind=0bdc8491374efd99 i64=df263e1ee75e2d7f sw=607fb6e15329d3f1 loop=7ba9ad70769b882d mem=bed6ff5566642000 f64=ea044c2d18dea474"]},
        {"cmd": "wasm /wasm/wgfx.wasm"}, {"sleep": 1.5}, {"shot": "wasm-gfx"}, {"sleep": 15},   # снимок - пока окно открыто
        {"log": [r"wgfx: frame0=%s frame30=%s" % WGFX, r"wgfx: 31 frames shown, ticks \d+"]},
        {"log_forbid": NO_ERR + [r"wasm: stopped"]},
    ], smp=2, mem="512M"),
    sc("wasm-speed", "WebAssembly: JIT x86-64 - плазма wgfx на стенде укладывается в порог по времени (до JIT на этом стенде было 1257 мс)", [
        {"log": BOOT_LOG},
        {"cmd": "wasm /wasm/wgfx.wasm"}, {"sleep": 6},
        {"log": [r"wasm: x86-64 native code, \d+ bytes \(JIT\)", r"wgfx: frame0=%s frame30=%s" % WGFX,
                 r"wasm: finished with code 0 in \d{1,3} ms"]},                 # порог: < 1000 мс
        {"log_forbid": NO_ERR + [r"wasm: stopped", r"wasm: interpreter mode"]},
    ], smp=2, mem="512M"),
    sc("sound-hda", "звук: Intel HDA (как Sound: hda в Limbo) - кодек, путь к выходу, тон через DMA", [
        {"log": BOOT_LOG + [r"\[KIRA HDA\] Sound: codec .* output path pin \d+ <- DAC \d+"]},
        {"cmd": "sound", "must": [r"выход: вывод \d+ <- ЦАП \d+"]},
        {"cmd": "beep 440 400", "must": [r"DMA прошёл [1-9]\d* байт - карта играет"], "timeout": 15},
        {"log": [r"\[KIRA AUDIO\] Mixer online"]},
        {"cmd": "melody"}, {"sleep": 5},                                   # программа: звук через k_snd_* и микшер ядра
        {"log": [r"melody: 8 notes, 76800 frames queued"]},
        {"cmd": "sound", "must": [r"микшер: работает", r"от программ смешано (7[6-9]|[89]\d)\d\d\d"]},
        {"cmd": "volume 30", "must": [r"громкость: 30%"]},
        {"cmd": "melody 8000 mono"}, {"sleep": 8},                         # 8 кГц моно: ядро пересчитывает в 48 кГц стерео
        {"log": [r"melody: 8 notes, 12800 frames queued at 8000 Hz mono"]},
        {"cmd": "sound", "must": [r"от программ смешано 15[2-4]\d\d\d", r"громкость 30%"]},
        {"log": [r"\[KIRA HDA\] Input: (microphone|line in) pin \d+ -> ADC \d+, recording ring running"]},
        {"cmd": "record 300", "must": [r"запись: (microphone|line in), 14400 кадров за \d+ мс"], "timeout": 10},   # вход QEMU none - тишина
        {"cmd": "rec 500"}, {"sleep": 4},
        {"log": [r"rec: 24000 frames \(48 kHz stereo\), peak \d+, RMS \d+, saved to /data/rec/last\.wav"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-device", "intel-hda", "-device", "hda-duplex,audiodev=a0"]),
    sc("sound-ac97", "звук: Intel AC'97 (как Sound: ac97 в Limbo) - вывод через тот же микшер, тон, программа, запись", [
        {"log": BOOT_LOG + [r"\[KIRA AC97\] Sound: codec 0x[0-9A-F]+, 48 kHz 16-bit stereo output and microphone input"]},
        {"cmd": "sound", "must": [r"AC'97: кодек", r"микшер: работает", r"вход: microphone"]},
        {"cmd": "beep 440 400", "must": [r"DMA прошёл [1-9]\d* байт - карта играет"], "timeout": 15},
        {"cmd": "melody"}, {"sleep": 6},
        {"log": [r"melody: 8 notes, 76800 frames queued"]},
        {"cmd": "sound", "must": [r"от программ смешано (7[6-9]|[89]\d)\d\d\d", r"недоборов 0"]},
        {"cmd": "record 300", "must": [r"запись: microphone, 14400 кадров за \d+ мс"], "timeout": 10},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-device", "AC97,audiodev=a0"]),
    sc("sound-sb16", "звук: Sound Blaster 16 (ISA, как Sound: sb16 в Limbo) - порт, IRQ и DMA от самой карты, 16 бит стерео через 8237 и микшер", [
        {"log": BOOT_LOG + [r"\[KIRA SB16\] Sound: Sound Blaster 16 DSP 4\.\d+ at port 0x[0-9A-F]+ \(DSP reset answered\), IRQ \d+, DMA 8-bit \d+, 16-bit [567] \(from the card's mixer registers 0x80/0x81\)"]},
        {"cmd": "sound", "must": [r"Sound Blaster 16: DSP 4\.\d+, порт 0x[0-9A-F]+, IRQ \d+", r"DMA 8 бит \d+, 16 бит [567]", r"микшер: работает", r"вход: нет"]},
        {"cmd": "beep 440 400", "must": [r"DMA прошёл [1-9]\d* байт - карта играет"], "timeout": 15},
        {"cmd": "melody"}, {"sleep": 6},
        {"log": [r"melody: 8 notes, 76800 frames queued"]},
        {"cmd": "sound", "must": [r"от программ смешано (7[6-9]|[89]\d)\d\d\d", r"недоборов 0", r"карта забрала кадров [1-9]\d{5,}", r"блоков подтверждено [1-9]"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-device", "sb16,audiodev=a0"]),
    sc("net-virtio", "сеть: virtio-net (MSI-X будит поток сети), DHCP, ping, программа fetch", [
        {"log": BOOT_LOG}, {"sleep": 8},
        {"log": [r"\[KIRA NET\] virtio-net: MAC 52:54:00.*MSI-X", r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
        {"cmd": "net", "must": [r"карта virtio-net, канал есть", r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"cmd": "fetch 10.0.2.100 /virtio"}, {"sleep": 3},
        {"log": [r"fetch: 10\.0\.2\.100/virtio - HTTP/1\.0 200 OK - 3\d\d\d bytes .*server closed"]},
        {"cmd": "fetch https://10.0.2.100/tls"}, {"sleep": 10},                            # TLS 1.3 с OpenSSL хозяина
        {"cmd": "fetch https://10.0.2.100:8443/self"}, {"sleep": 6},                     # самоподписанный - отказ
        {"log": [r"fetch: TLS failed: issuer is not trusted"]},
        {"cmd": "fetch https://10.0.2.100:8444/ed"}, {"sleep": 10},                       # цепочка Ed25519 (промежуточный подписан RSA-PSS)
        {"cmd": "fetch https://10.0.2.100:8445/pss"}, {"sleep": 10},                      # ключ id-RSASSA-PSS, подпись корневого RSA-PSS
        {"cmd": "fetch https://10.0.2.100:8446/ocsp"}, {"sleep": 10},                     # прикреплённый ответ OCSP: good
        {"cmd": "fetch https://10.0.2.100:8447/ocsp"}, {"sleep": 8},                      # revoked - отказ
        {"cmd": "fetch -r https://10.0.2.100:8448/resume"}, {"sleep": 16},                # билет сессии: второе соединение по PSK
        {"log": [r"fetch: 10\.0\.2\.100/ed - HTTP/1\.0 200 OK", r"fetch: 10\.0\.2\.100/pss - HTTP/1\.0 200 OK",
                 r"fetch: OCSP: stapled response checked \(signature, certificate, dates\): status good",
                 r"fetch: TLS failed: OCSP: the certificate has been revoked",
                 r"fetch: second connection with the session ticket",
                 r"session resumed from a ticket \(psk_dhe_ke\): no certificates sent"]},
        {"log": [r"fetch: OCSP: no stapled response \(not an error\)"]},
        {"log": [r"fetch: TLS 1\.3, X25519 \+ ChaCha20-Poly1305; certificate verified \(chain to a root, name, dates\)",
                 r"fetch: 10\.0\.2\.100/tls - HTTP/1\.0 200 OK - 2\d\d\d\d bytes .*server closed"]},
        {"cmd": "random", "must": [r"источники - .*virtio-rng", r"засеян аппаратным источником", r"проба: [0-9a-f]{32}"]},
        {"host_http": [18080, "/closed"]},                                                  # порт не слушается: RST сразу
        {"log": [r"\[HOST\] http /closed: error .* in [0-4]\.\d s"]},
        {"cmd": "serve"}, {"sleep": 2}, {"log": [r"serve: listening on port 8080"]},           # Кира - сервер: запрос с хозяина
        {"host_http": [18080, "/hello"]}, {"host_http": [18080, "/again"]}, {"sleep": 4},   # строку о запросе serve пишет после закрытия соединения
        {"log": [r"\[HOST\] http /hello: 19\d\d\d bytes, first line 'kira-server-ok /hello', window lines 900",
                 r"\[HOST\] http /again: 19\d\d\d bytes, first line 'kira-server-ok /again'", r"serve: GET /again -> 200"]},
        {"cmd": "net", "must": [r"TIME_WAIT: [1-9]"]},                                      # serve закрывал первым
        {"send": "udpecho 9000 2"}, {"sleep": 2},                                           # UDP для программ: эхо хозяину
        {"host_udp": [18082, "ping-1"]}, {"host_udp": [18082, "ping-2"]}, {"sleep": 2},
        {"log": [r"\[HOST\] udp reply 'kira-udp: ping-1'", r"\[HOST\] udp reply 'kira-udp: ping-2'", r"udpecho: done, 2 answered"]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE + ",guestfwd=tcp:10.0.2.100:443-cmd:python3 " + HTTPS_ONCE + " --chain " + TLS_CHAIN + ",guestfwd=tcp:10.0.2.100:8443-cmd:python3 " + HTTPS_ONCE + " --self " + TLS_CHAIN + OCSP_FWD + ",hostfwd=tcp:127.0.0.1:18080-:8080,hostfwd=udp:127.0.0.1:18082-:9000",
                                 "-device", "virtio-net-pci,netdev=n0", "-device", "virtio-rng-pci"], tls_chain=True, daemons=[RESUME_DAEMON]),
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
        {"log": [r"\[KIRA CORE \d+\] kernel-mode fault #14 at RIP 0x[0-9A-Fa-f]+, thread 'smpcrash'\. Thread terminated", r"\[KIRA CORE\] fault detail: error 0x[0-9A-Fa-f]+, CR2 0x[0-9A-Fa-f]+, RSP 0x[0-9A-Fa-f]+, RBP 0x[0-9A-Fa-f]+, stack:"]},
        {"cmd": "test crash 2"}, {"sleep": 3},
        {"log": [r"\[KIRA CORE 2\] machine check #18", r"\[KIRA CORE 2\] out of service"]},
        {"responsive": "status", "within": 5},
        {"cmd": "smp", "timeout": 10},                                         # состояние ядер после отказа 2-го
        {"send": "plasma"}, {"sleep": 4}, {"responsive": "status", "within": 5},
        {"log_forbid": [x for x in NO_ERR if x != r"kernel-mode fault"]},
    ], smp=4, mem="512M"),
    sc("net-ipv6", "IPv6 (QEMU user-net ipv6=on, virtio-net): адрес канала + DAD, RA -> SLAAC, ND (NS/NA), ping6, UDP и TCP программой net6; сборка фрагментов IPv4", [
        {"log": BOOT_LOG}, {"sleep": 10},
        {"log": [r"\[KIRA NET\] IPv6: fe80::5054:ff:fe12:3456 \(link-local, DAD passed\) ready",
                 r"\[KIRA NET\] IPv6: router advertisement from fe80::",
                 r"\[KIRA NET\] IPv6: fec0::5054:ff:fe12:3456 \(SLAAC /64, DAD passed\) ready"]},
        {"cmd": "net", "must": [r"IPv6: fec0::5054:ff:fe12:3456/64 \(SLAAC\)", r"IPv6: fe80::5054:ff:fe12:3456/64 \(адрес канала\)",
                                r"IPv6: соседей в кэше [1-9]"]},
        {"cmd": "ping6 fec0::2 3", "must": [r"ответ от fec0::2", r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"cmd": "ping6 fe80::1", "must": [r"отправлено|не отвечает"], "timeout": 20},          # неизвестный сосед: честный отказ, без зависания
        {"host6_serve": [9100, 9101]},
        {"cmd": "net6 fec0::2 9100 9101"}, {"sleep": 6},                                   # программа Ring 3: сокеты семейства 6
        {"log": [r"\[HOST\] ipv6 servers on",
                 r"net6: udp reply 'kira-udp6: hello6' from \[fec0::2\]:9100 family 6",
                 r"net6: tcp peer family 6, \d+ bytes, first line 'HTTP/1\.0 200 OK'"]},
        {"send": "udpecho 9000 2"}, {"sleep": 2},                                          # IPv4: хозяин шлёт 4000 байт, QEMU режет на фрагменты
        {"host_udp": [18082, "small"]},
        {"host_udp_big": [18082, 4000]}, {"sleep": 2},
        {"log": [r"\[HOST\] udp reply 'kira-udp: small'", r"\[HOST\] udp big 4000: reassembled ok", r"udpecho: done, 2 answered"]},
        {"cmd": "net", "must": [r"фрагменты IP: собрано [1-9]\d*"]},
        {"log_forbid": NO_ERR + [r"is already used by another node"]},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,ipv4=on,ipv6=on,hostfwd=udp:127.0.0.1:18082-:9000",
                                 "-device", "virtio-net-pci,netdev=n0"]),
]

# ---- Разум: рычаги 13-30 ----
MELODY_LOOP = [st for _ in range(75) for st in ({"send": "melody"}, {"sleep": 1.5})]      # звук без перерывов ~2 мин: рычагам звука есть что измерять
LEVERS_LIST = [r"13\. вес окна в фокусе: 6 /4 \(обычно 6\)", r"18\. период тактики: 25 мс \(обычно 25\)", r"26\. опережение звука перед картой: 100 мс \(обычно 100\)",
               r"28\. предел одного запроса к диску: 256 блоков", r"30\. окно предела доли процессора: 50 тиков"]
ALL.append(sc("mind-levers", "Разум: 18 рычагов (13-30) в `ai levers`; звук без перерыва - рычаг опережения пробуется, оставляется или откатывается по замеру; натиск программ; опыт записывается", [
    {"log": BOOT_LOG},
    {"cmd": "ai levers", "must": LEVERS_LIST, "timeout": 10},
    *MELODY_LOOP,
    {"log": [r"\[KIRA MIND\] рычаг 26 «опережение звука перед картой»: пробую \d+ мс вместо 100 \(причина: ",
             r"\[KIRA MIND\] рычаг 26 «опережение звука перед картой»: (оставлено|откат на|срочный откат) "]},
    {"cmd": "ai levers", "must": [r"26\. опережение звука перед картой: \d+ мс \(обычно 100\) - [^\n]*проб [1-9]", r"смен значений за запуск [1-9]"], "timeout": 10},
    *[{"send": "plasma"} for _ in range(8)],
    {"sleep": 40}, {"shot": "levers-load"},
    {"responsive": "status", "within": 5},
    {"cmd": "ai why", "must": [r"."], "timeout": 10},
    {"cmd": "ai save", "must": [r"опыт записан"], "timeout": 10},
    {"cmd": "ls /var", "must": [r"mind\.dat"]},
    {"log_forbid": NO_ERR},
], smp=4, mem="2G", extra=["-device", "intel-hda", "-device", "hda-duplex,audiodev=a0"]))

# Семя опыта: файл /etc/mind_seed.bin (здесь - испытательный образец tools/mindseed --fixture, не результат обучения) читается ядром,
# когда своего опыта на диске нет: веса предвидения и подсказки берутся, нормы устройства - нет, доверие ограничено
ALL.append(sc("mind-seed", "Разум: при первой загрузке без своего опыта ядро берёт семя (веса предвидения со сниженным числом наблюдений и подсказки по рычагам), нормы машины выучивает само", [
    {"log": BOOT_LOG}, {"sleep": 4},
    {"log": [r"\[KIRA MIND\] нет своего опыта: взято семя \(из QEMU, на железе лишь подсказка\), веса на 90 с"]},
    {"cmd": "ai model", "must": [r"фоновые программы: -?\d\.\d\d", r"обучено на (9\d|1[0-4]\d) с"], "timeout": 10},
    {"cmd": "ai", "must": [r"пока без права действовать"], "timeout": 10},
    {"cmd": "ai levers", "must": [r"стартовые подсказки взяты из семени опыта"], "timeout": 10},
    {"log_forbid": NO_ERR},
], smp=2, mem="512M", put={"/etc/mind_seed.bin": "ci-out/mind_seed_fixture.bin"}))

# Тренажёр (долгий, ОТДЕЛЬНЫЙ job workflow mind-train.yml, не в основной матрице): разные нагрузки подряд; в конце опыт
# записывается на диск образа, машина выключается, файл /var/mind.dat достаётся kfstool и становится артефактом.
def _train_phases():
    st = []
    for rnd in range(3):
        st += [{"send": "plasma"} for _ in range(10)]                         # спам программ
        st += [{"sleep": 60}]
        st += [{"cmd": "cp /bin/fetch /t%d_a" % rnd, "timeout": 60}, {"cmd": "cp /bin/wasm /t%d_b" % rnd, "timeout": 60},
               {"cmd": "cp /bin/cube3d /t%d_c" % rnd, "timeout": 60}]          # диск
        st += [{"cmd": "cp /t%d_a /t%d_a2" % (rnd, rnd), "timeout": 60}, {"cmd": "cp /t%d_b /t%d_b2" % (rnd, rnd), "timeout": 60}]
        st += [{"send": "fetch 10.0.2.100 /train"} for _ in range(4)]          # сеть
        st += [{"sleep": 20}]
        st += [{"send": "3d"}] + [{"send": "plasma"} for _ in range(3)] + [{"sleep": 60}]     # 3D с фоном
        st += [st_ for _ in range(40) for st_ in ({"send": "melody"}, {"sleep": 1.5})]        # звук
        st += [{"send": "burnall"}, {"sleep": 45}]                           # все ядра заняты
        st += [{"sleep": 30}]                                                # покой: нормы обновляются и без нагрузки
    return st
ALL.append(sc("mind-train", "Тренажёр Разума (долгий, ~25 мин): спам программ, диск, сеть, звук, 3D по кругу; опыт - в образ диска, затем workflow mind-train забирает /var/mind.dat", [
    {"log": BOOT_LOG},
    *_train_phases(),
    {"cmd": "ai", "must": [r"предвидение: риск опоздания экрана"], "timeout": 20},
    {"cmd": "ai levers", "must": [r"13\. вес окна в фокусе"], "timeout": 20},
    {"cmd": "ai save", "must": [r"опыт записан"], "timeout": 20},
    {"sleep": 3},
    {"exit": "poweroff", "within": 30},
    {"log_forbid": NO_ERR},
], smp=4, mem="2G", boot_timeout=120, extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE, "-device", "virtio-net-pci,netdev=n0",
                                        "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0"]))

# Тот же натиск на другом числе ядер: плавающий сбой при старте программ (kernel-mode fault в plasma) ловится чаще
import copy as _copy
for _n in (2, 4):
    _d = _copy.deepcopy(next(x for x in ALL if x["name"] == "stress-spam"))
    _d["name"] = "stress-spam-%d" % _n; _d["smp"] = _n
    ALL.append(_d)

# kcc на Кире (дорожка M3, x86-64): компилятор /bin/kcc (Ring 3, только CAP_FS) собирает программы из /data/kcc, runlocal запускает их без прав.
_CC = os.path.join("tests", "kcc", "native")
ALL.append(sc("cc-native", "kcc на Кире: cc собирает C-программу (функции, структуры, строки, цикл, вывод через kira.h), runlocal запускает без прав; ошибка синтаксиса с номером строки; отказ сокета KE_PERM", [
    {"log": BOOT_LOG},
    {"cmd": "ls /etc/kcc/include", "must": [r"kira\.h", r"stdio\.h", r"stdlib\.h", r"string\.h"]},
    {"cmd": "cc hello.c", "must": [r"использование: cc исходник результат"]},
    {"send": "cc hello.c hello"}, {"sleep": 10},
    {"log": [r"kcc: ok: hello\.c -> hello, \d+ байт"]},
    {"cmd": "ls /data/kcc", "must": [r"hello\.c", r"\d+\s+hello\s"]},
    {"send": "runlocal hello"}, {"sleep": 6},
    {"log": [r"\[LOCAL\] /data/kcc/hello запускается по явной команде пользователя как 'local-hello' без прав",
             r"native: hello from kcc on Kira", r"native: total=240", r"native: fib20=6765", r"native: sorted gear bolt nut",
             r"native: reversed=so-arik", r"native: hex=c0ffee", r"native: clock ok", r"native: done"]},
    # синтаксическая ошибка: сообщение с именем файла и номером строки, результат не создаётся
    {"send": "cc bad.c bad"}, {"sleep": 8},
    {"log": [r"bad\.c:9: error: "]},
    {"send": "runlocal bad"}, {"sleep": 3},
    {"log": [r"\[LOCAL\] /data/kcc/bad: "]},
    # программа без прав: сеть и файлы отказывают
    {"send": "cc net.c net"}, {"sleep": 10},
    {"log": [r"kcc: ok: net\.c -> net, \d+ байт"]},
    {"send": "runlocal net"}, {"sleep": 6},
    {"log": [r"net: dns=-12 connect=-12 file=-100", r"net: refused \(PASS\)", r"\[KIRA CAPS\] process 'local-net' .* denied, no right CAP_NET"]},
    # расширения языка: атомарные операции, struct по значению, va_list, case-диапазоны, ?:, printf
    {"send": "cc lang.c lang"}, {"sleep": 12},
    {"log": [r"kcc: ok: lang\.c -> lang, \d+ байт"]},
    {"send": "runlocal lang"}, {"sleep": 6},
    {"log": [r"atomic: counter=9 ok1=1 ok2=0 seen=100 xchg=100 sync=42", r"struct: 526 42 -5", r"varargs: 60 45", r"case: zero digit two-digit big",
             r"elvis: 11 6", r"fmt: \[   42\]\[ab   \]\[000ff\]\[k\]"]},
    # компилятору дано только CAP_FS: он ни разу не получил отказ; имя с путём не принимается
    {"log_forbid": NO_ERR + [r"\[KIRA CAPS\] process 'kcc'", r"NOT refused", r"native: .*BAD", r"native: malloc failed", r"\[IMMUNE\] /data/kcc/.*отклонён"]},
    {"cmd": "runlocal ../bin/plasma", "must": [r"\[LOCAL\] недопустимое имя файла"]},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", put={"/data/kcc/hello.c": os.path.join(_CC, "hello.c"), "/data/kcc/bad.c": os.path.join(_CC, "bad.c"),
                          "/data/kcc/net.c": os.path.join(_CC, "net.c"),
                          "/data/kcc/lang.c": os.path.join(_CC, "lang.c")}))

# Драйверы Ring 3: пакет edu намеренно MMIO-only до появления проверенной IOMMU-изоляции.
# Тест проверяет безопасный запуск, отказ DMA/MSI, запрет чужого BAR и восстановление устройства после остановки/сбоя.
ALL.append(sc("drv-edu", "MMIO-only Ring 3: хеш и подтверждение пакета; безопасный запуск; DMA/MSI fail-closed; MMIO и PCI config guard; освобождение ресурсов после остановки и сбоя", [
    {"log": BOOT_LOG},
    {"sleep": 6},
    {"log": [r"\[KIRA DRV\] abad: SHA-256 не совпал с записью /drv/abad\.rec - драйвер НЕ запущен",
             r"\[KIRA DRV\] eduun: пакет отклонён \(установка не подтверждена пользователем\)",
             r"\[KIRA DRV\] edu: устройство .* 1234:11e8 -> драйвер запущен как drv-edu \(pid \d+\), SHA-256 сверен, права: CAP_MMIO CAP_PCIDEV",
             r"drvedu: device 0x1234:0x11e8, rights 0x4800, no MSI",
             r"drvedu: id=0x10000ed", r"drvedu: 10!=3628800",
             r"drvedu: IRQ/MSI capability not granted \(least-privilege PASS\)",
             r"drvedu: dma rc=-12", r"drvedu: DMA denied: CAP_DMA not granted \(least-privilege PASS\)",
             r"drvedu: refused: other-bar=-2 past-bar=-10 bar-move=-12 bad-cmd=-12 cfg-id=0x",
             r"drvedu: PASS"]},
    {"cmd": "drv", "must": [r"drv-edu pid \d+", r"отображений 1", r"прерываний 0"]},
    {"cmd": "drv stop edu", "must": [r"останавливаю drv-edu"]}, {"sleep": 2},
    {"log": [r"\[KIRA DRV\] drv-edu \(pid \d+\) завершён: устройство .* остановлено",
             r"\[KIRA DRV\] edu: возвращено памяти DMA и колец 0 из 0 стр\..*утечек нет"]},
    {"cmd": "drv start edunc", "must": [r"драйвер запущен как drv-edunc"]}, {"sleep": 3},
    {"log": [r"drvedu: dma rc=-12", r"\[KIRA CAPS\] process 'drv-edunc' .* denied, no right CAP_DMA"]},
    {"cmd": "drv stop edunc"}, {"sleep": 2},
    {"cmd": "drv start edu crash", "must": [r"драйвер запущен как drv-edu"]}, {"sleep": 3},
    {"log": [r"drvedu: crashing on purpose", r"\[KIRA PROC\] pid \d+ killed by CPU exception #14", r"\[KIRA DRV\] drv-edu \(pid \d+\) завершён"]},
    {"sleep": 2},
    {"cmd": "drv start edu", "must": [r"драйвер запущен как drv-edu"]}, {"sleep": 3},
    {"log_forbid": [r"drvedu: .*\(FAIL\)", r"УТЕЧКА"] + NO_ERR},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", extra=["-device", "edu"]))


# drvget + драйвер RTL8139 (пакет L, N). Фаза 1: сеть virtio; drvget по настоящему TLS 1.3 скачивает исходники драйвера rl с хозяина,
# показывает источник/лицензию/SHA-256/права, ставит только после подтверждения хешем, собирает kcc на устройстве и пишет /drv.
# Фаза 2 (relaunch на том же диске): вместо virtio-net - rtl8139; драйвер из /drv находит карту по PCI-ID и регистрирует сетевой интерфейс.
def _drv_www():
    import hashlib, shutil, struct, re
    w = os.path.abspath("ci-out/drvwww")
    shutil.rmtree(w, ignore_errors=True)
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "drivers", "rl")
    def clone(name):
        os.makedirs(os.path.join(w, name))
        for f in ("drvget.rec", "rl.c", "if_rlreg.h"):
            shutil.copy(os.path.join(src, f), os.path.join(w, name, f))
    def edit(name, f, fn):
        pth = os.path.join(w, name, f)
        b = open(pth, "rb").read(); open(pth, "wb").write(fn(b))
    clone("rl")
    clone("gpl"); edit("gpl", "drvget.rec", lambda b: b.replace(b"\nname rl", b"\nname gpl"))
    edit("gpl", "if_rlreg.h", lambda b: b.replace(b"SPDX-License-Identifier: BSD-3-Clause", b"SPDX-License-Identifier: GPL-2.0-only"))
    # Keep the fixture internally valid: the source hash must match the deliberately GPL-marked file,
    # otherwise drvget correctly rejects the stale SHA-256 before reaching its licence scanner.
    gpl_hash = hashlib.sha256(open(os.path.join(w, "gpl", "if_rlreg.h"), "rb").read()).hexdigest().encode()
    edit("gpl", "drvget.rec", lambda b: re.sub(rb"(file if_rlreg[.]h if_rlreg[.]h sha256 )[0-9a-f]{64}", lambda m: m.group(1) + gpl_hash, b))
    clone("bin"); edit("bin", "drvget.rec", lambda b: b.replace(b"\nname rl", b"\nname bin"))
    edit("bin", "if_rlreg.h", lambda b: b"MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00This program cannot be run in DOS mode.")
    clone("nolic"); edit("nolic", "drvget.rec", lambda b: b.replace(b"\nname rl", b"\nname nolic"))
    edit("nolic", "if_rlreg.h", lambda b: b"/* no licence information */\n" + b"\n".join(b.split(b"\n")[40:]))
    # Keep the no-licence fixture internally valid too: exercise the licence scanner,
    # not the earlier SHA-256 integrity gate.
    nolic_hash = hashlib.sha256(open(os.path.join(w, "nolic", "if_rlreg.h"), "rb").read()).hexdigest().encode()
    edit("nolic", "drvget.rec", lambda b: re.sub(rb"(file if_rlreg[.]h if_rlreg[.]h sha256 )[0-9a-f]{64}", lambda m: m.group(1) + nolic_hash, b))
    h = hashlib.sha256()
    for n in ("drvget.rec", "rl.c", "if_rlreg.h"):          # так же, как dg_set_add в userland/drvget_core.c
        d = open(os.path.join(w, "rl", n), "rb").read()
        h.update(n.encode() + b"\0" + struct.pack("<Q", len(d)) + d)
    return w, h.hexdigest()[:8]

def _dg(cmd, rx, t):     # программа запускается в фоне: команда без ожидания приглашения, затем ожидание строки в журнале после неё
    return [{"sleep": 2}, {"send": cmd}, {"expect": rx, "timeout": t}]   # пауза: Разум не допускает больше 3 запусков программ в секунду

_DRV_WWW, _DRV_SET8 = _drv_www()
_DRV_HTTP = "guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE
ALL.append(sc("drvget-rl", "drvget: исходники RTL8139 по HTTPS, аудит лицензии и хешей, сборка KCC и установка; при отсутствии IOMMU ядро обязано отказать в запуске DMA-драйвера", [
    {"log": BOOT_LOG}, {"sleep": 6},
    *_dg("drvget base https://10.0.2.100", r"каталог рецептов теперь https://10\.0\.2\.100", 20),
    *_dg("drvget install gpl", r"drvget: отказ: .*GPL", 90),
    *_dg("drvget install bin", r"drvget: отказ: SHA-256 файла if_rlreg[.]h", 90),
    *_dg("drvget install nolic", r"drvget: отказ: if_rlreg[.]h: лицензия файла не найдена", 90),
    *_dg("drvget install http://10.0.2.100/rl/drvget.rec", r"разрешён только https", 20),
    {"cmd": "drv", "forbid": [r"drv-rl"]},
    *_dg("drvget install rl", r"drvget confirm rl " + _DRV_SET8, 120),
    {"log": [r"лицензия:   заявлена BSD-3-Clause", r"if_rlreg\.h", r"10ec:8139", r"CAP_DMA", r"CAP_NETDEV", r"IOMMU", r"Установка НЕ выполнена"]},
    *_dg("drvget confirm rl 00000000", r"хеш не совпал", 20),
    *_dg("drvget confirm rl " + _DRV_SET8, r"установлено: /drv/rl\.kdrv и /drv/rl\.rec", 240),
    {"log": [r"собрано, образ ELF \d+ байт"]},
    *_dg("drvget list", r"rl: версия 1, права: MMIO DMA PCIDEV NETDEV, SHA-256 сверен, издатель: (Ed25519 подтверждён|не подтверждён \\(локально\\)), источник: https://10\\.0\\.2\\.100/rl/drvget\\.rec", 30),
    *_dg("drvget verify rl", r"verify rl: OK", 20),
    {"log_forbid": NO_ERR},
    {"relaunch": ["-netdev", "user,id=n0," + _DRV_HTTP, "-device", "rtl8139,netdev=n0"]},
    {"sleep": 8},
    {"log": [r"\[KIRA DRV\] rl: DMA/MSI запрещены — изоляция IOMMU для устройства не настроена; драйвер НЕ запущен"]},
    {"log_forbid": [r"rl: network interface registered", r"rl: first frame received", r"rl: first frame transmitted"]},
    {"cmd": "drv", "forbid": [r"drv-rl pid"]},
    {"log_forbid": NO_ERR + [r"УТЕЧКА"]},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", tls_chain=True,
    extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:443-cmd:python3 " + HTTPS_ONCE + " --chain " + TLS_CHAIN + " --serve " + _DRV_WWW,
           "-device", "virtio-net-pci,netdev=n0"]))


# Окно «Драйверы» (/bin/drvman, пакет DM). Ядро чистое: окно - программа Ring 3; список PCI читается одним вызовом ядра под правом CAP_PCILIST,
# установка - тот же код, что у drvget (userland/drvget.c с -DDRVGET_LIB). Режим «drvman do КОМАНДА;...» выполняет теми же функциями то,
# что делают кнопки, и печатает те же строки, что рисуются в окне (устройства, пакеты, кадр), поэтому стенд проверяет именно показанное.
ALL.append(sc("drvman-edu", "окно «Драйверы»: список PCI из ядра (встроенный драйвер / Ring 3 запущен / драйвера нет), пакеты /drv и итог сверки (испорченный, неподтверждённый), остановка и запуск драйвера edu через менеджер ядра, отказ запуска без свободного устройства, удаление", [
    {"log": BOOT_LOG}, {"sleep": 6},
    {"log": [r"\[KIRA DRV\] edu: устройство .* 1234:11e8 -> драйвер запущен как drv-edu", r"drvedu: DMA denied: CAP_DMA not granted \(least-privilege PASS\)", r"drvedu: IRQ/MSI capability not granted \(least-privilege PASS\)", r"drvedu: PASS"]},
    *_dg("drvman do pci;pkgs", r"drvman: готово", 40),
    {"log": [r"drvman: окно создано 640x560",
             r"drvman: кадр: строк в списках [1-9]\d*, устройств [1-9]\d*, пакетов 4",
             r"drvman: pci \d\d:\d\d\.\d\s+1234:11e8\s+[0-9a-f]{6}\s+.*драйвер Ring 3 drv-edu запущен",
             r"drvman: pci \d\d:\d\d\.\d\s+[0-9a-f]{4}:[0-9a-f]{4}\s+[0-9a-f]{6}\s+.*встроенный драйвер ядра",
             r"drvman: pci \d\d:\d\d\.\d\s+[0-9a-f]{4}:[0-9a-f]{4}\s+060000\s+.*мост или шина",
             r"drvman: пакетов в /drv: 4",
             r"drvman: пакет edu \| edu\s+v\d+\s+лицензия: не записана\s+ЗАПУЩЕН\s+сверка: SHA-256 OK, локально",
             r"drvman: пакет edu \| права запрошены: MMIO PCIDEV\s+подтверждены: MMIO PCIDEV",
             r"drvman: пакет edu \| sha256 образа [0-9a-f]{64}",
             r"drvman: пакет edu \| источник: tools/mkdrv\.sh",
             r"drvman: пакет abad \| abad\s+v\d+\s+лицензия: не записана\s+не запущен\s+сверка: НЕ СОВПАДАЕТ",
             r"drvman: пакет eduun \| eduun\s+v\d+\s+лицензия: не записана\s+не запущен\s+сверка: не подтверждён",
             r"drvman: пакет edunc \| edunc\s+v\d+\s+лицензия: не записана\s+не запущен\s+сверка: SHA-256 OK, локально"]},
    # остановка через менеджер драйверов ядра (FS_OP_DRV_STOP -> очередь kdrv -> та же команда, что «drv stop»); устройство после этого - «пакет установлен, не запущен»
    *_dg("drvman do stop edu;pci", r"drvman: готово", 40),
    {"log": [r"drvman: просьба остановить edu принята", r"\[KIRA DRV\] останавливаю drv-edu", r"\[KIRA DRV\] drv-edu \(pid \d+\) завершён: устройство .* остановлено",
             r"drvman: драйвер edu остановлен, устройство возвращено ядру",
             r"drvman: pci \d\d:\d\d\.\d\s+1234:11e8\s+[0-9a-f]{6}\s+.*пакет edu установлен, не запущен"]},
    {"cmd": "drv", "forbid": [r"drv-edu pid"]},
    *_dg("drvman do start edu;pci", r"drvman: готово", 60),
    {"log": [r"drvman: просьба запустить edu принята", r"drvman: драйвер edu запущен на устройстве \d\d:\d\d\.\d \(1234:11e8\)",
             r"\[KIRA DRV\] edu: устройство .* 1234:11e8 -> драйвер запущен как drv-edu"]},
    {"cmd": "drv", "must": [r"drv-edu pid \d+"]},
    # честные отказы: уже запущен; удалить работающий нельзя; запуск другого пакета на занятое устройство ядро не выполняет
    *_dg("drvman do start edu;remove edu;start edunc", r"drvman: готово", 60),
    {"log": [r"drvman: драйвер edu уже запущен на устройстве", r"drvman: драйвер edu сейчас работает: сначала «Остановить»",
             r"drvman: драйвер edunc не запущен: нет свободного подходящего устройства PCI"]},
    # остановить и удалить; пакет исчезает из /drv и из списка
    *_dg("drvman do stop edu;remove edu;pkgs", r"drvman: готово", 60),
    {"log": [r"drvman: пакет edu удалён", r"drvman: пакетов в /drv: 3"]},
    {"cmd": "drv", "forbid": [r"\n  edu: версия"]},
    *_dg("drvman do frobnicate", r"drvman: готово с ошибками", 30),
    {"log": [r"drvman: неизвестная команда «frobnicate»"]},
    {"log_forbid": NO_ERR + [r"УТЕЧКА", r"\[KIRA CAPS\] process 'drvman'", r"drvman: .*\(FAIL\)"]},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", extra=["-device", "edu"]))

# Окно «Драйверы» целиком, с клавиатуры, как у пользователя: имя в поле + Enter (HTTPS к хозяину, отчёт), F2 (окно подтверждения), Esc (отмена - ничего не записано),
# F2 и Enter (сборка kcc на устройстве, запись в /drv), F4 (запуск без подходящего устройства - честный отказ), F3 (удаление). Снимки окна - в артефактах стенда.
_DM_BASE = os.path.abspath("ci-out/drvman-base.txt")
os.makedirs(os.path.dirname(_DM_BASE), exist_ok=True)
open(_DM_BASE, "w").write("https://10.0.2.100")
ALL.append(sc("apps-drvman", "окно «Драйверы» с клавиатуры: поиск драйвера rl по HTTPS (отчёт: источник, лицензия, хеш, права, IOMMU), подтверждение и отмена, установка со сборкой kcc, пакет виден drvget, запуск без устройства - отказ, удаление", [
    {"log": BOOT_LOG}, {"sleep": 6},
    {"cmd": "run drvman trace"}, {"sleep": 4}, {"shot": "drvman-list"},
    {"log": [r"drvman: окно создано 640x560", r"drvman: кадр: строк в списках [1-9]\d*, устройств [1-9]\d*, пакетов 4"]},
    {"send": ""}, {"keys": "rl", "then": ["ret"]},
    {"expect": r"drvman: набор готов к установке", "timeout": 200},
    {"log": [r"drvman: ищу драйвер «rl»", r"лицензия:   заявлена BSD-3-Clause", r"SHA-256 набора исходников \(рецепт \+ файлы\): " + _DRV_SET8,
             r"10ec:8139", r"CAP_DMA", r"IOMMU", r"Установка НЕ выполнена"]},
    {"shot": "drvman-found"},
    {"keys": "", "then": ["f2"]}, {"sleep": 2},
    {"log": [r"drvman: подтверждение: установить rl v1\?", r"drvman: подтверждение: источник https://10\.0\.2\.100/rl/drvget\.rec",
             r"drvman: подтверждение: лицензия BSD-3-Clause, права: MMIO DMA PCIDEV NETDEV",
             r"drvman: подтверждение: SHA-256 набора " + _DRV_SET8 + r"[0-9a-f]{56}",
             r"drvman: подтверждение: DMA/MSI будут отклонены ядром: домен IOMMU для устройства не настроен"]},
    {"shot": "drvman-confirm"},
    {"keys": "", "then": ["esc"]}, {"sleep": 2},
    {"log": [r"drvman: установка отменена пользователем, ничего не записано"]},
    *_dg("drvget list", r"drvget: пакетов в /drv: 4 ", 30),
    {"cmd": "drv", "forbid": [r"\n  rl: версия"]},
    {"send": ""}, {"keys": "", "then": ["f2"]}, {"sleep": 2}, {"keys": "", "then": ["ret"]},
    {"expect": r"drvman: драйвер rl установлен в /drv", "timeout": 400},
    {"log": [r"drvman: подтверждено пользователем, собираю и ставлю", r"drvget: собрано, образ ELF \d+ байт"]},
    {"shot": "drvman-installed"},
    *_dg("drvget list", r"rl: версия 1, права: MMIO DMA PCIDEV NETDEV, SHA-256 сверен, источник: https://10\.0\.2\.100/rl/drvget\.rec", 30),
    *_dg("drvget verify rl", r"verify rl: OK", 30),
    {"cmd": "drv", "must": [r"rl: версия 1, SHA-256 сверен, запрошено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV, подтверждено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV"]},
    {"send": ""}, {"keys": "", "then": ["f4"]},
    {"expect": r"drvman: драйвер rl не запущен: нет свободного подходящего устройства PCI", "timeout": 40},
    {"shot": "drvman-start-refused"},
    {"send": ""}, {"keys": "", "then": ["f3"]},
    {"expect": r"drvman: пакет rl удалён", "timeout": 40},
    {"shot": "drvman-removed"},
    {"cmd": "drv", "forbid": [r"\n  rl: версия"]},
    *_dg("drvget list", r"drvget: пакетов в /drv: 4 ", 30),
    {"log_forbid": NO_ERR + [r"\[KIRA CAPS\] process 'drvman'", r"drvman: .*\(FAIL\)", r"drvman: не удалось создать рабочий поток"]},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", tls_chain=True, put={"/data/drvman/base": _DM_BASE},
    extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:443-cmd:python3 " + HTTPS_ONCE + " --chain " + TLS_CHAIN + " --serve " + _DRV_WWW,
           "-device", "virtio-net-pci,netdev=n0"]))


# Обычный графический запуск не должен копировать журнал окна в пользовательскую консоль.
# Живой процесс подтверждает успешное создание окна: без окна drvman завершается сразу.
ALL.append(sc("drvman-quiet", "тихий запуск графического менеджера: окно живо, терминал не загрязняется журналом приложения", [
    {"log": BOOT_LOG}, {"sleep": 5},
    {"cmd": "run drvman"}, {"sleep": 3}, {"shot": "drvman-quiet"},
    {"cmd": "ps", "must": [r"drvman"]},
    {"log_forbid": [r"drvman: окно создано", r"drvman: кадр:", r"drvman: ищу драйвер"] + NO_ERR},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G"))
