"""Сценарии стенда. Шаги:
  {"cmd": "команда", "must": [regex...], "forbid": [regex...], "timeout": с}  - выполнить и проверить вывод
  {"send": "команда"}                    - отправить, не дожидаясь приглашения (запуск программ под нагрузкой)
  {"sleep": с}                           - подождать (журнал при этом читается и проверяется на аварии)
  {"shot": "имя"}                        - снимок экрана
  {"log": [regex...]}, {"log_forbid": [regex...]} - проверки всего журнала с начала
  {"responsive": "команда", "within": с} - система отвечает на команду не дольше заданного (стабильность под нагрузкой)
Сроки - для KVM; без KVM стенд умножает их (--timeout-scale)."""
import os, sys, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfiles import SUMS

FLASH = "ci-out/flash.img"          # создаёт ci/make_flash.sh (FAT32 + файл hello.txt)

BOOT_LOG = [r"\[KIRA SCHED\] Preemptive scheduler online", r"\[KIRA MIND\] Mind online", r"\[KIRA CONSOLE\] Command line ready",
            r"\[KIRA FS\] KiraFS mounted"]
NO_ERR = [r"kernel-mode fault", r"\[KIRA IRET\]", r"\[KIRA GS\]", r"\[KIRA VMM ERROR\]", r"Kernel thread stack overflow", r"bad free|double free", r"\[KIRA SMP\].*did not answer"]

# Пакетов драйверов на диске сборки: abad, eduun, edu, edunc, rsedu, ne2kio, vblk, vblknc + bsdne, bsdpcn, bsdfxp, bsddc (build_clang.sh). Менять вместе со списком пакетов.
NPKG = 12

BASIC = [
    {"log": BOOT_LOG},
    {"cmd": "test mem", "must": [r"heap: PASS", r"PMM: PASS", r"VMM: PASS"]},
    {"cmd": "disk", "must": [r"\[системный\]"]},
    {"cmd": "df", "must": [r"KiraFS"]},
    {"cmd": "verify", "must": [r"\[KIRA SEAL\] \d+ файлов в печати", r"образ цел"], "forbid": [r"ВНИМАНИЕ", r"НЕ СХОДИТСЯ"]},
    {"cmd": "status", "must": [r"."]},
    {"cmd": "ls /bin", "must": [r"plasma", r"cube3d"]},
    {"log_forbid": NO_ERR},
]


# Нагрузка сценария latency (А8): burn на всех ядрах, сеть, диск. Первые три шага - загрузка, замер секций, сброс гистограмм.
# -icount: виртуальное время идёт по числу выполненных команд (1 нс на команду), а не по часам хозяина: простои виртуальных ядер из-за
# хозяина (на стенде 2 настоящих ядра, чужие сборки) не попадают в измерение, и границы можно держать узкими. На настоящем железе - иначе.
LATENCY_EXTRA = ["-icount", "shift=0,sleep=on"]
LATENCY_LOAD = [
    {"log": BOOT_LOG + [r"\[KIRA LAT\] latency histograms online"]},
    {"cmd": "latency irqoff on", "must": [r"замер секций с выключенными прерываниями: включён"]},
    {"cmd": "latency reset", "must": [r"гистограммы и счётчики превышений сброшены"]},
    {"send": "burnall"},
    *[{"cmd": "ping 10.0.2.2 3", "must": [r"получено [1-9]"], "timeout": 90} for _ in range(4)],
    *[{"cmd": "write /lat%d.txt kira-latency-load-%d" % (i, i), "timeout": 60} for i in range(12)],
    *[{"cmd": "cat /lat%d.txt" % i, "must": [r"kira-latency-load-%d" % i], "timeout": 60} for i in range(0, 12, 3)],
    {"send": "burnall"}, {"sleep": 4},
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

# KASLR: одна загрузка - проверки (в режиме отладки база ядра печатается командой kaslr, без него скрыта); tag - метка файла этой загрузки
def _kaslr_boot(tag):
    return [
        {"log": BOOT_LOG + [r"\[KIRA KASLR\] on: kernel image moved to a random 2 MB slot \(1 of \d+ free slots, ~\d+ bits\), \d+ words relocated; entropy: ",
                           r"\[KIRA KASLR\] kernel base hidden by the kptr policy", r"\[KIRA VMM\] W\^X early: ok"]},
        {"cmd": "kptr", "must": [r"режим отладки: выключен", r"пример: K\+0x[0-9a-f]+"], "forbid": [r"пример: 0x"]},
        {"cmd": "kaslr", "must": [r"включён: образ ядра перемещён в случайную 2-МБ позицию", r"адрес базы скрыт политикой kptr"], "forbid": [r"kernel base 0x"]},
        {"cmd": "kptr debug on", "must": [r"режим отладки: включён", r"пример: 0x[0-9a-f]+"]},
        {"cmd": "kaslr", "must": [r"\[KIRA KASLR\] kernel base (0x[0-9A-Fa-f]+) \(link base 0x100000, slide 0x[0-9A-Fa-f]+\)"]},
        {"cmd": "kptr debug off", "must": [r"режим отладки: выключен"]},
        {"sleep": 6},
        {"cmd": "net", "must": [r"канал есть", r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"cmd": "write /kaslr-%s.txt kaslr-ok-%s" % (tag, tag)},
        {"cmd": "cat /kaslr-%s.txt" % tag, "must": [r"kaslr-ok-%s" % tag]},
        {"cmd": "status", "must": [r"."]},
        {"cmd": "wx", "must": [r"итог: ok"]},
    ]


# Сон S3: до сна - сеть (если есть), файл, часы; QMP system_wakeup будит машину; после - всё то же самое плюс новая запись, часы идут, ядра на месте.
# before/after - проверки устройств, которые есть только у этого сценария (звук, USB): выполняются до сна и после пробуждения.
def s3_steps(before=(), after=(), net=True, boot=()):
    return [
        {"log": BOOT_LOG + [r"\[KIRA ACPI\] AML up"] + list(boot)},
        {"sleep": 8},
    ] + ([{"cmd": "net", "must": [r"адрес 10\.0\.2\.15"]},
          {"cmd": "ping 10.0.2.2 2", "must": [r"отправлено 2, получено [1-2]"], "timeout": 20}] if net else []) + list(before) + [
        {"cmd": "write /s3-before.txt kira-before-sleep"},
        {"cmd": "sleep status", "must": [r"сон S3: поддерживается", r"хуки устройств"]},
        {"send": "status"}, {"sleep": 1}, {"capture": {"t0": r"precise clock ms: (\d+)"}},
        {"send": "sleep"},
        {"qmp": "system_wakeup", "wait_status": "suspended", "after_status": "running", "within": 60},
        {"expect": r"\[KIRA SLEEP\] awake: devices restored, 0 failed to come back", "timeout": 60},
        {"sleep": 3},
    ] + ([{"cmd": "net", "must": [r"адрес 10\.0\.2\.15"]},
          {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 30}] if net else []) + list(after) + [
        {"cmd": "cat /s3-before.txt", "must": [r"kira-before-sleep"]},
        {"cmd": "write /s3-after.txt kira-after-sleep"},
        {"cmd": "cat /s3-after.txt", "must": [r"kira-after-sleep"]},
        {"send": "status"}, {"sleep": 1}, {"capture": {"t1": r"precise clock ms: (\d+)"}},
        {"num": ["t1", "t0", 1, 600000]},
        {"cmd": "smp", "must": [r"(?s)(  online  .*){4}"], "forbid": [r"FAILED", r"offline"], "timeout": 10},
        {"cmd": "sleep status", "must": [r"сеансов сна: 1, отказов: 0, устройств не вернулось: 0"]},
        {"log_forbid": NO_ERR + [r"did not come back", r"power button pressed", r"НЕ ВОССТАНОВИЛОСЬ", r"did not answer", r"NOT isolated now"]},
    ]


S3_STEPS = s3_steps()
S3_Q35 = ["-global", "ICH9-LPC.disable_s3=0", "-global", "ICH9-LPC.disable_s4=1"]
S3_BEEP = {"cmd": "beep 440 400", "must": [r"DMA прошёл [1-9]\d* байт - карта играет"], "timeout": 15}
S3_USB = {"cmd": "cat /usb0/hello.txt", "must": [r"kira-ci"], "timeout": 30}

ALL = [
    sc("boot-grub", "запасной образ kira-grub.iso (GRUB): загрузка и базовые проверки", BASIC + [
        {"log": [r"Processors online: 4"]},
        *DISK_IRQ,
    ], boot="grub"),
    sc("boot-bios", "загрузка нашим BIOS-загрузчиком без GRUB: VBE, E820 и системный диск", BASIC + [
        # The release image is built with initrd.img; the MB2 handoff must report one module.
        {"log": [r"\[KIRA FB\] Bootloader mode: 1280x720x32", r"Processors online: 4", r"modules=1"]},
        *DISK_IRQ,
    ], boot="bios", needs=["initrd"]),
    sc("boot-virtio", "загрузка: virtio-blk, 4 ядра, базовые проверки", BASIC + [
        {"log": [r"Processors online: 4", r"parallel helpers: 3", r"virtio-blk, .*completion by MSI-X interrupt"]},
        *DISK_IRQ,
        {"cmd": "ps", "must": [r"mind-tact", r"par-0"]},
        {"cmd": "ai cap", "must": [r"предел загрузки ядер фоном сейчас 50%"]},
        {"cmd": "ai cap 70", "must": [r"70%"]},
        {"cmd": "ai", "must": [r"предвидение: риск опоздания экрана", r"модель обучена на \d+ с", r"стабилизация: фон ужимался \d+ раз, всего \d+ с \(самый долгий период \d+ с\), возвратов давления после снятия предела \d+; цена тактики \d+,\d\d% ядра"]},
        {"cmd": "ai model", "must": [r"фоновые программы: -?\d+\.\d\d", r"обучено на \d+ с"]},
    ]),
    sc("boot-ata", "загрузка: IDE (как в Limbo)", BASIC, disk="ata", smp=2),
    sc("boot-sata", "загрузка: AHCI (SATA) на плате q35", BASIC + [{"log": [r"\[KIRA AHCI\] sata0", r"completion by MSI interrupt"]}] + DISK_IRQ, disk="sata", machine="q35"),
    sc("boot-nvme", "загрузка: NVMe", BASIC + [{"log": [r"\[KIRA NVMe\] nvme0n1", r"NVMe .*completion by interrupt"]}] + DISK_IRQ, disk="nvme"),
    sc("wx-early", "ранний W^X: ранняя карта страниц без W+X (проверка до vmm_enforce_wx), команда wx, запись в .text и .rodata даёт ловушку", BASIC + [
        {"log": [r"\[KIRA VMM\] W\^X early: ok, no W\+X page since the first instruction with paging on \(text=\d+ KB ro=\d+ KB rw=\d+ KB, checked before vmm_enforce_wx\)",
                 r"\[KIRA VMM\] W\^X kernel: ok \(text=\d+ KB ro=\d+ KB rw=\d+ KB\)"]},
        {"log_forbid": [r"W\^X early: the early page map has", r"W\^X violated", r"wrong rights"]},
        {"cmd": "wx", "must": [r"ранняя карта .*: без страниц W\+X с первой инструкции", r"\[KIRA VMM\] W\^X kernel: ok \(text=\d+ KB ro=\d+ KB rw=\d+ KB\)",
                              r"запись в \.text: ловушка сработала \(#PF\)", r"запись в \.rodata: ловушка сработала \(#PF\)", r"\[KIRA WX\] итог: ok"],
         "forbid": [r"ПРОШЛА", r"НАРУШЕНО", r"БЫЛИ страницы"]},
        {"cmd": "wx", "must": [r"итог: ok"]},                         # ловушка повторяема: система после неё жива
        {"cmd": "status", "must": [r"."]},
        {"log_forbid": NO_ERR},
    ], smp=4),
    sc("kaslr", "KASLR: три загрузки подряд на одном диске - база ядра в журнале разная, сеть (ping), файл на диске и status работают; без режима отладки база не печатается", _kaslr_boot("a") +
       [{"relaunch": []}] + _kaslr_boot("b") + [{"relaunch": []}] + _kaslr_boot("c") + [
        {"distinct": r"\[KIRA KASLR\] kernel base (0x[0-9A-Fa-f]+) \(link base 0x100000, slide 0x[0-9A-Fa-f]+\)", "count": 3},
        {"log_forbid": NO_ERR},
    ], smp=4, mem="1G", boot_timeout=120),
    sc("kaslr-off", "параметр ядра kira.nokaslr: образ остаётся на месте (1 МБ), причина в журнале, ранний W^X и остальное работают", BASIC + [
        {"log": [r"\[KIRA KASLR\] off: disabled by kira.nokaslr\.", r"\[KIRA VMM\] W\^X early: ok"]},
        {"log_forbid": [r"\[KIRA KASLR\] on:"]},
        {"cmd": "kptr debug on", "must": [r"режим отладки: включён"]},
        {"cmd": "kaslr", "must": [r"выключен: disabled by kira.nokaslr"]},
        {"cmd": "wx", "must": [r"итог: ok"]},
    ], boot="bios", bios_img="kira-bios-nokaslr.img", smp=2),
    sc("boot-1cpu", "одно ядро, 256 МБ", BASIC, smp=1, mem="256M"),
    sc("boot-8cpu", "восемь ядер, 4 ГБ (память выше 1 ГБ)", BASIC + [{"log": [r"Processors online: 8", r"Mapped RAM above 1 GB: [1-9]"]}], smp=8, mem="4G"),
    sc("isolation", "изоляция: адресные пространства, враждебные программы, IPC, потоки, FPU", [
        {"log": BOOT_LOG},
        {"cmd": "test isolation"}, {"sleep": 6},
        {"cmd": "test atk"}, {"sleep": 6},
        {"cmd": "test caps"}, {"sleep": 4},
        {"cmd": "test ipc"}, {"sleep": 8},
        {"cmd": "test gfxport"}, {"sleep": 4},
        {"log": [r"\[KIRA GFXCTL\] port kira\.gfx ready", r"gfxport: screen \d+x\d+, \d+ checks, 0 failed - PASS"]},
        {"cmd": "test fsport"}, {"sleep": 6},
        {"log": [r"\[KIRA FSSVC\] port kira\.fs ready", r"fsport: \d+ checks, 0 failed - PASS"]},
        {"cmd": "test threads"}, {"sleep": 6},
        {"cmd": "test fpu"}, {"sleep": 8},
        {"cmd": "rsdemo"}, {"sleep": 4},
        {"log": [r"rsdemo: sha256\(abc\) ok", r"rsdemo: sha256\(1e6 x 'a'\) ok", r"rsdemo: ring buffer ok", r"rsdemo: bounds ok", r"rsdemo: all checks passed \(PASS\)"]},
        {"log": [r"address spaces are isolated \(PASS\)", r"kernel GS intact after the attack \(PASS\)",
                 r"kernel pointer passed to write\(\) was rejected \(PASS\)", r"mt: .* - PASS|- PASS",
                 r"atk-cap: 18 calls without rights refused, harmless calls work \(PASS\)", r"\[KIRA CAPS\] process 'atk-cap' .* denied, no right CAP_GFX",
                 r"ipcx: foreign-domain open of a public shared-memory object refused \(PASS\)",
                 r"ipc client: descriptors passed through messages \(private port and shared memory, both directions\) \(PASS\)",
                 r"ipc server: descriptors received through messages: client's private port and shared memory \(PASS\)",
                 r"ipc server: wait on two ports: messages from both seen, timeout and duplicate refused \(PASS\)"]},
        {"log_forbid": [r"\(FAIL\)", r"isolation broken", r"ipc (server|client): FAIL", r"ipcx: FAIL", r"fsport: FAIL", r"gfxport: FAIL", r"fsport: \d+ checks, [1-9]\d* failed", r"mt: FAIL", r"rsdemo: (FAILED|panic)"] + NO_ERR},
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
        {"expect": r"RESULT: (PASS|FAIL)", "timeout": 100},   # 8 ядер спорят за один билетный замок: на 2 настоящих ядрах хозяина это от 2 с до нескольких минут
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
    sc("usb-xhci-iommu", "USB xHCI под VT-d (q35): свой домен, проверка принуждения, кольца и буферы выданы, хаб, клавиатура, флешка", [
        {"sleep": 6},
        {"log": [r"\[KIRA IOMMU\] xHCI: enforcement check passed"]},
        {"cmd": "usb", "must": [r"xHCI", r"хаб", r"клавиатура", r"планшет"]},
        {"cmd": "cat /usb0/hello.txt", "must": [r"kira-ci"]},
        {"cmd": "ls /usb0", "must": [r"hello.txt", r"big.bin"]},
        {"cmd": "umount usb0"},
        {"log_forbid": NO_ERR + [r"xHCI: .*NOT confirmed", r"DMA fault"]},
    ], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-device", "qemu-xhci,id=xhci", "-device", "usb-hub,bus=xhci.0,port=1",
              "-device", "usb-kbd,bus=xhci.0,port=1.2", "-device", "usb-tablet,bus=xhci.0,port=2"], flash=FLASH, hub=True),
    sc("usb-uhci", "USB UHCI (как в Limbo): планшет, клавиатура, флешка", [
        {"sleep": 4},
        {"cmd": "usb", "must": [r"UHCI", r"клавиатура|планшет"]},
        {"cmd": "cat /usb0/hello.txt", "must": [r"kira-ci"]},
        {"log_forbid": NO_ERR},
    ], extra=["-usb", "-device", "usb-tablet", "-device", "usb-kbd"], flash=FLASH),
    sc("usb-uhci-iommu", "USB UHCI под VT-d (q35): свой домен, проверка принуждения, память расписания выдана, планшет, клавиатура, флешка", [
        {"sleep": 6},
        {"log": [r"\[KIRA IOMMU\] UHCI: enforcement check passed"]},
        {"cmd": "usb", "must": [r"UHCI", r"клавиатура|планшет"]},
        {"cmd": "cat /usb0/hello.txt", "must": [r"kira-ci"]},
        {"log_forbid": NO_ERR + [r"UHCI: .*NOT confirmed", r"UHCI: could not grant", r"DMA fault"]},
    ], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-device", "piix3-usb-uhci,id=uhc",
              "-device", "usb-tablet,bus=uhc.0", "-device", "usb-kbd,bus=uhc.0"], flash=FLASH, flash_bus="uhc.0"),
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
    sc("limbo-spam", "как Limbo (эмуляция одним потоком, 4 ядра, IDE): 8 plasma - экран держит компоновщик (сборка на одном ядре), Разум наблюдает", [
        {"log": BOOT_LOG},
        *[{"send": "plasma"} for _ in range(8)],
        {"sleep": 25}, {"shot": "limbo-spam"},
        {"responsive": "status", "within": 10},
        {"cmd": "ai", "must": [r"тактика: фон", r"замок планировщика: ядра ждут его", r"композитор: .*параллельно [1-9]\d* нс, на одном ядре [1-9]"], "timeout": 20},
        {"cmd": "ai why", "timeout": 20},
        {"cmd": "ai levers", "must": [r"кадрами экрана Разум не управляет", r"сборка кадра"], "timeout": 20},
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
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,ipv4=on,ipv6=on,guestfwd=tcp:10.0.2.100:80-cmd:python3 " + HTTP_ONCE, "-device", "e1000,netdev=n0"], needs=["dns"]),
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
    sc("apps-explorer-nav", "Проводник (единый стиль): навигация клавишами (вход, Alt+←/→, Backspace), сортировка щелчком по шапке колонки, создание, переименование, удаление с подтверждением и отказом, выделение и буфер обмена (Ctrl+A/C/V), контекстное меню правой кнопкой и Shift+F10; состояние читается командой fmstate, плюс снимки", [
        {"log": BOOT_LOG},
        {"cmd": "mkdir /nv-a"}, {"cmd": "mkdir /nv-a/sub"}, {"cmd": "mkdir /nv-b"},
        {"cmd": "write /nv-a/a.txt x"}, {"cmd": "write /nv-a/c.txt cccccc"}, {"cmd": "write /nv-a/big.txt bigbigbigbigbigbig"},
        {"cmd": "files /nv-a /nv-b"}, {"sleep": 3}, {"shot": "nav-open"},
        {"cmd": "fmstate"}, {"sleep": 1},
        {"log": [r"state: active=L mode=normal L\{path=/nv-a cur=\.\. n=5 marks=0 sort=name:asc back=1 fwd=0\} R\{path=/nv-b cur=\.\. n=1 "]},
        # клавиши: вход в каталог, назад и вперёд по истории, вверх
        {"keys": "", "then": ["down"]}, {"keys": "", "then": ["ret"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a/sub cur=\.\. n=1 marks=0 sort=name:asc back=2 fwd=0\}"]},
        {"combo": ["alt", "left"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a cur=sub n=5 marks=0 sort=name:asc back=1 fwd=1\}"]},
        {"combo": ["alt", "right"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a/sub cur=\.\. n=1 marks=0 sort=name:asc back=2 fwd=0\}"]},
        {"keys": "", "then": ["backspace"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a cur=sub n=5 "]},
        # сортировка щелчком по шапке: по размеру (возрастание, затем убывание), снова по имени
        {"pointer": [673, 183]}, {"click": "left"}, {"sleep": 1},
        {"keys": "", "then": ["home", "down", "down"]}, {"sleep": 1}, {"shot": "nav-sort-size"},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a cur=a\.txt n=5 marks=0 sort=size:asc "]},
        {"click": "left"}, {"sleep": 1},
        {"keys": "", "then": ["home", "down", "down"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a cur=big\.txt n=5 marks=0 sort=size:desc "]},
        {"pointer": [600, 183]}, {"click": "left"}, {"sleep": 1},
        {"keys": "", "then": ["home", "down", "down"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a cur=a\.txt n=5 marks=0 sort=name:asc "]},
        # создание, переименование
        {"keys": "", "then": ["f7"]}, {"sleep": 1}, {"shot": "nav-mkdir"},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=input .*dialog=\"Новый каталог\""]},
        {"keys": "newd\n"}, {"sleep": 2},
        {"log": [r"\[KIRA FILES\] mkdir /nv-a/newd"]},
        {"keys": "", "then": ["f2"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=input .*dialog=\"Переименовать\" input=\"newd\""]},
        {"keys": "", "then": ["backspace", "backspace", "backspace", "backspace"]}, {"keys": "renm\n"}, {"sleep": 2},
        {"log": [r"\[KIRA FILES\] rename /nv-a/newd -> /nv-a/renm"]},
        {"cmd": "ls /nv-a", "must": [r"renm"], "forbid": [r"newd"]},
        # удаление: сначала отказ, затем подтверждение
        {"keys": "", "then": ["delete"]}, {"sleep": 1}, {"shot": "nav-delete"},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=confirm .*dialog=\"Удаление\""]},
        {"keys": "", "then": ["esc"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=normal "]},
        {"cmd": "ls /nv-a", "must": [r"renm"]},
        {"keys": "", "then": ["delete"]}, {"sleep": 1}, {"keys": "", "then": ["ret"]}, {"sleep": 2},
        {"log": [r"\[KIRA FILES\] delete /nv-a/renm"]},
        {"cmd": "ls /nv-a", "forbid": [r"renm"]},
        # выделить всё и копировать через буфер обмена в правую панель
        {"combo": ["ctrl", "a"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"L\{path=/nv-a cur=sub n=5 marks=4 "]},
        {"combo": ["ctrl", "c"]}, {"sleep": 1}, {"keys": "", "then": ["tab"]}, {"combo": ["ctrl", "v"]}, {"sleep": 3},
        {"log": [r"\[KIRA FILES\] copy /nv-a/big\.txt -> /nv-b/big\.txt", r"\[KIRA FILES\] copy /nv-a/a\.txt -> /nv-b/a\.txt"]},
        {"cmd": "cat /nv-b/c.txt", "must": [r"cccccc"]},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"active=R mode=normal .* clip=4\(copy\) menu=-"]},
        # контекстное меню: правая кнопка по строке правой панели, затем Esc; и с клавиатуры Shift+F10
        {"pointer": [850, 240]}, {"click": "right"}, {"sleep": 1}, {"shot": "nav-menu"},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"active=R mode=menu .* menu=0"]},
        {"keys": "", "then": ["down", "down"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=menu .* menu=3"]},
        {"keys": "", "then": ["esc"]}, {"sleep": 1},
        {"combo": ["shift", "f10"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=menu .* menu=0"]},
        {"keys": "", "then": ["esc"]}, {"sleep": 1},
        {"cmd": "fmstate"}, {"sleep": 1}, {"log": [r"mode=normal "]},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M"),
    sc("desktop-launcher", "меню запуска (F9): все программы по группам, поиск по названию и меткам, запуск Enter; значок открытой программы появляется в доке; закрытие Esc; снимки", [
        {"log": BOOT_LOG},
        {"keys": "", "then": ["f9"]}, {"sleep": 2}, {"shot": "launcher-groups"},
        {"keys": "edit"}, {"sleep": 1}, {"shot": "launcher-search"},
        {"keys": "", "then": ["ret"]}, {"sleep": 4}, {"shot": "launcher-edit-open"},
        {"log": [r"\[KIRA DOCK\] открыта: edit «Текстовый редактор»"]},
        {"keys": "", "then": ["f9"]}, {"sleep": 1}, {"keys": "", "then": ["esc"]}, {"sleep": 1}, {"shot": "launcher-closed"},
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
    ], smp=2, mem="1G", files=True, put={"/var/dock": "ci/fixtures/dock_plasma.txt"}),
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
    sc("python", "Python (MicroPython на WebAssembly): команда python - код, скрипт из песочницы, интерактивный режим, исключения и код выхода", [
        {"log": BOOT_LOG},
        {"cmd": "python -c print(6*7)"}, {"sleep": 10},
        {"log": [r"wasm: /wasm/python\.wasm - \d+ functions", r"\[USER pid \d+\] 42\b", r"wasm: finished with code 0"]},
        {"cmd": "write /data/wasm/hi.py print(sum(range(101)))"},
        {"cmd": "python hi.py"}, {"sleep": 10},
        {"log": [r"\[USER pid \d+\] 5050\b"]},
        {"send": "python"}, {"sleep": 10},
        {"type": "x = [i*i for i in range(5)]"}, {"sleep": 3}, {"type": "print(x)"}, {"sleep": 3},
        {"type": "1/0"}, {"sleep": 3}, {"type": "import sys; sys.exit(5)"}, {"sleep": 4},
        {"log": [r"MicroPython v1\.24\.1", r"\[0, 1, 4, 9, 16\]", r"ZeroDivisionError: divide by zero", r"wasm: finished with code 5"]},
        {"type": ""},                                                       # Enter: оболочка снова печатает приглашение
        {"cmd": "status", "must": [r"."]},
        {"cmd": "write /data/wasm/t.sh for i in 1 2 3; do echo n=$((i*i)); done"},
        {"cmd": "ksh t.sh"}, {"sleep": 8},
        {"log": [r"wasm: /wasm/ksh\.wasm - \d+ functions", r"n=1\b", r"n=4\b", r"n=9\b", r"wasm: finished with code 0"]},
        {"log_forbid": NO_ERR + [r"wasm: stopped"]},
    ], smp=2, mem="512M"),
    sc("ipcbench", "замер обмена: опрос против ожидания портов, 1 МиБ сообщениями против одного вложения памяти", [
        {"log": BOOT_LOG},
        {"cmd": "test ipcbench"}, {"sleep": 40},
        {"log": [r"ipcbench: round trip, polling two ports \(before\)", r"ipcbench: round trip, waiting on two ports \(after\)",
                 r"ipcbench: round trip, one port \(floor\)", r"ipcbench: server idle 300 ms by polling: \d+ rounds",
                 r"ipcbench: 1 MiB in 64 KiB messages \(before\)", r"ipcbench: 1 MiB by one shared-memory attachment \(after\)", r"ipcbench: done"]},
        {"log_forbid": [r"ipcbench: FAIL"] + NO_ERR},
    ], smp=2, mem="512M"),
    sc("mouse-load", "замер: цикл дисплея без мыши и при потоке движений мыши (PS/2), по этапам цикла", [
        {"log": BOOT_LOG},
        {"sleep": 8}, {"cmd": "fb"},
        {"sleep": 10}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"mouse": {"seconds": 10, "hz": 125, "step": 10}}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"mouse": {"seconds": 10, "hz": 500, "step": 10}}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"sleep": 5}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "status", "must": [r"."]},
    ], smp=2, mem="512M"),
    sc("stall-load", "замер: 4 ядра на 2 реальных, нагрузка burn и мышь; худшие паузы цикла дисплея, ожидание замка планировщика, Разум", [
        {"log": BOOT_LOG},
        {"sleep": 6}, {"cmd": "fb"},
        {"cmd": "test burn"}, {"sleep": 2},
        {"mouse": {"seconds": 15, "hz": 250, "step": 10}}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"sleep": 10}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "ai", "must": [r"."]},
    ], smp=4, mem="512M"),
    sc("latency", "время реакции под нагрузкой (А8): burn на всех ядрах + сеть + диск; гистограммы задержек прерывания, обработчика, критических секций и планировщика; границы из ci/latency_policy.json (стенд: QEMU TCG, -icount: время считается по числу выполненных команд, шум хозяина исключён)", LATENCY_LOAD + [
        {"latency_policy": "latency_policy.json", "timeout": 120},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=LATENCY_EXTRA),
    sc("latency-mutation", "проверка границ мутацией (А8): в обработчик таймера вносится искусственная задержка (latency inject isr), проверка границ ОБЯЗАНА нарушиться; сценарий проходит, когда она нарушилась", LATENCY_LOAD[:3] + [
        {"cmd": "latency inject isr 3000000", "must": [r"проверочная задержка обработчика таймера: 3000000 нс"]},
    ] + LATENCY_LOAD[3:] + [
        {"latency_policy": "latency_policy.json", "timeout": 120, "expect_fail": True},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M", extra=LATENCY_EXTRA),
    sc("blackbox", "чёрный ящик: событие записывается (проверочная пауза 1,3 с), кольцо сбрасывается на диск само, переживает обрыв питания и выключение; прежний запуск читается как bb old, два файла по очереди", [
        {"log": BOOT_LOG + [r"\[KIRA BLACKBOX\] flight recorder online"]},
        {"sleep": 3},
        {"cmd": "bb all", "must": [r"\[BB\] событий \d+", r"запуск системы, номер 1\b", r"\[BB\] запуск #1"], "timeout": 10},
        {"cmd": "ai fx 2"}, {"sleep": 3},
        {"cmd": "bb pause 1300", "must": [r"пауза основного цикла на 1300 мс"], "timeout": 10},
        {"sleep": 1},
        {"cmd": "bb all", "must": [r"\[ТЯЖЁЛО\] пауза 1[3-9]\d\d мс: основной цикл ядра не возвращался \[core\]", r"Разум: fx \[6\] -> 2", r"тяжёлых [1-9]"], "timeout": 10},
        {"cmd": "fb", "must": [r"\[BB\] событий \d+ \(в кольце \d+, вытеснено \d+, не записано из-за частоты \d+\): кадры \d+, замки \d+, ожидание процессора \d+, паузы [1-9]", r"\[BB\] запуск #1"]},
        {"sleep": 5},      # поток сброса сам записывает файл после тяжёлого события, команды bb save не было
        {"cmd": "bb file", "must": [r"\[BB\] файл /var/log/blackbox.bin: запуск #1, записей \d+", r"причина: (тяжёлое событие|первый сброс запуска|по расписанию)", r"пауза 1[3-9]\d\d мс"], "timeout": 10},
        {"crash": []},       # обрыв питания без выключения
        {"cmd": "bb old", "must": [r"\[BB\] файл /var/log/blackbox.bin: запуск #1", r"пауза 1[3-9]\d\d мс: основной цикл ядра не возвращался", r"Разум: fx \[6\] -> 2"], "timeout": 10},
        {"cmd": "bb all", "must": [r"запуск системы, номер 2\b", r"\[BB\] запуск #2"], "timeout": 10},
        {"cmd": "bb save", "must": [r"\[BB\] запуск #2, на диске /var/log/blackbox.bin записан \d+ с назад \(по команде\)"], "timeout": 10},
        {"cmd": "ls /var/log", "must": [r"blackbox\.bin", r"blackbox\.old\.bin"], "timeout": 10},
        {"cmd": "bb old", "must": [r"\[BB\] файл /var/log/blackbox.old.bin: запуск #1", r"пауза 1[3-9]\d\d мс"], "timeout": 10},      # первый сброс запуска #2 сдвинул файл #1 в .old
        {"cmd": "bb file", "must": [r"\[BB\] файл /var/log/blackbox.bin: запуск #2"], "timeout": 10},
        {"cmd": "bb pause 1100", "must": [r"пауза основного цикла на 1100 мс"], "timeout": 10},
        {"relaunch": []},    # poweroff: перед выключением ящик сбрасывается сам
        {"cmd": "bb old", "must": [r"\[BB\] файл /var/log/blackbox.bin: запуск #2", r"причина: перед выключением", r"пауза 1[1-9]\d\d мс"], "timeout": 10},
        {"cmd": "bb clear", "must": [r"кольцо очищено"], "timeout": 10},
        {"cmd": "bb", "must": [r"\[BB\] событий [1-3] \(", r"пометка: очищено"], "timeout": 10},
        {"cmd": "bb pause 5000", "must": [r"bb pause мс \(1\.\.2000\)"], "timeout": 10},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M"),
    sc("mind-cost", "замер: сколько процессора и памяти берёт Разум (ai cost) - покой с закрытым окном, покой с открытым окном «Разум», нагрузка burn и мышь", [
        {"log": BOOT_LOG},
        {"sleep": 6}, {"cmd": "ai cost", "must": [r"цена Разума за \d+ мс"], "timeout": 10},
        {"sleep": 30}, {"cmd": "ai cost", "must": [r"секундный шаг целиком: \d+ раз", r"тактика \(поток mind-tact\): \d+ раз", r"цикл экрана: проходов update_mind"], "timeout": 10},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"pointer": [412, 20, 1280, 720]}, {"click": "left"}, {"sleep": 3}, {"shot": "mind-window"},
        {"cmd": "ai cost", "timeout": 10},
        {"sleep": 30}, {"cmd": "ai cost", "must": [r"перерисовок окна Разума [1-9]\d*"], "timeout": 10},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "test burn"}, {"sleep": 2},
        {"mouse": {"seconds": 15, "hz": 250, "step": 10}},
        {"cmd": "ai cost", "timeout": 10},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "ai", "must": [r"."], "timeout": 10},
    ], smp=4, mem="512M"),
    sc("mind-cost-limbo", "замер как на телефоне (Limbo, эмуляция одним потоком): сколько процессора и памяти берёт Разум (ai cost) - покой с закрытым окном, покой с открытым окном «Разум», нагрузка burn и мышь", [
        {"log": BOOT_LOG},
        {"sleep": 6}, {"cmd": "ai cost", "must": [r"цена Разума за \d+ мс"], "timeout": 10},
        {"sleep": 30}, {"cmd": "ai cost", "must": [r"секундный шаг целиком: \d+ раз", r"тактика \(поток mind-tact\): \d+ раз", r"цикл экрана: проходов update_mind"], "timeout": 10},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"pointer": [412, 20, 1280, 720]}, {"click": "left"}, {"sleep": 3}, {"shot": "mind-window"},
        {"cmd": "ai cost", "timeout": 10},
        {"sleep": 30}, {"cmd": "ai cost", "must": [r"перерисовок окна Разума [1-9]\d*"], "timeout": 10},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "test burn"}, {"sleep": 2},
        {"mouse": {"seconds": 15, "hz": 250, "step": 10}},
        {"cmd": "ai cost", "timeout": 10},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "ai", "must": [r"."], "timeout": 10},
    ], smp=4, mem="512M", limbo=True),
    sc("mouse-limbo", "замер как на телефоне (Limbo: эмуляция одним потоком, 4 ядра, qemu64): цикл дисплея без мыши и при потоке движений мыши", [
        {"log": BOOT_LOG},
        {"sleep": 8}, {"cmd": "fb"},
        {"sleep": 10}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"mouse": {"seconds": 10, "hz": 125, "step": 10}}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"mouse": {"seconds": 10, "hz": 500, "step": 10}}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"sleep": 5}, {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"cmd": "ai", "must": [r"."]},
    ], smp=4, mem="512M", limbo=True),
    sc("wm-drag", "перетаскивание окон (А14): быстрое перетаскивание «Разума» и терминала по экрану при работающем кубе, экран 1412x1060 на virtio-gpu и один поток TCG, как Limbo на телефоне; замер времени кадра, пересчитанных точек, полных перерисовок и сверка с полной перерисовкой (остатков старого положения быть не должно)", [
        {"log": BOOT_LOG}, {"send": "3d"}, {"sleep": 8},
        {"open_window": {"title": "Разум", "launcher": "mind"}}, {"sleep": 3}, {"shot": "start"},
        {"cmd": "fb", "must": [r"\[WMWIN\] id \d+ .*\"Разум"]},
        {"drag": {"window": "Разум", "seconds": 8, "hz": 125, "step": 24}},
        {"cmd": "fb", "must": [r"\[WMPERF\] drawn \d+", r"\[PERIOD\] ms \d+ frames \d+"]},
        {"redraw_compare": {"name": "after-mind", "mask": ["Разум", "Терминал", "3D", "cube"], "max_diff": 0}},
        {"drag": {"window": "Терминал", "seconds": 8, "hz": 125, "step": 24}},
        {"cmd": "fb", "must": [r"\[WMPERF\] drawn \d+", r"\[PERIOD\] ms \d+ frames \d+"]},
        {"redraw_compare": {"name": "after-term", "mask": ["Разум", "Терминал", "3D", "cube"], "max_diff": 0}},
    ], smp=4, mem="1G", limbo=True, vga="none", extra=["-device", "virtio-gpu-pci,disable-legacy=on,xres=1412,yres=1060"]),
    sc("hover-look", "вид: наведение на значок Dock и вкладку - подпись рисуется целиком, следов нет", [
        {"log": BOOT_LOG}, {"sleep": 6},
        {"pointer": [603, 672, 1280, 720]}, {"shot": "hover-dock"},
        {"pointer": [660, 20, 1280, 720]}, {"shot": "hover-tab"},
        {"pointer": [1100, 300, 1280, 720]}, {"shot": "hover-away"},
    ], smp=2, mem="512M"),
    sc("wasm-speed", "WebAssembly: JIT x86-64 - плазма wgfx на стенде укладывается в порог по времени (до JIT на этом стенде было 1257 мс)", [
        {"log": BOOT_LOG},
        {"cmd": "wasm /wasm/wgfx.wasm"}, {"sleep": 6},
        {"log": [r"wasm: x86-64 native code, \d+ bytes \(JIT\)", r"wgfx: frame0=%s frame30=%s" % WGFX,
                 r"wasm: finished with code 0 in \d{1,3} ms"]},                 # порог: < 1000 мс
        {"log_forbid": NO_ERR + [r"wasm: stopped", r"wasm: interpreter mode"]},
    ], smp=2, mem="512M", needs=["kvm"]),
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
    sc("net-virtio-iommu", "virtio-net под VT-d (q35): свой домен устройства, проверка принуждения, DHCP и ping работают, нарушений DMA нет", [
        {"log": BOOT_LOG}, {"sleep": 8},
        {"log": [r"\[KIRA IOMMU\] VT-d translation enabled on [1-9]", r"\[KIRA IOMMU\] virtio-net: enforcement check passed", r"\[KIRA NET\] virtio-net: MAC 52:54:00"]},
        {"cmd": "net", "must": [r"карта virtio-net, канал есть", r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"log_forbid": NO_ERR + [r"virtio-net: .*NOT isolated"]},
    ], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,disable-legacy=on,iommu_platform=on"]),
    sc("virtio-iommu-all", "virtio-rng, virtio-input и virtio-gpu под VT-d (q35): у каждого свой домен и подтверждённое принуждение, устройства работают", [
        {"log": BOOT_LOG}, {"sleep": 6},
        {"log": [r"\[KIRA IOMMU\] virtio-rng: enforcement check passed", r"\[KIRA IOMMU\] virtio-gpu: enforcement check passed"]},
        {"log": [r"\[KIRA GPU\] virtio-gpu: \d+x\d+ 2D scanout"]},
        {"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]},
        {"log_forbid": NO_ERR + [r"virtio-(rng|gpu): .*NOT isolated", r"virtio-gpu found but did not start", r"virtio-gpu stopped answering"]},
    ], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-device", "virtio-rng-pci,disable-legacy=on,iommu_platform=on",
                                             "-device", "virtio-keyboard-pci,disable-legacy=on,iommu_platform=on", "-device", "virtio-gpu-pci,disable-legacy=on,iommu_platform=on"], vga="none"),
    sc("nvme-iommu", "NVMe под VT-d (q35): свой домен, проверка принуждения, система читает и пишет диск через выданные буферы", BASIC + [
        {"log": [r"\[KIRA IOMMU\] nvme: enforcement check passed", r"\[KIRA NVMe\] nvme0n1", r"NVMe .*completion by interrupt"]}] + DISK_IRQ + [
        {"log_forbid": NO_ERR + [r"nvme: .*NOT isolated", r"could not grant buffers"]}],
       disk="nvme", machine="q35", smp=2, mem="1G", extra=["-device", "intel-iommu,aw-bits=48"]),
    sc("net-e1000-iommu", "e1000 под VT-d (q35): свой домен, проверка принуждения, кольца выданы устройству, DHCP и ping работают", [
        {"log": BOOT_LOG}, {"sleep": 8},
        {"log": [r"\[KIRA IOMMU\] e1000: enforcement check passed", r"\[KIRA NET\] e1000 \(8086:"]},
        {"cmd": "net", "must": [r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"log_forbid": NO_ERR + [r"e1000: .*NOT isolated", r"e1000: could not grant"]},
    ], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]),
    sc("rcon", "удалённая консоль по TCP: выключена по умолчанию; пароль, включение, вход с хозяина, команда, неверный пароль отклоняется", [
        {"log": BOOT_LOG}, {"sleep": 6},
        {"cmd": "rcon status", "must": [r"rcon: выключена, порт 7077, пароля нет"]},
        {"cmd": "rcon on", "must": [r"сначала задайте пароль"]},
        {"cmd": "rcon passwd short", "must": [r"короче 8 знаков"]},
        {"cmd": "rcon passwd kira-test-pass", "must": [r"пароль задан"]},
        {"cmd": "rcon on", "must": [r"включена, порт 7077"]}, {"sleep": 3},
        {"log": [r"\[KIRA RCON\] listening on TCP port 7077"]},
        {"host_rcon": [17077, "wrong-password-1", ["status"]]},
        {"host_rcon": [17077, "kira-test-pass", ["PING", "rcon status", "net"]]},
        {"sleep": 2},
        {"log": [r"\[RCON\] KIRA-RCON 1", r"\[RCON\] ERR", r"\[RCON\] OK", r"\[RCON\] PONG", r"\[RCON\] rcon: включена, порт 7077, пароль задан, сеанс идёт",
                 r"\[RCON\] .*адрес 10\.0\.2\.15", r"\[KIRA RCON\] client logged in"]},
        {"cmd": "rcon status", "must": [r"сеансов 2, отказов 1"]},
        {"cmd": "rcon off", "must": [r"выключена"]},
        {"log_forbid": NO_ERR + [r"\[RCON\] .*kira-test-pass"]},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:17077-:7077", "-device", "e1000,netdev=n0"]),
    # Журнал аудита (docs/AUDIT.md, А7): события попадают в цепочку, audit verify OK, журнал переживает выключение и обрыв питания, независимая проверка
    # tools/auditverify.py сходится с ядром. Сеть нужна для rcon (неверный пароль, вход, команда).
    sc("audit", "журнал аудита: запуск, отказ прав, rcon (неверный пароль, вход, смена пароля) попадают в журнал; audit verify OK; цепочка не рвётся от выключения и обрыва питания; tools/auditverify.py", [
        {"log": BOOT_LOG}, {"sleep": 10},
        {"log": [r"\[KIRA AUDIT\] journal online: next record #\d+, boot #1, closed segments 0, key id [0-9a-f]{8}, first boot of this journal, chain verified OK"]},
        {"cmd": "test caps"}, {"sleep": 5},
        {"cmd": "rcon passwd kira-audit-pass", "must": [r"пароль задан"]},
        {"cmd": "rcon on", "must": [r"включена, порт 7077"]}, {"sleep": 3},
        {"host_rcon": [17078, "wrong-password-1", ["status"]]},
        {"host_rcon": [17078, "kira-audit-pass", ["rcon passwd kira-audit-pass-2", "PING"]]},
        {"sleep": 2},
        {"cmd": "audit 100", "must": [r"boot system OK pid=0 dom=0 arg=4 kernel", r"key system OK pid=0 dom=0 arg=0 device-key \| created on first boot", r"config console OK pid=0 dom=0 arg=7077 rcon \| rcon on", r"trust system \w+ pid=0 dom=0 arg=\d+ boot-trust",
                                      r"exec console OK pid=\d+ dom=\d+ arg=\d+ atk-cap", r"cap-deny program DENIED pid=\d+ dom=\d+ arg=1 atk-cap \| syscall \d+",
                                      r"rcon-login rcon DENIED pid=0 dom=0 arg=1 rcon \| wrong password", r"rcon-login rcon OK pid=0 dom=0 arg=0 rcon \| client logged in",
                                      r"rcon-passwd (console|rcon) OK .* rcon \| password changed"],
         "forbid": [r"kira-audit-pass"]},
        {"cmd": "audit status", "must": [r"status: boot #1, key id [0-9a-f]{16}, records \d+ \(on disk up to #\d+\)", r"lost 0, disk errors 0, tamper none, at boot: ok",
                                         r"hot path: \d+ events, mean \d+ ns per event"]},
        {"cmd": "audit verify", "timeout": 120, "must": [r"verify: OK: \d+ records \(#1\.\.#\d+\), [1-9]\d* signed checkpoints"], "forbid": [r"tamper detected", r"VIOLATION"]},
        {"send": "audit key"}, {"expect": r"public key \(Ed25519\): [0-9a-f]{64}", "timeout": 20},
        {"capture": {"pub": r"public key \(Ed25519\): ([0-9a-f]{64})"}},
        {"send": "audit export 1 6"}, {"expect": r"AUDEND|AUDNEXT \d+", "timeout": 20},
        {"save_log": {"pattern": r"^(AUDKEY [0-9a-f]{64}|AUDREC [0-9a-f]{320})", "as": "export1.txt", "min": 7}},
        {"host_run": {"argv": ["python3", "-I", "tools/auditverify.py", "--pub", "<<pub>>", "--export", "{out}/export1.txt"], "rc": 0, "must": [r"^OK: записей 6 \(с 1 по 6\)"]}},
        {"relaunch": ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:17078-:7077", "-device", "e1000,netdev=n0"],
         "disk_ops": [{"get": "/var/audit.log", "as": "p1_audit.log"}, {"get": "/var/audit.ckpt", "as": "p1_audit.ckpt"}]},
        {"sleep": 8},
        {"log": [r"\[KIRA AUDIT\] journal online: next record #\d+, boot #2, closed segments 0, key id [0-9a-f]{8}, previous run ended cleanly, chain verified OK"]},
        {"host_run": {"argv": ["python3", "-I", "tools/auditverify.py", "--pub", "<<pub>>", "--ckpt", "{out}/p1_audit.ckpt", "{out}/p1_audit.log"], "rc": 0, "must": [r"^OK: записей \d+ \(с 1 по \d+\), подписанных точек проверено [1-9]"]}},
        {"cmd": "audit 100", "must": [r"shutdown system OK pid=0 dom=0 arg=0 kernel \| clean shutdown", r"boot system OK pid=0 dom=0 arg=0 kernel"]},
        {"cmd": "audit verify", "timeout": 120, "must": [r"verify: OK: \d+ records \(#1\.\.#\d+\)"]},
        {"cmd": "test caps"}, {"sleep": 5},
        {"crash": ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:17078-:7077", "-device", "e1000,netdev=n0"]},
        {"sleep": 8},
        {"log": [r"\[KIRA AUDIT\] journal online: next record #\d+, boot #3, .*previous run ended without shutdown record, chain verified OK"]},
        {"cmd": "audit verify", "timeout": 120, "must": [r"verify: OK: \d+ records \(#1\.\.#\d+\)"], "forbid": [r"VIOLATION"]},
        {"cmd": "audit 100", "must": [r"boot system OK pid=0 dom=0 arg=1 kernel"]},
        {"log_forbid": NO_ERR + [r"tamper detected"]},
        # пароль не должен попасть в файл журнала на диске (в консольном логе он есть только как эхо команд харнесса)
        {"host_run": {"argv": ["grep", "-c", "-a", "kira-audit-pass", "{out}/p1_audit.log"], "rc": 1, "must": [r"^0$"]}},
    ], smp=2, mem="512M", extra=["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:17078-:7077", "-device", "e1000,netdev=n0"]),
    # Подмена: файл журнала правится на диске вне системы (машина выключена); следующая загрузка находит и показывает место, audit verify говорит то же,
    # tools/auditverify.py на вынутом файле - тоже; неизменённая копия проходит.
    sc("audit-tamper", "журнал аудита: файл /var/audit.log изменён на диске вне системы - следующая загрузка пишет [KIRA AUDIT] tamper detected с местом, audit verify и tools/auditverify.py сообщают нарушение", [
        {"log": BOOT_LOG}, {"sleep": 10},
        {"log": [r"\[KIRA AUDIT\] journal online: next record #\d+, boot #1, .*first boot of this journal, chain verified OK"]},
        {"cmd": "test caps"}, {"sleep": 5},
        {"cmd": "rcon passwd kira-tamper-pass", "must": [r"пароль задан"]},
        {"cmd": "rcon passwd kira-tamper-pass2", "must": [r"пароль задан"]},
        {"cmd": "audit checkpoint", "must": [r"checkpoint #\d+ signed and written"]},
        {"cmd": "audit verify", "timeout": 120, "must": [r"verify: OK: \d+ records"], "forbid": [r"VIOLATION"]},
        {"send": "audit key"}, {"expect": r"public key \(Ed25519\): [0-9a-f]{64}", "timeout": 20},
        {"capture": {"pub": r"public key \(Ed25519\): ([0-9a-f]{64})"}},
        {"relaunch": [], "disk_ops": [{"get": "/var/audit.log", "as": "clean_audit.log"}, {"get": "/var/audit.ckpt", "as": "clean_audit.ckpt"},
                                      {"xor": "/var/audit.log", "offset": 256 + 160 * 4 + 64, "mask": 1}]},    # запись №5: изменён первый байт detail
        {"sleep": 8},
        {"log": [r"\[KIRA AUDIT\] tamper detected \(boot\): .*цепочка хэшей разорвана.*: record #6 \(file #0, offset 1056\)",
                 r"\[KIRA AUDIT\] journal online: .*boot #2, .*chain VIOLATED"]},
        {"cmd": "audit status", "must": [r"tamper DETECTED, at boot: CHAIN"]},
        {"cmd": "audit verify", "timeout": 120, "must": [r"verify: VIOLATION", r"tamper detected \(verify\): .*цепочка хэшей разорвана.*record #6 \(file #0, offset 1056\)"], "forbid": [r"verify: OK"]},
        {"cmd": "audit 100", "must": [r"tamper kernel FAIL pid=0 dom=0 arg=6 boot \| CHAIN file#0 off 1056"]},
        {"cmd": "test caps"}, {"sleep": 5},                                   # система продолжает работать и писать в журнал, нарушение остаётся видно
        {"relaunch": [], "disk_ops": [{"get": "/var/audit.log", "as": "after_audit.log"}, {"get": "/var/audit.ckpt", "as": "after_audit.ckpt"}]},
        {"sleep": 8},
        {"log": [r"\[KIRA AUDIT\] tamper detected \(boot\): .*record #6"]},
        {"host_run": {"argv": ["python3", "-I", "tools/auditverify.py", "--pub", "<<pub>>", "--ckpt", "{out}/clean_audit.ckpt", "{out}/clean_audit.log"], "rc": 0, "must": [r"^OK: записей"]}},
        {"host_run": {"argv": ["python3", "-I", "tools/auditverify.py", "--pub", "<<pub>>", "{out}/after_audit.log"], "rc": 1, "must": [r"НАРУШЕНИЕ: цепочка хэшей разорвана.*запись 6 \(файл №0, смещение 1056\)"]}},
        {"log_forbid": NO_ERR},
    ], smp=2, mem="512M"),
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
        {"cmd": "net", "must": [r"TIME_WAIT: \d+ \(всего [1-9]"]},                             # serve закрывал первым: запись в TIME_WAIT была (на медленной машине 60 с уже вышли - смотрим «всего»)
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
    sc("cube3d-bench", "3D-движок k3d: самопроверка в Kira, замер кадра без помощников и с 3 потоками (на ядра QEMU, цифры шумные), потом обычный запуск", [
        {"log": BOOT_LOG},
        {"send": "run cube3d bench threads=3"},
        {"expect": r"cube3d: selftest PASS", "timeout": 150},
        {"expect": r"cube3d: k3d engine running, 3 helper threads", "timeout": 150},
        {"expect": r"cube3d: bench helpers=0: \d+ frames", "timeout": 150},
        {"expect": r"cube3d: bench helpers=3: \d+ frames", "timeout": 150},
        {"expect": r"cube3d: bench speedup x\d+\.\d+", "timeout": 150},
        {"send": "run cube3d"}, {"sleep": 8}, {"shot": "cube3d-k3d"},
        {"responsive": "status", "within": 5},
        {"log_forbid": NO_ERR},
    ], smp=4, mem="1G"),
    sc("cube3d-bench-mttcg", "то же на настоящих потоках хозяина (MTTCG): здесь помощники k3d идут по-настоящему параллельно", [
        {"log": BOOT_LOG},
        {"send": "run cube3d bench threads=3"},
        {"expect": r"cube3d: selftest PASS", "timeout": 150},
        {"expect": r"cube3d: bench helpers=0: \d+ frames", "timeout": 150},
        {"expect": r"cube3d: bench helpers=3: \d+ frames", "timeout": 150},
        {"expect": r"cube3d: bench speedup x\d+\.\d+", "timeout": 150},
        {"log_forbid": NO_ERR},
    ], smp=4, mem="1G", mttcg=True),
    sc("sched-scale", "планировщик на 8 ядрах: плазма x8 + burnall x3 + опрос ps/smp, замеры замков очередей (ai), отклик консоли", [
        {"log": BOOT_LOG},
        *[{"send": "plasma"} for _ in range(8)],
        {"send": "burnall"}, {"sleep": 5}, {"send": "burnall"}, {"sleep": 5}, {"send": "burnall"}, {"sleep": 6},
        {"responsive": "status", "within": 3},
        {"cmd": "ps", "must": [r"plasma"], "timeout": 20},
        {"cmd": "smp", "timeout": 20},
        {"cmd": "ai", "must": [r"замок планировщика: ядра ждут его \d+,\d% времени; взятий с ожиданием \d+ из \d+; затяжных ожиданий \d+"], "timeout": 20},
        {"responsive": "status", "within": 3},
        {"log_forbid": NO_ERR + [r"KERNEL PANIC", r"Watchdog"]},
    ], smp=8, mem="2G"),
    sc("power-off", "выключение: ACPI S5 из FADT/DSDT (_S5) - машина выключается сама, без -no-reboot-уловок", [
        {"log": BOOT_LOG},
        {"exit": "poweroff", "within": 20},
        {"log": [r"\[KIRA ACPI\] powering off \(S5\)"]},
        {"log_forbid": NO_ERR + [r"did not power off"]},
    ], smp=2, mem="512M"),
    sc("acpi-power-button", "кнопка питания (QMP system_powerdown, плата pc/PIIX): SCI -> интерпретатор AML -> штатное выключение с сохранением данных", [
        {"log": BOOT_LOG + [r"\[KIRA ACPI\] AML up"]},
        {"cmd": "write /acpi-btn.txt kira-acpi-button"},
        {"qmp_exit": "system_powerdown", "within": 30},
        {"log": [r"\[KIRA ACPI\] power button pressed", r"\[KIRA POWER\] power button: saving data", r"\[KIRA ACPI\] powering off \(S5\)"]},
        {"log_forbid": NO_ERR + [r"did not power off", r"AML is busy"]},
    ], smp=2, mem="512M"),
    sc("acpi-info", "ACPI на плате pc (PIIX): сводка интерпретатора, таблицы, выгрузка DSDT в журнал", [
        {"cmd": "acpi", "must": [r"ACPI AML: таблиц 1, объектов \d+.*ошибок загрузки 0", r"_INI 1 \(ошибок 0\)", r"ACPI _PRT .*PCI0: записей 128",
                                 r"устр 3 INTA -> GSI 11 уровень .*связка LNKC", r"устр 3 INTC -> GSI 10 уровень .*связка LNKA", r"устр 1 INTA -> GSI 9 уровень .*связка LNKS"], "timeout": 60},
        {"cmd": "acpi tables", "must": [r"DSDT: 0x"]},
        {"cmd": "acpi dump dsdt", "must": [r"ACPIDUMP BEGIN DSDT \d+", r"ACPIDUMP END DSDT"], "timeout": 120},
    ], smp=2, mem="512M"),
    sc("acpi-info-q35", "ACPI на плате q35 (ICH9): сводка интерпретатора, таблицы, выгрузка DSDT в журнал", [
        {"cmd": "acpi", "must": [r"ACPI AML: таблиц 1, объектов \d+.*ошибок загрузки 0", r"_INI 1 \(ошибок 0\)", r"ACPI _PRT .*PCI0: записей 128",
                                 r"устр 25 INTA -> GSI 16 уровень .*связка GSIA", r"устр 0 INTA -> GSI 20 уровень .*связка GSIE", r"устр 31 INTD -> GSI 19 уровень .*связка GSID"], "timeout": 60},
        {"cmd": "acpi tables", "must": [r"DSDT: 0x"]},
        {"cmd": "acpi dump dsdt", "must": [r"ACPIDUMP BEGIN DSDT \d+", r"ACPIDUMP END DSDT"], "timeout": 120},
    ], smp=2, mem="512M", machine="q35"),
    sc("acpi-power-button-q35", "кнопка питания (QMP system_powerdown, плата q35/ICH9): та же цепочка на другой плате", [
        {"log": BOOT_LOG + [r"\[KIRA ACPI\] AML up"]},
        {"qmp_exit": "system_powerdown", "within": 30},
        {"log": [r"\[KIRA ACPI\] power button pressed", r"\[KIRA ACPI\] powering off \(S5\)"]},
        {"log_forbid": NO_ERR + [r"did not power off", r"AML is busy"]},
    ], smp=2, mem="512M", machine="q35"),
    sc("sleep-s3-q35", "сон S3 (плата q35/ICH9, сеть e1000): sleep -> QMP system_wakeup -> сеть, диск, часы, все ядра на месте", S3_STEPS, smp=4, mem="512M", machine="q35",
       extra=["-global", "ICH9-LPC.disable_s3=0", "-global", "ICH9-LPC.disable_s4=1", "-nic", "user,model=e1000"]),
    sc("sleep-s3-q35-virtio", "сон S3 (плата q35, сеть virtio-net): та же цепочка с virtio-устройствами", S3_STEPS, smp=4, mem="512M", machine="q35",
       extra=["-global", "ICH9-LPC.disable_s3=0", "-global", "ICH9-LPC.disable_s4=1", "-nic", "user,model=virtio-net-pci"]),
    sc("sleep-s3-pc", "сон S3 (плата pc/PIIX4, сеть e1000): sleep -> QMP system_wakeup -> всё работает", S3_STEPS, smp=4, mem="512M",
       extra=["-global", "PIIX4_PM.disable_s3=0", "-global", "PIIX4_PM.disable_s4=1", "-nic", "user,model=e1000"]),
    sc("sleep-s3-nvme-iommu", "сон S3: NVMe под VT-d (q35): контроллер и трансляция возвращаются, диск читается и пишется после пробуждения", s3_steps(boot=[r"\[KIRA NVMe\] nvme0n1"]),
       disk="nvme", machine="q35", smp=4, mem="1G", extra=S3_Q35 + ["-device", "intel-iommu,aw-bits=48", "-nic", "user,model=e1000"]),
    sc("sleep-s3-sata", "сон S3: AHCI (SATA) на плате q35: порт останавливается и запускается, диск читается и пишется", s3_steps(boot=[r"\[KIRA AHCI\] sata0"]),
       disk="sata", machine="q35", smp=4, mem="512M", extra=S3_Q35 + ["-nic", "user,model=e1000"]),
    sc("sleep-s3-ata", "сон S3: IDE (ATA PIO) на плате pc", s3_steps(boot=[r"\[KIRA ATA\] Disk"]), disk="ata", smp=4, mem="512M",
       extra=["-global", "PIIX4_PM.disable_s3=0", "-global", "PIIX4_PM.disable_s4=1", "-nic", "user,model=e1000"]),
    sc("sleep-s3-usb-xhci", "сон S3: xHCI с хабом, клавиатурой, планшетом и флешкой: после пробуждения устройства подключаются заново, флешка читается", s3_steps(
        before=[{"sleep": 3}, {"cmd": "usb", "must": [r"xHCI", r"хаб", r"клавиатура", r"планшет"]}, S3_USB],
        after=[{"sleep": 6}, {"cmd": "usb", "must": [r"xHCI", r"хаб", r"клавиатура", r"планшет"]}, {"cmd": "df", "must": [r"usb0"]}, S3_USB]),
       machine="q35", smp=4, mem="512M", flash=FLASH, hub=True,
       extra=S3_Q35 + ["-nic", "user,model=e1000", "-device", "qemu-xhci,id=xhci", "-device", "usb-hub,bus=xhci.0,port=1",
                       "-device", "usb-kbd,bus=xhci.0,port=1.2", "-device", "usb-tablet,bus=xhci.0,port=2"]),
    sc("sleep-s3-usb-uhci", "сон S3: UHCI с планшетом, клавиатурой и флешкой (плата pc)", s3_steps(
        before=[{"sleep": 3}, {"cmd": "usb", "must": [r"UHCI", r"клавиатура|планшет"]}, S3_USB],
        after=[{"sleep": 6}, {"cmd": "usb", "must": [r"UHCI", r"клавиатура|планшет"]}, S3_USB]),
       smp=4, mem="512M", flash=FLASH,
       extra=["-global", "PIIX4_PM.disable_s3=0", "-global", "PIIX4_PM.disable_s4=1", "-nic", "user,model=e1000", "-usb", "-device", "usb-tablet", "-device", "usb-kbd"]),
    sc("sleep-s3-hda", "сон S3: Intel HDA: контроллер и кодек возвращаются, тон снова играет через DMA", s3_steps(before=[S3_BEEP], after=[S3_BEEP], boot=[r"\[KIRA HDA\] Sound: codec"]),
       machine="q35", smp=4, mem="512M", extra=S3_Q35 + ["-nic", "user,model=e1000", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0"]),
    sc("sleep-s3-ac97", "сон S3: Intel AC'97", s3_steps(before=[S3_BEEP], after=[S3_BEEP], boot=[r"\[KIRA AC97\] Sound: codec"]),
       machine="q35", smp=4, mem="512M", extra=S3_Q35 + ["-nic", "user,model=e1000", "-device", "AC97,audiodev=a0"]),
    sc("sleep-s3-sb16", "сон S3: Sound Blaster 16 (ISA)", s3_steps(before=[S3_BEEP], after=[S3_BEEP], boot=[r"\[KIRA SB16\] Sound: Sound Blaster 16"]),
       smp=4, mem="512M", extra=["-global", "PIIX4_PM.disable_s3=0", "-global", "PIIX4_PM.disable_s4=1", "-nic", "user,model=e1000", "-device", "sb16,audiodev=a0"]),
    sc("sleep-s3-virtio-iommu", "сон S3: virtio-rng, virtio-input, virtio-gpu и virtio-net под VT-d (q35): домены возвращаются, экран, ввод и сеть работают", s3_steps(
        before=[{"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]}], after=[{"cmd": "fb", "must": [r"\[PERIOD\] ms \d+ frames \d+"]}],
        boot=[r"\[KIRA IOMMU\] virtio-rng: enforcement check passed", r"\[KIRA GPU\] virtio-gpu: \d+x\d+ 2D scanout"], net=True),
       smp=4, mem="1G", machine="q35", vga="none",
       extra=S3_Q35 + ["-device", "intel-iommu,aw-bits=48", "-device", "virtio-rng-pci,disable-legacy=on,iommu_platform=on", "-device", "virtio-keyboard-pci,disable-legacy=on,iommu_platform=on",
                       "-device", "virtio-gpu-pci,disable-legacy=on,iommu_platform=on", "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,disable-legacy=on,iommu_platform=on"]),
    sc("power-reboot","перезагрузка на q35 (регистр сброса FADT / 0xCF9): система поднимается снова, файл, записанный до сброса, на месте", [
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
                                 "-device", "virtio-net-pci,netdev=n0"], needs=["ipv6"]),
]

# ---- Разум: рычаги 13-23 ----
MELODY_LOOP = [st for _ in range(75) for st in ({"send": "melody"}, {"sleep": 1.5})]      # звук без перерывов ~2 мин: рычагам звука есть что измерять
LEVERS_LIST = [r"13\. вес окна в фокусе: 6 /4 \(обычно 6\)", r"16\. период тактики: 25 мс \(обычно 25\)", r"19\. опережение звука перед картой: 100 мс \(обычно 100\)",
               r"21\. предел одного запроса к диску: 256 блоков", r"23\. окно предела доли процессора: 50 тиков"]
ALL.append(sc("mind-levers", "Разум: 11 рычагов (13-23) в `ai levers`; звук без перерыва - рычаг опережения пробуется, оставляется или откатывается по замеру; натиск программ; опыт записывается", [
    {"log": BOOT_LOG},
    {"cmd": "ai levers", "must": LEVERS_LIST, "timeout": 10},
    *MELODY_LOOP,
    {"log": [r"\[KIRA MIND\] рычаг 19 «опережение звука перед картой»: пробую \d+ мс вместо 100 \(причина: ",
             r"\[KIRA MIND\] рычаг 19 «опережение звука перед картой»: (оставлено|откат на|срочный откат) "]},
    {"cmd": "ai levers", "must": [r"19\. опережение звука перед картой: \d+ мс \(обычно 100\) - [^\n]*проб \d+, удач \d+, откатов \d+", r"смен значений за запуск [1-9]"], "timeout": 10},   # откат из-за «нагрузка изменилась» (нечестное сравнение) в счётчик проб не идёт - на медленной машине так бывает всегда
    *[{"send": "plasma"} for _ in range(8)],
    {"sleep": 40}, {"shot": "levers-load"},
    {"responsive": "status", "within": 5},
    {"cmd": "ai why", "must": [r"."], "timeout": 10},
    {"cmd": "ai save", "must": [r"опыт записан"], "timeout": 10},
    {"cmd": "ls /var", "must": [r"mind\.dat"]},
    {"log_forbid": NO_ERR},
], smp=4, mem="2G", extra=["-device", "intel-hda", "-device", "hda-duplex,audiodev=a0"]))

# Испытательное семя опыта собирается на лету, если его ещё нет (ci-out не в git).
def _mind_seed_fixture():
    import os, subprocess
    out = "ci-out/mind_seed_fixture.bin"
    if os.path.exists(out):
        return
    os.makedirs("ci-out", exist_ok=True)
    exe = "ci-out/mindseed_tool"
    subprocess.run(["cc", "-O1", "-I.", "-o", exe, "tools/mindseed.c", "mind.c", "mindnet.c"], check=True)
    subprocess.run([exe, "--fixture", out], check=True, stdout=subprocess.DEVNULL)
_mind_seed_fixture()

# Повреждённый основной файл Mind не должен ломать запуск: архив проверяется, затем используется
# ограниченное семя, а нормы текущей машины обучаются заново.
ALL.append(sc("mind-corrupt-state", "Разум: восстановление после повреждённого основного файла опыта", [
    {"log": BOOT_LOG}, {"sleep": 4},
    {"log": [r"\[KIRA MIND\] нет своего опыта: взято семя \(из QEMU, на железе лишь подсказка\), веса на 90 с"]},
    {"cmd": "ai model", "must": [r"фоновые программы: -?\d\.\d\d", r"обучено на (9\d|1[0-4]\d) с"], "timeout": 10},
    {"cmd": "ai", "must": [r"пока без права действовать"], "timeout": 10},
    {"log_forbid": NO_ERR},
], smp=2, mem="512M", put={
    "/var/mind.dat": "tests/fixtures/mind-corrupt-model.bin",
    "/etc/mind_seed.bin": "ci-out/mind_seed_fixture.bin",
}))

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
    {"cmd": "drv stop edu"}, {"sleep": 2},
    # Тот же драйвер на Rust (userland/rust/rsedu.rs): те же проверки устройства и отказов ядра, без unsafe в коде драйвера
    {"cmd": "drv start rsedu", "must": [r"драйвер запущен как drv-rsedu"]}, {"sleep": 3},
    {"log": [r"rsedu: device 0x1234:0x11e8, rights 0x4800, no MSI", r"rsedu: id=0x10000ed", r"rsedu: factorial ok",
             r"rsedu: DMA denied: CAP_DMA not granted \(least-privilege PASS\)", r"rsedu: kernel guards ok", r"rsedu: PASS"]},
    {"cmd": "drv stop rsedu"}, {"sleep": 2},
    # выход за BAR в Rust - проверка смещения, паника и штатный выход 101 (не исключение процессора)
    {"cmd": "drv start rsedu oob", "must": [r"драйвер запущен как drv-rsedu"]}, {"sleep": 3},
    {"log": [r"rsedu: reading past the BAR on purpose", r"rust: panic", r"\[KIRA DRV\] drv-rsedu \(pid \d+\) завершён"]},
    {"log_forbid": [r"drvedu: .*\(FAIL\)", r"rsedu: .*\(FAIL\)", r"УТЕЧКА"] + NO_ERR},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", extra=["-device", "edu"]))


# Диск virtio-blk в Ring 3 (CAP_BLKDEV, kdrv_blk.h): плата q35 с VT-d; системный диск vda берёт встроенный драйвер, второй virtio-blk (iommu_platform) ядро не берёт -
# его получает драйвер Ring 3 (пакет vblk), заводит в blk.c как vdb и обслуживает кольца запросов в общей памяти по прерываниям MSI-X (без опроса).
# Проверка диска - test blk vdb: запись и чтение 300 секторов (три буфера), возврат прежнего содержимого, выход за границы отклоняется (ядро режет до драйвера).
# Затем: остановка драйвера (диск остаётся в списке, операции - ошибка, ядро живо), перезапуск с враждебными ответами (evil), падение драйвера посреди записи
# (crash), пакет без права CAP_BLKDEV (vblknc) - регистрация отклонена.
ALL.append(sc("drv-blk", "Ring 3: диск virtio-blk через CAP_BLKDEV (кольца в общей памяти, VT-d, MSI-X): чтение/запись/границы, остановка, враждебные ответы, падение посреди записи, пакет без права диска", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA IOMMU\] VT-d translation enabled on [1-9]"]},
    {"sleep": 8},
    {"log": [r"\[KIRA DRV\] vblk: устройство .* 1af4:1042 -> драйвер запущен как drv-vblk \(pid \d+\), SHA-256 сверен, права: .*CAP_DMA.*CAP_BLKDEV",
             r"drvvblk: bad-reg: name=-10 sector=-10 empty=-10 huge=-10 flags=-10 bits=-10 noterm=-10 nullptr=-10 \(refused PASS\)",
             r"drvvblk: capacity 32768 sectors, queue 32, features 0x[0-9a-f]+, completion by interrupt",
             r"drvvblk: disk vdb registered, data at 0x[0-9a-f]+, ring slots 8",
             r"\[KIRA DRV\] vblk: диск vdb заведён в blk\.c: 32768 секторов по 512 Б"]},
    {"cmd": "disk", "must": [r"vda\s+.*\[системный\]", r"vdb\s+16 МБ\s+Ring 3 drv-vblk\s+\[пустой\]"]},
    {"cmd": "test blk vdb", "must": [r"запись\+чтение 300 секторов совпали", r"прежнее содержимое возвращено", r"выход за границы отклонён \(5 из 5\)", r"flush ok PASS"], "timeout": 60,
     "forbid": [r"FAIL"]},
    {"cmd": "drv", "must": [r"drv-vblk pid \d+.*прерываний [1-9]\d*.*диск vdb: запросов [1-9]\d*, битых ответов 0"]},
    {"cmd": "drv stop vblk", "must": [r"останавливаю drv-vblk"]}, {"sleep": 2},
    {"log": [r"\[KIRA DRV\] drv-vblk \(pid \d+\) завершён: устройство .* остановлено",
             r"\[KIRA DRV\] vblk: возвращено памяти DMA и колец [1-9]\d* из [1-9]\d* стр\..*утечек нет"]},
    {"cmd": "test blk vdb", "must": [r"vdb: чтение 300 секторов: ошибка 5 .*FAIL"]},              # драйвера нет: ошибка, не авария ядра
    {"cmd": "disk", "must": [r"vdb\s+16 МБ\s+Ring 3 drv-vblk"]},                                   # диск остаётся в списке
    {"responsive": "status", "within": 5},
    {"cmd": "drv start vblk evil", "must": [r"драйвер запущен как drv-vblk"]}, {"sleep": 4},
    {"log": [r"drvvblk: evil: rsp_head out of ring", r"\[KIRA DRV\] vblk: диск vdb заведён в blk\.c"]},
    {"cmd": "test blk vdb", "must": [r"запись\+чтение 300 секторов совпали", r"выход за границы отклонён \(5 из 5\)", r"PASS"], "timeout": 60, "forbid": [r"FAIL"]},
    {"log": [r"drvvblk: evil: response with unknown id"]},
    {"cmd": "drv", "must": [r"drv-vblk pid \d+.*диск vdb: запросов [1-9]\d*, битых ответов [1-9]\d*"]},      # ядро отбросило и посчитало враждебные ответы
    {"cmd": "drv stop vblk"}, {"sleep": 2},
    {"cmd": "drv start vblk crash", "must": [r"драйвер запущен как drv-vblk"]}, {"sleep": 4},
    {"cmd": "test blk vdb", "must": [r"запись: ошибка 5 FAIL"], "timeout": 60},                  # драйвер падает на первой записи: запрос в полёте завершается ошибкой
    {"log": [r"drvvblk: crashing on purpose", r"\[KIRA PROC\] pid \d+ killed by CPU exception #14", r"\[KIRA DRV\] drv-vblk \(pid \d+\) завершён"]},
    {"responsive": "status", "within": 5}, {"sleep": 2},
    {"cmd": "drv start vblknc", "must": [r"драйвер запущен как drv-vblknc"]}, {"sleep": 4},
    {"log": [r"drvvblk: CAP_BLKDEV not granted: register denied \(least-privilege PASS\)", r"\[KIRA CAPS\] process 'drv-vblknc' .* denied, no right CAP_BLKDEV"]},
    {"cmd": "drv start vblk", "must": [r"драйвер запущен как drv-vblk"]}, {"sleep": 4},
    {"cmd": "test blk vdb", "must": [r"запись\+чтение 300 секторов совпали", r"прежнее содержимое возвращено", r"PASS"], "timeout": 60, "forbid": [r"FAIL"]},
    {"log_forbid": [r"drvvblk: .*\(FAIL\)", r"УТЕЧКА", r"\[KIRA IOMMU\] DMA fault", r"драйвер не ответил"] + NO_ERR},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", machine="q35", spare_mb=16,
   extra=["-device", "intel-iommu,aw-bits=48", "-drive", "file={spare},format=raw,if=none,id=kd1",
          "-device", "virtio-blk-pci,drive=kd1,disable-legacy=on,iommu_platform=on"]))


# Порты ввода-вывода Ring 3 (KDRV_SYS_IO): драйвер читает MAC из PROM настоящей карты ne2k_pci через порты её I/O BAR и проверяет отказы ядра.
ALL.append(sc("drv-io", "Ring 3: порты I/O BAR своей карты (ne2k_pci): чтение MAC, одиночные и ins; чужой BAR, выход за границы, невыровненный доступ, ширина, порты как память - отказ", [
    {"log": BOOT_LOG},
    {"sleep": 6},
    {"log": [r"\[KIRA DRV\] ne2kio: устройство .* 10ec:8029 -> драйвер запущен как drv-ne2kio",
             r"drvne2kio: bar0 flags 1 size 256", r"drvne2kio: isr after reset=\d+", r"drvne2kio: ins rc=0 mac=52:54:00:12:34:99",
             r"drvne2kio: refused: mem-bar=-\d+ past=-\d+ unaligned=-\d+ width3=-\d+ no-bar=-\d+ bad-buf=-\d+ map-ports=-\d+",
             r"drvne2kio: PASS"]},
    {"log_forbid": [r"drvne2kio: .*\(FAIL\)", r"drvne2kio: partial"] + NO_ERR},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", extra=["-device", "ne2k_pci,mac=52:54:00:12:34:99"]))


# Драйвер OpenBSD (if_ne_pci + dp8390 + ne2000 + rtl80x9 из закреплённого корпуса) в Ring 3 через слой совместимости BSD: карта ne2k_pci,
# ядро получает сетевую карту драйвера, DHCP QEMU выдаёт адрес через неё. Карту сначала держит тестовый ne2kio: его останавливают, затем запускают bsdne.
ALL.append(sc("drv-bsd-ne2k", "драйвер OpenBSD ne(4) в Ring 3 через слой совместимости: карта ne2k_pci, регистрация интерфейса, DHCP и ping через неё", [
    {"log": BOOT_LOG},
    {"sleep": 6},
    {"log": [r"drvne2kio: PASS"]},                                           # первым карту берёт тестовый драйвер портов (пакет ne2kio)
    {"cmd": "drv stop ne2kio", "must": [r"останавливаю drv-ne2kio"]}, {"sleep": 2},
    {"cmd": "drv start bsdne", "must": [r"драйвер запущен как drv-bsdne"]}, {"sleep": 8},
    {"log": [r"ne0 at pci 10ec:8029", r"bsdrt: ne0 up, mac 52:54:00:12:34:56",
             r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
    {"cmd": "net", "must": [r"канал есть", r"адрес 10\.0\.2\.15"]},
    {"cmd": "ping 10.0.2.2 3", "must": [r"ответ от 10\.0\.2\.2", r"отправлено 3, получено [1-3]"], "timeout": 20},
    {"sleep": 6},
    {"log": [r"bsdrt: прерывания: INTx по событию", r"bsdrt: ne0: tx [1-9][0-9]* rx [1-9][0-9]* .* irq [1-9][0-9]*"]},   # прерывания настоящие: карта их прислала
    {"log_forbid": [r"bsdrt: .*FAIL", r"bsd panic", r"bsd: порты отказали", r"irq [0-9]{5,}"] + NO_ERR},               # и без шторма
    {"responsive": "status", "within": 5},
], smp=2, mem="1G", extra=["-netdev", "user,id=n0", "-device", "ne2k_pci,netdev=n0"]))


# Драйверы OpenBSD с настоящим DMA (bus_dma через слой совместимости): плата q35 с VT-d (intel-iommu), устройство видит только блоки, выданные ядром.
# Кольца дескрипторов - память bus_dmamem, буферы кадров копируются через промежуточные блоки; DHCP и ping через эту карту, прерывания INTx.
# Пакет драйвера стартует сам при загрузке: карта находится по PCI-ID.
def _bsd_dma_sc(name, about, pci, ifname, qdev, irq=True):
    return sc(name, about, [
        {"log": BOOT_LOG},
        {"log": [r"\[KIRA IOMMU\] VT-d translation enabled on [1-9]"]},
        {"sleep": 8},
        {"log": [r"\[KIRA IOMMU\] 00:03\.0 isolated: own translation domain", r"%s at pci %s" % (ifname, pci), r"bsdrt: %s up, mac 52:54:00:12:34:56" % ifname,
                 r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
        {"cmd": "net", "must": [r"канал есть", r"адрес 10\.0\.2\.15"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"ответ от 10\.0\.2\.2", r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"sleep": 6},
        {"log": [r"\[KIRA DRV\] INTx: GSI (1[6-9]|2[0-3]) via IOAPIC vector", r"bsdrt: прерывания: INTx по событию", r"bsdrt: %s: tx [1-9][0-9]* rx [1-9][0-9]* .* irq [1-9][0-9]*" % ifname]} if irq else
        {"log": [r"bsdrt: прерывания: опрос раз в 10 мс", r"bsdrt: %s: tx [1-9][0-9]* rx [1-9][0-9]* " % ifname]},
        {"log_forbid": [r"bsdrt: .*FAIL", r"bsd panic", r"irq [0-9]{5,}", r"\[KIRA IOMMU\] DMA fault", r"00:03\.0 .*NOT isolated: (named|no active|domain setup)",
                        r"00:03\.0 back to passthrough"] + NO_ERR},
        {"responsive": "status", "within": 5},
    ], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-netdev", "user,id=n0", "-device", qdev + ",netdev=n0,romfile="] + (["-trace", "tulip_*"] if os.environ.get("KIRA_TRACE") else []))


ALL.append(_bsd_dma_sc("drv-bsd-pcn", "драйвер OpenBSD pcn(4) в Ring 3 с DMA под VT-d: карта pcnet на q35, DHCP и ping через неё, прерывания INTx", "1022:2000", "pcn0", "pcnet"))
ALL.append(sc("drv-bsd-pcn-bridge", "драйвер OpenBSD pcn(4) в Ring 3 за мостом PCIe (корневой порт, шина 1) под VT-d: линия INTx по правилу моста, DHCP, ping", [
    {"log": BOOT_LOG}, {"sleep": 8},
    {"log": [r"pcn0 at pci 1022:2000", r"\[KIRA DRV\] INTx: GSI (1[6-9]|2[0-3]) via IOAPIC vector \(PCI 1:0\.0\)", r"bsdrt: pcn0 up, mac 52:54:00:12:34:56",
             r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
    {"cmd": "ping 10.0.2.2 3", "must": [r"отправлено 3, получено [1-3]"], "timeout": 20},
    {"sleep": 6},
    {"log": [r"bsdrt: прерывания: INTx по событию", r"bsdrt: pcn0: tx [1-9][0-9]* rx [1-9][0-9]* .* irq [1-9][0-9]*"]},
    {"log_forbid": [r"bsdrt: .*FAIL", r"bsd panic", r"irq [0-9]{5,}"] + NO_ERR},
], smp=2, mem="1G", machine="q35", extra=["-device", "intel-iommu,aw-bits=48", "-netdev", "user,id=n0", "-device", "pcie-root-port,id=rp1,chassis=1,slot=1", "-device", "pcnet,netdev=n0,bus=rp1,romfile="]))
ALL.append(_bsd_dma_sc("drv-bsd-fxp", "драйвер OpenBSD fxp(4) в Ring 3 с DMA под VT-d: Intel 82559 (i82559er) на q35, DHCP и ping через неё, прерывания INTx", "8086:1209", "fxp0", "i82559er"))
ALL.append(_bsd_dma_sc("drv-bsd-dc", "драйвер OpenBSD dc(4) в Ring 3 с DMA под VT-d: DEC 21143 (tulip) на q35, DHCP и ping, прерывания INTx", "1011:0019", "dc0", "tulip"))


# drvget + драйвер RTL8139 (пакет L, N). Фаза 1: сеть virtio; drvget по настоящему TLS 1.3 скачивает исходники драйвера rl с хозяина,
# показывает источник/лицензию/SHA-256/права, ставит только после подтверждения хешем, собирает kcc на устройстве и пишет /drv.
# Фаза 2 (relaunch на том же диске): вместо virtio-net - rtl8139; драйвер из /drv находит карту по PCI-ID и регистрирует сетевой интерфейс.
def _drv_www():
    import hashlib, shutil, struct, re
    w = os.path.abspath("ci-out/drvwww")
    shutil.rmtree(w, ignore_errors=True)
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "drivers", "net")
    def clone(name):
        os.makedirs(os.path.join(w, name))
        for f in ("drvget.rec", "rtl8139_kdrv.c", "rtl8139.c", "rtl8139.h"):
            shutil.copy(os.path.join(src, f), os.path.join(w, name, f))
    def edit(name, f, fn):
        pth = os.path.join(w, name, f)
        b = open(pth, "rb").read(); open(pth, "wb").write(fn(b))
    clone("rl")
    clone("gpl"); edit("gpl", "drvget.rec", lambda b: b.replace(b"\nname rl", b"\nname gpl"))
    edit("gpl", "rtl8139.h", lambda b: b.replace(b"SPDX-License-Identifier: BSD-3-Clause", b"SPDX-License-Identifier: GPL-2.0-only"))
    # Keep the fixture internally valid: the source hash must match the deliberately GPL-marked file,
    # otherwise drvget correctly rejects the stale SHA-256 before reaching its licence scanner.
    gpl_hash = hashlib.sha256(open(os.path.join(w, "gpl", "rtl8139.h"), "rb").read()).hexdigest().encode()
    edit("gpl", "drvget.rec", lambda b: re.sub(rb"(file rtl8139[.]h rtl8139[.]h sha256 )[0-9a-f]{64}", lambda m: m.group(1) + gpl_hash, b))
    clone("bin"); edit("bin", "drvget.rec", lambda b: b.replace(b"\nname rl", b"\nname bin"))
    edit("bin", "rtl8139.h", lambda b: b"MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00This program cannot be run in DOS mode.")
    clone("nolic"); edit("nolic", "drvget.rec", lambda b: b.replace(b"\nname rl", b"\nname nolic"))
    edit("nolic", "rtl8139.h", lambda b: b"/* no licence information */\n" + b"\n".join(b.split(b"\n")[40:]))
    # Keep the no-licence fixture internally valid too: exercise the licence scanner,
    # not the earlier SHA-256 integrity gate.
    nolic_hash = hashlib.sha256(open(os.path.join(w, "nolic", "rtl8139.h"), "rb").read()).hexdigest().encode()
    edit("nolic", "drvget.rec", lambda b: re.sub(rb"(file rtl8139[.]h rtl8139[.]h sha256 )[0-9a-f]{64}", lambda m: m.group(1) + nolic_hash, b))
    h = hashlib.sha256()
    for n in ("drvget.rec", "rtl8139_kdrv.c", "rtl8139.c", "rtl8139.h"):          # так же, как dg_set_add в userland/drvget_core.c
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
    *_dg("drvget install bin", r"drvget: отказ: SHA-256 файла rtl8139[.]h", 90),
    *_dg("drvget install nolic", r"drvget: отказ: rtl8139[.]h: лицензия файла не найдена", 90),
    *_dg("drvget install http://10.0.2.100/rl/drvget.rec", r"разрешён только https", 20),
    {"cmd": "drv", "forbid": [r"drv-rl"]},
    *_dg("drvget install rl", r"drvget confirm rl " + _DRV_SET8, 120),
    {"log": [r"лицензия:   заявлена BSD-3-Clause", r"rtl8139\.h", r"10ec:8139", r"CAP_DMA", r"CAP_NETDEV", r"IOMMU", r"Установка НЕ выполнена"]},
    *_dg("drvget confirm rl 00000000", r"хеш не совпал", 20),
    *_dg("drvget confirm rl " + _DRV_SET8, r"установлено: /drv/rl\.kdrv и /drv/rl\.rec", 240),
    {"log": [r"собрано, образ ELF \d+ байт"]},
    *_dg("drvget list", r"rl: версия 2, права: MMIO DMA PCIDEV NETDEV, SHA-256 сверен", 20),
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


# Автоустановка драйвера BSD по идентификатору устройства: drvget скачивает по TLS рецепт и файлы OpenBSD (зеркало с теми же хешами), сверяет SHA-256 и
# лицензии, собирает всё kcc на устройстве в одну единицу трансляции, ставит после подтверждения хешем. Фаза 2 (relaunch на том же диске): карта ne2k_pci,
# драйвер из /drv находится по PCI-ID, запускается и даёт ядру DHCP.
def _bsd_ok():
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import bsdwww
        return bsdwww.build()
    except Exception as e:
        print("drvget-bsd: набор недоступен:", e, file=sys.stderr)
        return None

_BSD = _bsd_ok()
if _BSD:
    _BSD_WWW, _BSD_NAME, _BSD_SET8 = _BSD
    ALL.append(sc("drvget-bsd", "drvget: драйвер OpenBSD ne(4) по TLS (рецепт, файлы с хешами, лицензии), сборка kcc на устройстве единым модулем, установка после подтверждения; затем драйвер сам находит карту ne2k_pci и даёт ядру DHCP", [
        {"log": BOOT_LOG}, {"sleep": 6},
        *_dg("drvget base https://10.0.2.100", r"каталог рецептов теперь https://10\.0\.2\.100", 20),
        *_dg("drvget install " + _BSD_NAME, r"drvget confirm " + _BSD_NAME + " " + _BSD_SET8, 240),
        {"log": [r"лицензия:   заявлена BSD-3-Clause", r"dp8390\.c", r"if_ne_pci\.c", r"10ec:8029", r"CAP_NETDEV", r"Установка НЕ выполнена"]},
        *_dg("drvget confirm " + _BSD_NAME + " " + _BSD_SET8, r"установлено: /drv/" + _BSD_NAME + r"\.kdrv и /drv/" + _BSD_NAME + r"\.rec", 400),
        {"log": [r"собрано, образ ELF \d+ байт"]},
        *_dg("drvget verify " + _BSD_NAME, r"verify " + _BSD_NAME + ": OK", 20),
        {"log_forbid": NO_ERR},
        {"relaunch": ["-netdev", "user,id=n0", "-device", "ne2k_pci,netdev=n0"]},
        {"sleep": 8},
        {"log": [r"drvne2kio: PASS"]},                                           # первым карту берёт тестовый драйвер портов (пакет ne2kio из образа сборки)
        {"cmd": "drv stop ne2kio", "must": [r"останавливаю drv-ne2kio"]}, {"sleep": 2},
        {"cmd": "drv start " + _BSD_NAME, "must": [r"драйвер запущен как drv-" + _BSD_NAME]}, {"sleep": 8},
        {"log": [r"ne0 at pci 10ec:8029", r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"ответ от 10\.0\.2\.2", r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"log_forbid": NO_ERR + [r"bsd panic", r"bsdrt: .*FAIL"]},
    ], smp=2, mem="1G", tls_chain=True,
        extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:443-cmd:python3 " + HTTPS_ONCE + " --chain " + TLS_CHAIN + " --serve " + _BSD_WWW,
               "-device", "virtio-net-pci,netdev=n0"]))


# То же для драйвера с DMA: Intel fxp(4) собирается kcc на устройстве (в нём offsetof с неконстантным индексом), ставится после подтверждения, при загрузке
# находит карту i82559er на q35 под VT-d и даёт ядру DHCP и ping. Тестовый пакет bsdfxp из образа (тот же PCI-ID) останавливается, запускается установленный.
def _bsdf_ok():
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import bsdwww
        return bsdwww.build("fxp_pci", "fxp", out="ci-out/bsdwww-fxp")
    except Exception as e:
        print("drvget-bsd-fxp: набор недоступен:", e, file=sys.stderr)
        return None

_BSDF = _bsdf_ok()
if _BSDF:
    _BSDF_WWW, _BSDF_NAME, _BSDF_SET8 = _BSDF
    ALL.append(sc("drvget-bsd-fxp", "drvget: драйвер OpenBSD fxp(4) (bus_dma, MII) по TLS, сборка kcc на устройстве, установка, работа на q35 под VT-d: DHCP и ping", [
        {"log": BOOT_LOG}, {"sleep": 6},
        *_dg("drvget base https://10.0.2.100", r"каталог рецептов теперь https://10\.0\.2\.100", 20),
        *_dg("drvget install " + _BSDF_NAME, r"drvget confirm " + _BSDF_NAME + " " + _BSDF_SET8, 400),
        {"log": [r"лицензия:   заявлена BSD-3-Clause", r"fxp\.c", r"if_fxp_pci\.c", r"8086:1209", r"CAP_DMA", r"Установка НЕ выполнена"]},
        *_dg("drvget confirm " + _BSDF_NAME + " " + _BSDF_SET8, r"установлено: /drv/" + _BSDF_NAME + r"\.kdrv и /drv/" + _BSDF_NAME + r"\.rec", 600),
        {"log": [r"собрано, образ ELF \d+ байт"]},
        *_dg("drvget verify " + _BSDF_NAME, r"verify " + _BSDF_NAME + ": OK", 20),
        {"log_forbid": NO_ERR},
        {"relaunch": ["-device", "intel-iommu,aw-bits=48", "-netdev", "user,id=n0", "-device", "i82559er,netdev=n0,romfile="]},
        {"sleep": 8},
        {"cmd": "drv stop bsdfxp", "must": [r"останавливаю drv-bsdfxp"]}, {"sleep": 2},
        {"cmd": "drv start " + _BSDF_NAME, "must": [r"драйвер запущен как drv-" + _BSDF_NAME]}, {"sleep": 8},
        {"log": [r"\[KIRA IOMMU\] 00:03\.0 isolated: own translation domain", r"fxp0 at pci 8086:1209", r"\[KIRA NET\] DHCP: address 10\.0\.2\.15/24"]},
        {"cmd": "ping 10.0.2.2 3", "must": [r"ответ от 10\.0\.2\.2", r"отправлено 3, получено [1-3]"], "timeout": 20},
        {"log_forbid": NO_ERR + [r"bsd panic", r"bsdrt: .*FAIL", r"\[KIRA IOMMU\] DMA fault"]},
    ], smp=2, mem="1G", machine="q35", tls_chain=True,
        extra=["-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:443-cmd:python3 " + HTTPS_ONCE + " --chain " + TLS_CHAIN + " --serve " + _BSDF_WWW,
               "-device", "virtio-net-pci,netdev=n0"]))


# Окно «Драйверы» (/bin/drvman, пакет DM). Ядро чистое: окно - программа Ring 3; список PCI читается одним вызовом ядра под правом CAP_PCILIST,
# установка - тот же код, что у drvget (userland/drvget.c с -DDRVGET_LIB). Режим «drvman do КОМАНДА;...» выполняет теми же функциями то,
# что делают кнопки, и печатает те же строки, что рисуются в окне (устройства, пакеты, кадр), поэтому стенд проверяет именно показанное.
ALL.append(sc("drvman-edu", "окно «Драйверы»: список PCI из ядра (встроенный драйвер / Ring 3 запущен / драйвера нет), пакеты /drv и итог сверки (испорченный, неподтверждённый), остановка и запуск драйвера edu через менеджер ядра, отказ запуска без свободного устройства, удаление", [
    {"log": BOOT_LOG}, {"sleep": 6},
    {"log": [r"\[KIRA DRV\] edu: устройство .* 1234:11e8 -> драйвер запущен как drv-edu", r"drvedu: DMA denied: CAP_DMA not granted \(least-privilege PASS\)", r"drvedu: IRQ/MSI capability not granted \(least-privilege PASS\)", r"drvedu: PASS"]},
    *_dg("drvman do pci;pkgs", r"drvman: готово", 40),
    {"log": [r"drvman: окно создано 640x560",
             r"drvman: кадр: строк в списках [1-9]\d*, устройств [1-9]\d*, пакетов %d" % NPKG,
             r"drvman: pci \d\d:\d\d\.\d\s+1234:11e8\s+[0-9a-f]{6}\s+.*драйвер Ring 3 drv-edu запущен",
             r"drvman: pci \d\d:\d\d\.\d\s+[0-9a-f]{4}:[0-9a-f]{4}\s+[0-9a-f]{6}\s+.*встроенный драйвер ядра",
             r"drvman: pci \d\d:\d\d\.\d\s+1234:1111\s+030000\s+.*экран: буфер загрузчика, драйвер не нужен",
             r"drvman: pci \d\d:\d\d\.\d\s+[0-9a-f]{4}:[0-9a-f]{4}\s+060000\s+.*мост или шина",
             r"drvman: пакетов в /drv: %d" % NPKG,
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
    {"log": [r"drvman: пакет edu удалён", r"drvman: пакетов в /drv: %d" % (NPKG - 1)]},
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
    {"log": [r"drvman: окно создано 640x560", r"drvman: кадр: строк в списках [1-9]\d*, устройств [1-9]\d*, пакетов %d" % NPKG]},
    {"send": ""}, {"keys": "rl", "then": ["ret"]},
    {"expect": r"drvman: набор готов к установке", "timeout": 200},
    {"log": [r"drvman: ищу драйвер «rl»", r"лицензия:   заявлена BSD-3-Clause", r"SHA-256 набора исходников \(рецепт \+ файлы\): " + _DRV_SET8,
             r"10ec:8139", r"CAP_DMA", r"IOMMU", r"Установка НЕ выполнена"]},
    {"shot": "drvman-found"},
    {"keys": "", "then": ["f2"]}, {"sleep": 2},
    {"log": [r"drvman: подтверждение: установить rl v2\?", r"drvman: подтверждение: источник https://10\.0\.2\.100/rl/drvget\.rec",
             r"drvman: подтверждение: лицензия BSD-3-Clause, права: MMIO DMA PCIDEV NETDEV",
             r"drvman: подтверждение: SHA-256 набора " + _DRV_SET8 + r"[0-9a-f]{56}",
             r"drvman: подтверждение: DMA/MSI будут отклонены ядром: домен IOMMU для устройства не настроен"]},
    {"shot": "drvman-confirm"},
    {"keys": "", "then": ["esc"]}, {"sleep": 2},
    {"log": [r"drvman: установка отменена пользователем, ничего не записано"]},
    *_dg("drvget list", r"drvget: пакетов в /drv: %d " % NPKG, 30),
    {"cmd": "drv", "forbid": [r"\n  rl: версия"]},
    {"send": ""}, {"keys": "", "then": ["f2"]}, {"sleep": 2}, {"keys": "", "then": ["ret"]},
    {"expect": r"drvman: драйвер rl установлен в /drv", "timeout": 400},
    {"log": [r"drvman: подтверждено пользователем, собираю и ставлю", r"drvget: собрано, образ ELF \d+ байт"]},
    {"shot": "drvman-installed"},
    *_dg("drvget list", r"rl: версия 2, права: MMIO DMA PCIDEV NETDEV, SHA-256 сверен, издатель: (Ed25519 подтверждён|не подтверждён \(локально\)), источни(?:\r?\n\[USER pid \d+\] )?к: https://10\.0\.2\.100/rl/drvget\.rec", 30),
    *_dg("drvget verify rl", r"verify rl: OK", 30),
    {"cmd": "drv", "must": [r"rl: версия 2, SHA-256 сверен, запрошено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV, подтверждено: CAP_MMIO CAP_DMA CAP_PCIDEV CAP_NETDEV"]},
    {"send": ""}, {"keys": "", "then": ["f4"]},
    {"expect": r"drvman: драйвер rl не запущен: нет свободного подходящего устройства PCI", "timeout": 40},
    {"shot": "drvman-start-refused"},
    {"send": ""}, {"keys": "", "then": ["f3"]},
    {"expect": r"drvman: пакет rl удалён", "timeout": 40},
    {"shot": "drvman-removed"},
    {"cmd": "drv", "forbid": [r"\n  rl: версия"]},
    *_dg("drvget list", r"drvget: пакетов в /drv: %d " % NPKG, 30),
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


# Задание на сборку (этап D плана сборщика): `build` ставит задачу процессу kcc без прав, показывает хеш образа, разрешение даёт только
# человек (первые знаки SHA-256), запуск - только после разрешения и без прав; подмена образа между allow и run отклоняется.
# Эталоны контрольных сумм считает хост той же арифметикой, что и шаблоны (mind_build.c).
def _mbj_ref_probe(iters, seed):
    x = seed & 0xFFFFFFFF; acc = 0
    for i in range(iters):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        acc ^= x >> 7
        acc = (acc + i) & 0xFFFFFFFF
    return acc & 0x7FFFFFFF


def _mbj_ref_table(n, mul, mod):
    s = 0
    for i in range(n):
        s = (s + ((i * mul + 1) % mod) * (i + 1)) & 0xFFFFFFFF
    return s & 0x7FFFFFFF


_MBJ_P = _mbj_ref_probe(50000, 7)
_MBJ_T = _mbj_ref_table(64, 37, 1009)
_MBJ_REP = r"\[BUILD\] (mb%d[0-9a-f]{6}) sha256 ([0-9a-f]{64})"
ALL.append(sc("mind-build", "build: сборка процессом kcc без прав, хеш в отчёте, отказ run без разрешения, allow с неверным и верным префиксом, run без прав и контрольная сумма, подмена образа, подделка разрешения в песочнице", [
    {"log": BOOT_LOG},
    {"cmd": "build", "must": [r"build probe N SEED", r"build allow ИМЯ ХЕШ", r"решает человек"]},
    {"cmd": "build probe 0 1", "must": [r"отказ: неизвестный шаблон или параметр вне диапазона"]},
    {"cmd": "build probe 99999999999 1", "must": [r"использование: build probe N SEED"]},
    {"cmd": "build table 257 1 1", "must": [r"отказ: неизвестный шаблон или параметр вне диапазона"]},
    {"cmd": "build list", "must": [r"образов нет"]},
    # зонд: сборка в процессе kcc, отчёт с хешем
    {"send": "build probe 50000 7"},
    {"expect": _MBJ_REP % 1, "timeout": 120},
    {"capture": {"p": r"\[BUILD\] (mb1[0-9a-f]{6}) sha256", "psha": r"sha256 ([0-9a-f]{64})"}},
    {"expect": r"ожидает решения; запуска не было", "timeout": 30},
    {"log": [r"\[BUILD\] задание принято, сборка идёт в процессе kcc \(pid \d+, только CAP_FS\)", r"\[BUILD\] <<p>> размер \d+ байт, лексем \d+, арена \d+ КБ, сборка \d+ мс",
             r"\[BUILD\] <<p>> ожидает решения; запуска не было"]},
    # ядро считает хеш само: он обязан совпасть с отчётом сборщика
    {"cmd": "build list", "must": [r"<<p>>  шаблон 1  \d+ байт  sha256 <<psha>>  ожидает решения"]},
    # без разрешения запуск отклонён
    {"cmd": "build run <<p>>", "must": [r"отказ: разрешения нет"], "forbid": [r"запущен без прав", r"запускается по явной команде"]},
    # неверный и короткий префикс, чужое имя
    {"cmd": "build allow <<p>> 00000000", "must": [r"отказ: введённый хеш не совпал"], "forbid": [r"разрешён"]},
    {"cmd": "build allow <<p>> <<psha:7>>", "must": [r"отказ: введённый хеш не совпал"], "forbid": [r"разрешён"]},
    {"cmd": "build allow mb1000000 <<psha:12>>", "must": [r"отказ: нет записи о сборке или образа"]},
    {"cmd": "build allow ../bin/kcc <<psha:12>>", "must": [r"отказ: неизвестный шаблон или параметр вне диапазона"]},
    {"cmd": "build run <<p>>", "must": [r"отказ: разрешения нет"], "forbid": [r"запущен без прав"]},
    # подделка разрешения процессом/файлом песочницы: файл .ok в /data/kcc ядром не читается
    {"cmd": "write /data/kcc/<<p>>.ok forged", "must": [r"записано"]},
    {"cmd": "build run <<p>>", "must": [r"отказ: разрешения нет"], "forbid": [r"запущен без прав"]},
    # верный префикс: разрешение, затем запуск без прав и контрольная сумма
    {"cmd": "build allow <<p>> <<psha:12>>", "must": [r"<<p>> разрешён человеком"]},
    {"cmd": "build list", "must": [r"<<p>>  шаблон 1 .* разрешён человеком"]},
    {"send": "build run <<p>>"}, {"sleep": 6},
    {"log": [r"\[LOCAL\] /data/kcc/<<p>> запускается по явной команде пользователя как 'local-<<p>>' без прав",
             r"\[BUILD\] <<p>> запущен без прав \(pid \d+\)",
             r"'local-<<p>>' finished, exit code %d," % _MBJ_P]},
    # подмена образа между allow и run: ядро перечитывает образ и сверяет хеш
    {"cmd": "write /data/kcc/<<p>> tampered", "must": [r"записано"]},
    {"cmd": "build run <<p>>", "must": [r"отказ: образ изменён после сборки или разрешения"], "forbid": [r"запущен без прав", r"запускается по явной команде"]},
    {"cmd": "build allow <<p>> <<psha:12>>", "must": [r"отказ: образ изменён после сборки или разрешения"]},
    {"cmd": "build list", "must": [r"<<p>>  не годен: образ изменён после сборки или разрешения"]},
    {"cmd": "build drop <<p>>", "must": [r"<<p>> удалён вместе с записью и разрешением"]},
    {"cmd": "build run <<p>>", "must": [r"отказ: нет записи о сборке или образа"]},
    # таблица: тот же путь, другое значение; разрешение именно этого образа не переносится на другой
    {"send": "build table 64 37 1009"},
    {"expect": _MBJ_REP % 2, "timeout": 120},
    {"capture": {"t": r"\[BUILD\] (mb2[0-9a-f]{6}) sha256", "tsha": r"\[BUILD\] mb2[0-9a-f]{6} sha256 ([0-9a-f]{64})"}},
    {"cmd": "build allow <<t>> <<tsha:8>>", "must": [r"<<t>> разрешён человеком"]},
    {"send": "build run <<t>>"}, {"sleep": 6},
    {"log": [r"'local-<<t>>' finished, exit code %d," % _MBJ_T]},
    # второй зонд с другим SEED: другой образ; хеш первого к нему не подходит
    {"send": "build probe 1000 99"},
    {"expect": _MBJ_REP % 1, "timeout": 120},
    {"capture": {"q": r"\[BUILD\] (mb1[0-9a-f]{6}) sha256", "qsha": r"sha256 ([0-9a-f]{64})"}},
    {"cmd": "build allow <<q>> <<tsha:12>>", "must": [r"отказ: введённый хеш не совпал"]},
    {"cmd": "build run <<q>>", "must": [r"отказ: разрешения нет"]},
    {"cmd": "build drop <<q>>", "must": [r"удалён"]},
    {"cmd": "build drop <<t>>", "must": [r"удалён"]},
    {"cmd": "build list", "must": [r"образов нет"]},
    # сборщик (kcc) права не расширял, ядро не падало; запуск шёл только по разрешению
    {"log_forbid": NO_ERR + [r"\[KIRA CAPS\] process 'kcc'", r"\[KIRA CAPS\] process 'local-mb", r"\[BUILD\] отказ:", r"\[IMMUNE\] /data/kcc/mb"]},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G"))

# Разум только предлагает: строка-предложение в журнале, сборки нет, процесс kcc не запускался, образов нет, разрешать и запускать нечего.
ALL.append(sc("mind-build-propose", "Разум предлагает задание строкой с командой build, но сам ничего не собирает, не разрешает и не запускает", [
    {"log": BOOT_LOG},
    {"send": "ai"}, {"expect": r"\[BUILD\] Разум предлагает задание: калибровочный зонд процессора, выполнить: build probe \d+ \d+ ", "timeout": 150},
    {"cmd": "build list", "must": [r"образов нет", r"Разум предлагал \(t=\d+ с\): build probe \d+ \d+"]},
    {"cmd": "build", "must": [r"Разум предлагал"]},
    {"log_forbid": NO_ERR + [r"\[KIRA PROC\] pid \d+ 'kcc'", r"\[USER pid \d+\] \[BUILD\]", r"\[LOCAL\]", r"разрешён (человеком|подписью)"]},
    {"responsive": "status", "within": 5},
], smp=2, mem="1G"))


# ---------- А2. Доверенная загрузка по UEFI Secure Boot (docs/SECURE_BOOT.md) ----------
# Прошивка OVMF с включённым Secure Boot (ключи PK/KEK/db создаёт ci/sb_fixture.py на лету, в ci-out/secureboot, в git их нет) проверяет подпись
# BOOTX64.EFI; загрузчик проверяет подписанный манифест KIRA.SIG и хэш ядра. ESP - диск AHCI на плате q35 с SMM (так требует OVMF secboot).
# Сценарии отказа не доходят до приглашения (no_prompt): они ждут строку причины и проверяют, что ядро НЕ стартовало.
_SB = dict(machine="q35,smm=on", smp=2, mem="1G", needs=["secureboot"])
_SB_NO_KERNEL = [r"\[KIRA OS\] Kernel core starting", r"\[KIRA UEFI\] Common BootInfo accepted", r"\[KIRA CONSOLE\] Command line ready",
                 r"KIRA UEFI: handing off to kernel", r"trusted boot: signature verified"]

ALL.append(sc("secureboot-ok", "Secure Boot включён, образ подписан: прошивка проверила загрузчик, загрузчик - манифест и хэш ядра, ядро сообщает состояние цепочки", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] Secure Boot: enabled \(firmware verified the signature of this loader\)",
             r"\[KIRA BOOT\] trusted mode: the kernel must be signed with the release key embedded in this loader",
             r"\[KIRA BOOT\] kernel KIRA\.ELF: SHA-256 and size match the signed manifest",
             r"\[KIRA BOOT\] TPM: not present, boot is not measured",
             r"\[KIRA BOOT\] trusted boot: signature verified, handing off to the kernel",
             # строки самого ядра: состояние пришло из заголовка BootInfo, а не из предположения
             r"\[KIRA BOOT\] Secure Boot: enabled\r?\n",
             r"\[KIRA BOOT\] Image signature: verified by the loader \(boot files match the signed manifest\), release 3\r?\n",
             r"\[KIRA BOOT\] Measured boot \(TPM PCR 9\): no",
             r"\[KIRA BOOT\] Trusted boot: YES"]},
    {"cmd": "test mem", "must": [r"heap: PASS", r"PMM: PASS", r"VMM: PASS"]},
    {"log_forbid": NO_ERR + [r"TRUSTED BOOT REFUSED", r"Trust chain: the loader reports none"]},
], secureboot={"image": "good", "vars": "secboot"}, boot_timeout=240, **_SB))

ALL.append(sc("secureboot-tamper-loader", "загрузчик изменён на 1 байт после подписи: прошивка с Secure Boot его отвергает, загрузчик и ядро не запускаются", [
    {"expect": r"Access Denied", "timeout": 120},
    {"sleep": 10},
    {"log_forbid": _SB_NO_KERNEL + [r"KIRA OS UEFI loader", r"\[KIRA BOOT\]"]},
], secureboot={"image": "tamper-loader", "vars": "secboot"}, no_prompt=True, **_SB))

ALL.append(sc("secureboot-unsigned-loader", "загрузчик без подписи ключа db: прошивка с Secure Boot его отвергает", [
    {"expect": r"Access Denied", "timeout": 120},
    {"sleep": 10},
    {"log_forbid": _SB_NO_KERNEL + [r"KIRA OS UEFI loader", r"\[KIRA BOOT\]"]},
], secureboot={"image": "unsigned-loader", "vars": "secboot"}, no_prompt=True, **_SB))

ALL.append(sc("secureboot-tamper-kernel", "подпись загрузчика верна, ядро изменено на 1 байт: загрузчик отказывается и пишет причину на экран и в COM1, ядро не стартует", [
    {"expect": r"\[KIRA BOOT\] Secure Boot: enabled \(firmware verified the signature of this loader\)", "timeout": 120},
    {"expect": r"\[KIRA BOOT\] TRUSTED BOOT REFUSED: the kernel is NOT started", "timeout": 120},
    {"expect": r"\[KIRA BOOT\]   object: \\EFI\\KIRA\\KIRA\.ELF", "timeout": 20},
    {"expect": r"\[KIRA BOOT\]   reason: file was modified: SHA-256 differs from the signed manifest", "timeout": 20},
    {"sleep": 8},
    {"log": [r"KIRA BOOT: TRUSTED BOOT REFUSED: the kernel is NOT started"]},         # то же сообщение на экране (консоль вывода прошивки)
    {"log_forbid": _SB_NO_KERNEL},
], secureboot={"image": "tamper-kernel", "vars": "secboot"}, no_prompt=True, **_SB))

ALL.append(sc("secureboot-no-manifest", "подписанный загрузчик, но манифеста KIRA.SIG нет: загрузка отклонена, а не продолжена без проверки", [
    {"expect": r"\[KIRA BOOT\] TRUSTED BOOT REFUSED: the kernel is NOT started", "timeout": 120},
    {"expect": r"\[KIRA BOOT\]   reason: signed manifest is missing: an unsigned image is not booted", "timeout": 20},
    {"sleep": 8},
    {"log_forbid": _SB_NO_KERNEL},
], secureboot={"image": "no-manifest", "vars": "secboot"}, no_prompt=True, **_SB))

ALL.append(sc("secureboot-rollback", "манифест выпуска 3, а загрузчик допускает выпуски не ниже 5: откат отклонён (поле номера выпуска для А3)", [
    {"expect": r"\[KIRA BOOT\] TRUSTED BOOT REFUSED: the kernel is NOT started", "timeout": 120},
    {"expect": r"\[KIRA BOOT\]   reason: image version is older than the allowed minimum \(rollback refused\)", "timeout": 20},
    {"sleep": 8},
    {"log_forbid": _SB_NO_KERNEL},
], secureboot={"image": "rollback", "vars": "secboot"}, no_prompt=True, **_SB))

ALL.append(sc("secureboot-required", "прошивка без ключей (Secure Boot выключен), а загрузчик доверенный: цепочка не начинается с прошивки, загрузка отклонена", [
    {"expect": r"\[KIRA BOOT\] Secure Boot: disabled", "timeout": 120},
    {"expect": r"\[KIRA BOOT\] TRUSTED BOOT REFUSED: the kernel is NOT started", "timeout": 120},
    {"expect": r"\[KIRA BOOT\]   reason: Secure Boot is not enabled with enrolled keys", "timeout": 20},
    {"sleep": 8},
    {"log_forbid": _SB_NO_KERNEL},
], secureboot={"image": "good", "vars": "plain"}, no_prompt=True, **_SB))

# Измеряемая загрузка (этап 2): эмулируемый TPM 2.0 (swtpm). Загрузчик вносит манифест и ядро в PCR 9 и печатает значение, прочитанное из самого TPM;
# проверка независимая: ожидаемое значение хост считает по файлам образа (ci/sb_fixture.py: H(PCR || H(данные)) по порядку).
ALL.append(sc("secureboot-measured", "доверенная загрузка с эмулируемым TPM 2.0: PCR 9 равен расчёту по манифесту и ядру, ядро сообщает, что загрузка измерена", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] TPM: PCR 9 \(SHA-256\) = <<pcr9>>\r?\n",
             r"\[KIRA BOOT\] TPM: boot payload measured into PCR 9",
             r"\[KIRA BOOT\] Measured boot \(TPM PCR 9\): yes",
             r"\[KIRA BOOT\] Trusted boot: YES"]},
    {"log_forbid": NO_ERR + [r"TRUSTED BOOT REFUSED", r"TPM: measurement failed", r"TPM: PCR 9 could not be read back"]},
], secureboot={"image": "good", "vars": "secboot", "tpm": True}, boot_timeout=240, **dict(_SB, needs=["secureboot", "swtpm"])))


# ---------- А3. Защита от отката и подмены (docs/SECURE_BOOT.md, раздел «Откат»; docs/PLAN.md, А3) ----------
# 1. Откат образа: монотонный счётчик выпуска в эмулируемом TPM (swtpm). Состояние TPM лежит в ci-out/tpm-state-<имя> и переживает перезапуски машины
#    внутри сценария (как NV-память настоящего TPM); перед сценарием оно стирается. Загрузчик сверяет выпуск из подписанного манифеста со счётчиком,
#    ядро поднимает счётчик после подтверждённой «хорошей» загрузки (20 секунд работы).
ALL.append(sc("secureboot-rollback-tpm", "выпуск 5 грузится и поднимает счётчик TPM, затем выпуск 3 с верной подписью отвергается; счётчик пережил перезапуск; выпуск 5 грузится снова", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] rollback: TPM release counter created \(first use of this TPM\)",
             r"\[KIRA BOOT\] rollback: release 5 is not older than the TPM counter 0 \(the kernel raises the counter after a confirmed good boot\)",
             r"\[KIRA BOOT\] Image signature: verified by the loader \(boot files match the signed manifest\), release 5\r?\n",
             r"\[KIRA BOOT\] rollback: TPM 2\.0 \(TIS\) found; release 5, TPM counter 0",
             r"\[KIRA BOOT\] Trusted boot: YES"]},
    {"expect": r"\[KIRA BOOT\] rollback: good boot confirmed, TPM counter is now 5\r?\n", "timeout": 180},
    # выпуск 3: подпись манифеста верна (тот же ключ выпуска), но счётчик TPM уже 5
    {"relaunch": [], "esp": "good", "prompt": False, "from_start": True},
    {"expect": r"\[KIRA BOOT\] rollback: release 3 is older than the TPM counter 5 \(rollback refused\)", "timeout": 240},
    {"expect": r"\[KIRA BOOT\] TRUSTED BOOT REFUSED: the kernel is NOT started", "timeout": 30},
    {"expect": r"\[KIRA BOOT\]   reason: release is older than the TPM rollback counter \(rollback refused\)", "timeout": 30},
    {"sleep": 8},
    {"forbid_since": _SB_NO_KERNEL},
    # счётчик пережил перезапуск, а законный выпуск 5 по-прежнему грузится (равный счётчику допустим)
    {"relaunch": [], "esp": "release5", "hard": True, "from_start": True},
    {"expect": r"\[KIRA BOOT\] rollback: release 5 is not older than the TPM counter 5 ", "timeout": 30},
    {"log_forbid": NO_ERR},
], secureboot={"image": "release5", "vars": "secboot", "tpm": True, "tpm_state": "rollback"}, boot_timeout=300, **dict(_SB, needs=["secureboot", "swtpm"])))

# Без TPM защита образа - только вшитый порог, и журнал говорит об этом прямо (а не молчит).
ALL.append(sc("secureboot-rollback-no-tpm", "доверенная загрузка без TPM: прежнее поведение (порог KIRA_MIN_IMAGE_VERSION), в журнале честная строка, что счётчика нет", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] rollback: no TPM counter: only the embedded minimum release 0 protects from rollback; release 3",
             r"\[KIRA BOOT\] rollback: no TPM driver device: image rollback is checked only against the embedded minimum release; disk rollback is not detected",
             r"\[KIRA BOOT\] Trusted boot: YES"]},
    {"cmd": "fsanchor", "must": [r"fsanchor: TPM absent"]},
    {"log_forbid": NO_ERR + [r"TRUSTED BOOT REFUSED"]},
], secureboot={"image": "good", "vars": "secboot"}, boot_timeout=240, **_SB))

# 2. Откат диска: эпоха тома KiraFS и якорь в TPM. Старая копия диска (снимок до последних записей) ставится на место текущего - том открывается только для чтения.
ALL.append(sc("fs-rollback-tpm", "эпоха диска KiraFS закрепляется в TPM; подмена диска старой копией обнаруживается при монтировании, том только для чтения, оператор подтверждает", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] rollback: TPM 2\.0 \(TIS\) found; boot is not signed, the release counter is not used",
             r"\[KIRA FS\] anchor: none in TPM yet, disk epoch \d+ is taken as the starting point"]},
    {"cmd": "write /old.txt first", "must": [r"."]},
    {"cmd": "fsanchor sync", "must": [r"\[KIRA FS\] anchor: disk epoch \d+ saved in TPM \(command\)", r"fsanchor: done"]},
    {"cmd": "fsanchor", "must": [r"TPM present", r"TPM anchor \d+"]},
    {"relaunch": [], "save_disk": "old", "from_start": True},                        # выключение: эпоха и якорь на носитель; снимок диска = «старая копия»
    {"expect": r"\[KIRA FS\] anchor: disk epoch (\d+) is not older than the TPM anchor \1: ok", "timeout": 10},
    {"cmd": "write /new.txt second", "must": [r"."]},
    {"cmd": "cat /new.txt", "must": [r"second"]},
    {"relaunch": [], "restore_disk": "old", "from_start": True},                    # выключение закрепило новую эпоху; затем на место диска ставится старая копия
    {"expect": r"\[KIRA FS\] rollback detected: disk epoch \d+ is older than the TPM anchor \d+: an old copy of the disk was put in place\. The volume is READ-ONLY", "timeout": 10},
    {"capture": {"ep": r"rollback detected: disk epoch (\d+) is older", "an": r"is older than the TPM anchor (\d+): an old copy"}},
    {"num": ["an", "ep", 1, 100000]},
    {"cmd": "cat /old.txt", "must": [r"first"]},                                      # читать можно
    {"cmd": "cat /new.txt", "forbid": [r"second"]},                                   # в старой копии новых данных нет
    {"cmd": "write /x.txt blocked", "must": [r"только для чтения"]},                  # писать нельзя: безопасный режим
    {"cmd": "fsanchor", "must": [r"READ-ONLY \(rollback\)"]},
    {"cmd": "fsanchor sync", "must": [r"rollback detected"]},
    {"cmd": "fsanchor accept", "must": [r"operator accepted the disk", r"volume is writable"]},
    {"cmd": "write /x.txt allowed", "must": [r"."]},
    {"cmd": "cat /x.txt", "must": [r"allowed"]},
    {"cmd": "fsanchor sync", "must": [r"fsanchor: done"]},
    {"cmd": "fsanchor", "must": [r"TPM anchor \d+"], "forbid": [r"READ-ONLY"]},
    {"log_forbid": NO_ERR},
], smp=2, mem="1G", tpm=True, tpm_state="fsrollback", needs=["swtpm"], boot_timeout=120))

ALL.append(sc("fs-anchor-no-tpm", "без TPM защита диска от отката недоступна, и ядро говорит об этом строкой в журнале и командой fsanchor", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] rollback: no TPM driver device: image rollback is checked only against the embedded minimum release; disk rollback is not detected",
             r"\[KIRA FS\] anchor: no TPM, a rolled-back disk copy would not be detected"]},
    {"cmd": "fsanchor", "must": [r"fsanchor: TPM absent"]},
    {"cmd": "write /a.txt works", "must": [r"."]}, {"cmd": "cat /a.txt", "must": [r"works"]},       # без TPM запись работает как раньше
    {"log_forbid": NO_ERR + [r"rollback detected"]},
], smp=2, mem="1G"))

# 3. Подпись приложений: манифест /etc/apps.manifest подписан ключом издателя (/etc/apps.sig, домен APPS). Нужно ядро с тестовыми ключами (KIRA_TEST_TRUST=1:
#    сборка в отдельном каталоге; метка kira.testtrust), боевое ядро доверяет только офлайн-ключу издателя. Допуск по подписи включает строка ядра kira.signedapps
#    (образ kira-bios-signedapps.img) или доверенная загрузка (там подпись обязательна всегда).
_APPS_PUT = {"/etc/apps.manifest": "{apps_manifest}", "/etc/apps.sig": "{apps_sig_a}"}
_DEMO_OK = r"data page intact after 3 sleeps: address spaces are isolated \(PASS\)"
_APPS = dict(boot="bios", bios_img="kira-bios-signedapps.img", smp=2, mem="1G", apps=True, needs=["testtrust"], boot_timeout=120)

ALL.append(sc("apps-signed-ok", "манифест приложений с верной подписью издателя: программа запускается и допускается по подписанному манифесту", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA APPS\] policy: signed application manifest required \(kira\.signedapps, development boot\)"]},
    {"cmd": "run demo"}, {"sleep": 6},
    {"log": [_DEMO_OK]},
    {"cmd": "progs", "must": [r"demo\s+доверенная"]},
    {"log_forbid": NO_ERR + [r"launch refused", r"Запуск отклонён"]},
], put=_APPS_PUT, **_APPS))

ALL.append(sc("apps-signed-tampered", "манифест приложений подменён после подписи: подпись не принимается, программа не запускается", [
    {"log": BOOT_LOG},
    {"cmd": "run demo"}, {"sleep": 4},
    {"log": [r"\[PROG\] /bin/demo: signed application manifest: manifest signature rejected: хеш пакета не совпал с манифестом; launch refused",
             r"\[IMMUNE\] /bin/demo: подписанный манифест не принят\. Запуск отклонён"]},
    {"cmd": "progs", "must": [r"demo\s+чужая"]},
    {"log_forbid": NO_ERR + [_DEMO_OK]},
], put={"/etc/apps.manifest": "{apps_manifest_bad}", "/etc/apps.sig": "{apps_sig_a}"}, **_APPS))

ALL.append(sc("apps-signed-missing", "в режиме подписи манифеста приложений нет вообще: программы не запускаются (встроенного манифеста для обхода нет)", [
    {"log": BOOT_LOG},
    {"cmd": "run demo"}, {"sleep": 4},
    {"log": [r"\[PROG\] /bin/demo: signed application manifest: signed manifest /etc/apps\.manifest is missing; launch refused"]},
    {"log_forbid": NO_ERR + [_DEMO_OK]},
], **_APPS))

ALL.append(sc("apps-signed-revoked", "манифест подписан новым ключом (ротация): запускается; ключ отозван командой drv revoke - отказ; ключ A по-прежнему принимается", [
    {"log": BOOT_LOG},
    {"send": "run demo"}, {"expect": _DEMO_OK, "timeout": 30},                               # ключ TEST-ED25519-002 в таблице ядра: принят
    {"cmd": "drv revoke TEST-ED25519-002", "must": [r"."]},
    {"cmd": "run demo"}, {"sleep": 4},
    {"log": [r"\[PROG\] /bin/demo: signed application manifest: manifest signature rejected: издательский ключ отозван; launch refused"]},
    {"log_forbid": NO_ERR},
], put={"/etc/apps.manifest": "{apps_manifest}", "/etc/apps.sig": "{apps_sig_b}"}, **_APPS))

ALL.append(sc("apps-dev-mode", "режим разработки (без kira.signedapps и без доверенной загрузки): допуск по встроенному манифесту SHA-256 как раньше, подпись не требуется и подменённый манифест на диске ничего не меняет", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA APPS\] policy: development mode, programs are admitted by the SHA-256 manifest built into the kernel \(signature not required\)"]},
    {"cmd": "run demo"}, {"sleep": 6},
    {"log": [_DEMO_OK]},
    {"cmd": "progs", "must": [r"demo\s+доверенная"]},
    {"log_forbid": NO_ERR + [r"launch refused", r"Запуск отклонён"]},
], put={"/etc/apps.manifest": "{apps_manifest_bad}", "/etc/apps.sig": "{apps_sig_a}"}, smp=2, mem="1G", apps=True, needs=["testtrust"]))

# В доверенной загрузке подпись манифеста приложений обязательна без всякой строки ядра (ядро с тестовыми ключами и ESP из kira.bin этой же сборки).
ALL.append(sc("secureboot-apps", "доверенная загрузка: программа с верно подписанным манифестом запускается", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA APPS\] policy: signed application manifest required \(trusted boot\)", r"\[KIRA BOOT\] Trusted boot: YES"]},
    {"cmd": "run demo"}, {"sleep": 8},
    {"log": [_DEMO_OK]},
    {"log_forbid": NO_ERR + [r"launch refused"]},
], put=_APPS_PUT, apps=True, secureboot={"image": "good", "vars": "secboot"}, boot_timeout=240, **dict(_SB, needs=["secureboot", "testtrust"])))

ALL.append(sc("secureboot-apps-unsigned", "доверенная загрузка без подписанного манифеста: программа не запускается, обхода через встроенный манифест нет", [
    {"log": BOOT_LOG},
    {"log": [r"\[KIRA BOOT\] Trusted boot: YES", r"\[KIRA APPS\] policy: signed application manifest required \(trusted boot\)"]},
    {"cmd": "run demo"}, {"sleep": 6},
    {"log": [r"\[PROG\] /bin/demo: signed application manifest: signed manifest /etc/apps\.manifest is missing; launch refused"]},
    {"log_forbid": NO_ERR + [_DEMO_OK]},
], apps=True, secureboot={"image": "good", "vars": "secboot"}, boot_timeout=240, **dict(_SB, needs=["secureboot", "testtrust"])))
