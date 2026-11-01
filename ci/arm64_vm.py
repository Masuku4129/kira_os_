#!/usr/bin/env python3
"""Стенд ARM64: QEMU virt (эмуляция TCG), последовательный порт на stdio. Каждый сценарий - машина с другим
железом (GICv2/GICv3, число ядер, ОЗУ, процессор, вход на EL1 или EL2); ядро обязано найти всё по дереву устройств.
Итог - ci-out/arm64-<имя>/serial.log и result.json."""
import json, os, re, select, shutil, socket, struct, subprocess, sys, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfiles import SUMS as mkfiles_sums

READY = r"\[KIRA ARM64\] Ready\."
COMMON = [r"MMU on: identity map", r"generic timer verified: ([5-9]\d|1[0-4]\d) ticks in 200 ms",
          r"memory manager: \d+ MB free pages; kernel heap and scheduler online"]
NO_ERR = [r"FATAL", r"exception on core", r"did not come up", r"CPU_ON .* failed", r"KERNEL PANIC", r"RESULT: FAIL", r"no thread slot", r"process terminated", r"FAIL"]

SC = [
    dict(name="arm64-gicv3", m="virt,gic-version=3", cpu="cortex-a72", smp=4, mem="1G", disk="virtio-blk-pci",
         must=[r"GICv3", r"Processors online: 4 of 4 \(PSCI via hvc\)", r"working timer interrupt: 4 of 4", r"entered at EL1", r"RAM: 1024 MB",
               r"\[KIRA PCI\] ECAM at .*BAR\(s\) assigned", r"virtio-blk", r"\[KIRA FS\] KiraFS mounted", r"\[KIRA MIND\] Mind online", r"\[KIRA SYSCALL\] SVC from EL0",
               r"\[KIRA GIC\] ITS at .*PCI interrupts by message", r"virtio-blk, .*completion by MSI-X interrupt"],
         net=True,
         cmds=[("smp", [r"core 3: MPIDR 0x0+3, online, ticks [1-9]"]),
               ("mem", [r"RAM 0x0+40000000 \.\. 0x0+80000000 \(1024 MB\)", r"kernel heap: \d+ bytes"]),
               ("test smp", [r"RESULT: PASS", r"preempted by tick", r"kernel mutex .*: 8000 of 8000 ok"]),
               ("ps", [r"core\s+RUNNING", r"idle", r"mind-tact"]),
               ("disk", [r"\[системный\]", r"прерываний устройств \(MSI\): [1-9]"]), ("df", [r"KiraFS"]),
               ("write /hello.txt kira-arm64", []), ("cat /hello.txt", [r"kira-arm64"]), ("ls /", [r"hello.txt"]),
               ("ai", [r"предвидение: риск опоздания экрана", r"здоровье \d+"]), ("ai model", [r"обучено на \d+ с"]),
               ("ai cap 70", [r"70%"]),
               ("ls /bin", [r"mt", r"ipc"]),
               ("mt", [r"mt: .* - PASS"]),
               ("ipc", []), ("ipc", [r"ipc server: .* - PASS", r"ipc client: .* - PASS"]),
               ("ps", [r"core\s+RUNNING"]),
               ("help", [r"status", r"hw", r"progs", r"telemetry"]),            # общая консоль - те же команды, что у x86-64
               ("status", [r"interrupts: GICv3"]), ("hw", [r"\[KIRA HW\] CPU MIDR"]), ("progs", [r"mt", r"edit"]),
               ("net", [r"канал есть", r"адрес 10\.0\.2\.15"]), ("ping 10.0.2.2 3", [r"ответ от 10\.0\.2\.2", r"получено [1-3]"]),
               ("http 10.0.2.100 /arm", [r"kira-tcp-ok GET /arm HTTP/1\.0", r"получено 3\d\d\d байт .*сервер закрыл соединение"]),
               ("dns example.com", [r"example\.com -> \d+\.\d+\.\d+\.\d+"]),
               ("fetch 10.0.2.100 /prog", [r"fetch: 10\.0\.2\.100/prog - HTTP/1\.0 200 OK - 3\d\d\d bytes .*server closed"]),
               ("cat /data/fetch/last.txt", [r"kira-tcp-ok GET /prog HTTP/1\.0"]),
               ]),
    dict(name="arm64-gicv2", m="virt,gic-version=2", cpu="cortex-a53", smp=2, mem="512M", disk="nvme", net="virtio",
         must=[r"GICv2", r"Processors online: 2 of 2", r"working timer interrupt: 2 of 2", r"RAM: 512 MB",
               r"\[KIRA NVMe\] nvme0n1", r"\[KIRA FS\] KiraFS mounted", r"NVMe .*completion by interrupt"],
         cmds=[("smp", [r"core 1: .*online"]), ("test smp", [r"RESULT: PASS"]),
               ("write /irq.txt kira-v2m", []), ("cat /irq.txt", [r"kira-v2m"]),
               ("disk", [r"прерываний устройств \(MSI\): [1-9]"]),
               ("@sleep 4", []), ("net", [r"карта virtio-net, канал есть", r"адрес 10\.0\.2\.15"]),
               ("fetch 10.0.2.100 /v2m", [r"fetch: 10\.0\.2\.100/v2m - HTTP/1\.0 200 OK - 3\d\d\d bytes .*server closed"]),
               ("fetch https://10.0.2.100:8443/self", [r"fetch: TLS failed: issuer is not trusted"]),
               ("fetch https://10.0.2.100/arm-tls", [r"fetch: TLS 1\.3, X25519 \+ ChaCha20-Poly1305; certificate verified \(chain to a root, name, dates\)",
                 r"fetch: 10\.0\.2\.100/arm-tls - HTTP/1\.0 200 OK - 2\d\d\d\d bytes .*server closed"]),
               ("random", [r"источники - .*interrupt timing", r"проба: [0-9a-f]{32}"]),
               ("udpecho 9000 1", [r"udpecho: listening on UDP port 9000"]),
               ("@udp 18083 arm-ping", [r"\[HOST\] udp reply 'kira-udp: arm-ping'"]), ("@sleep 2", []),
               ("serve", [r"serve: listening on port 8080"]),
               ("@http 18081 /arm-server", [r"\[HOST\] http /arm-server: 19\d\d\d bytes, first line 'kira-server-ok /arm-server', window lines 900"])]),
    dict(name="arm64-el2-8cpu", m="virt,gic-version=3,virtualization=on", cpu="cortex-a57", smp=8, mem="3G",
         must=[r"entered at EL2, running at EL1", r"Processors online: 8 of 8", r"working timer interrupt: 8 of 8", r"RAM: 3072 MB"],
         cmds=[("test smp", [r"RESULT: PASS", r"worker 8: core 0"])]),
    # Экран и ввод как в Limbo ARM: virtio-gpu, virtio-клавиатура и планшет. Команды "@keys ..." набираются на
    # клавиатуре virtio (QMP send-key), "@tap X Y" - касание планшета (0..32767), "@shot имя" - снимок экрана.
    dict(name="arm64-gui", m="virt,gic-version=3", cpu="cortex-a72", smp=4, mem="1G", disk="virtio-blk-pci", gui=True, files=True,
         must=[r"\[KIRA GPU\] virtio-gpu: \d+x\d+ 2D scanout", r"\[KIRA INPUT\] virtio-input: .*[Kk]eyboard.*, MSI-X interrupt",
               r"\[KIRA INPUT\] virtio-input: .*\(absolute pointer\), MSI-X interrupt", r"\[KIRA DISPLAY\] Shell online",
               r"\[KIRA RTC\] Real-time clock \(PL031, UTC\): "],
         cmds=[("@sleep 3", []), ("@shot desktop", []),
               ("@keys smp", [r"core 3: MPIDR 0x0+3, online"]),              # клавиатура virtio -> консоль
               ("@tap 16000 16000", []),
               ("plasma", []), ("cube3d", []), ("@sleep 10", []), ("@shot apps", []),
               ("ps", [r"plasma", r"cube3d", r"display"]),
               ("ai", [r"композитор: ", r"тактика"]),
               ("beep 440 300", [r"DMA прошёл [1-9]\d* байт - карта играет"]),
               ("melody", [r"melody: 8 notes, 76800 frames queued"]), ("sound", [r"от программ смешано (7[6-9]|[89]\d)\d\d\d"]),
               ("melody 22050", [r"melody: 8 notes, 35280 frames queued at 22050 Hz stereo"]), ("@sleep 2", []),
               ("sound", [r"от программ смешано 15[2-4]\d\d\d"]),
               ("record 200", [r"запись: (microphone|line in), 9600 кадров за \d+ мс"]),
               ("edit", []), ("@sleep 3", []), ("@type hello kira", []), ("@key f2", []), ("@sleep 2", []), ("@shot edit", []),
               ("cat /data/edit/note.txt", [r"hello kira"]),
               ("view /pic.png", [r"view: /pic\.png - PNG 97x61, .* sum %08x" % mkfiles_sums["/pic.png"]]), ("@sleep 2", []), ("@shot view", []),
               ("ps", [r"core\s+RUNNING"])]),
    # Изоляция, как сценарий isolation у x86-64: враждебные программы останавливаются, ядро живо
    dict(name="arm64-isolation", m="virt,gic-version=3", cpu="cortex-a72", smp=4, mem="1G", disk="virtio-blk-pci",
         allow=[r"process terminated", r"exception on core"],
         must=[r"Processors online: 4 of 4"],
         cmds=[("test isolation", [r"address spaces are isolated \(PASS\)", r"address spaces are isolated \(PASS\)[\s\S]*address spaces are isolated \(PASS\)"]),
               ("test atk", [r"kernel pointer passed to write\(\) was rejected \(PASS\)", r"jumping into kernel memory"]),
               ("@sleep 3", []),
               ("test fpu", [r"fpu: rounds \d+, mismatches 0 - PASS[\s\S]*- PASS[\s\S]*- PASS"]),
               ("test threads", [r"mt: .* - PASS"]),
               ("status", [r"interrupts: GICv3"]),
               ("wasm /wasm/wtest.wasm", [r"wtest:\ fib\(24\)=46368\ primes<20000=2262\ sort=ad642057dcc4b702\ switch=4148393079\ ops=ba422c8d525a9aea\ sext=269715", r"wasm: finished with code 0"]),
               ("wasm /wasm/ftest.wasm", [r"ftest: mandel inside=662 iters=146978 pi=400921f61616ca3a f32sum=000000004004dfe3 hash=225e25f3d90cf161"])]),
    dict(name="arm64-fault", m="virt,gic-version=3", cpu="cortex-a72", smp=1, mem="256M",
         must=[r"Processors online: 1 of 1"],
         cmds=[("fault", [r"data abort", r"FAR 0x0000600000000000"])], fault=True, allow=[r"kernel-mode fault"]),
    dict(name="arm64-crash", m="virt,gic-version=3", cpu="cortex-a72", smp=4, mem="512M",
         allow=[r"kernel-mode fault"], must=[r"Processors online: 4 of 4"],
         cmds=[("test crash thread", [r"\[KIRA CORE \d+\] kernel-mode fault \(data abort\) at PC 0x[0-9A-Fa-f]+, FAR 0x0000600000000000, thread 'smpcrash'\. Thread terminated"]),
               ("test crash 1", [r"cannot raise SError from software"]),
               ("test smp", [r"PASS"]),
               ("status", [r"interrupts: GICv3"])]),
    dict(name="arm64-overflow", m="virt,gic-version=3", cpu="cortex-a72", smp=2, mem="512M", fault=True,
         allow=[r"stack overflow"], must=[r"Processors online: 2 of 2"],
         cmds=[("test crash stack", [r"Kernel thread stack overflow: guard area hit at SP 0x00007[0-9A-Fa-f]+ \(PC 0x[0-9A-Fa-f]+\), now on the spare stack",
                                     r"Not isolated: kernel thread stack overflow"])]),
    dict(name="arm64-ramfb", m="virt,gic-version=3", cpu="cortex-a72", smp=2, mem="512M", qmp=True, shot_size=(1024, 768),
         extra=["-device", "ramfb", "-device", "virtio-keyboard-pci"],
         must=[r"\[KIRA FB\] ramfb: 1024x768 XRGB8888 frame in RAM, configured through fw_cfg DMA", r"\[KIRA DISPLAY\] Shell online",
               r"\[KIRA INPUT\] virtio-input: .*, MSI-X interrupt"],
         cmds=[("@sleep 3", []), ("@shot ramfb", [])]),
    dict(name="arm64-usb", m="virt,gic-version=3", cpu="cortex-a72", smp=2, mem="512M", disk="virtio-blk-pci",
         extra=["-device", "qemu-xhci,id=xhci", "-device", "usb-hub,bus=xhci.0,port=1",
                "-device", "usb-kbd,bus=xhci.0,port=1.2", "-device", "usb-tablet,bus=xhci.0,port=2",
                "-device", "usb-storage,bus=xhci.0,port=3,drive=stick,removable=on"], flash=True,
         must=[r"Processors online: 2 of 2"],
         cmds=[("@sleep 4", []),
               ("usb", [r"xHCI", r"хаб", r"клавиатура", r"планшет"]),
               ("df", [r"usb0"]),
               ("cat /usb0/hello.txt", [r"kira-ci"]),
               ("cp /bin/plasma /usb0/plasma.bin", []),
               ("ls /usb0", [r"plasma\.bin"]),
               ("umount usb0", [])]),
    dict(name="arm64-power", m="virt,gic-version=3", cpu="cortex-a72", smp=2, mem="512M", disk="virtio-blk-pci",
         must=[r"Processors online: 2 of 2"],
         cmds=[("write /reboot.txt kira-before-reboot", []),
               ("reboot", [r"\[KIRA PSCI\] rebooting \(SYSTEM_RESET\)", READY]), ("@sleep 2", []),
               ("cat /reboot.txt", [r"kira-before-reboot"]),
               ("@exit poweroff", [r"\[KIRA PSCI\] powering off \(SYSTEM_OFF\)"])]),
]


def ppm_to_png(ppm, png):
    """PPM (P6) от screendump -> PNG без сторонних библиотек."""
    data = open(ppm, "rb").read()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    px = parts[4][: w * h * 3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    open(png, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    return w, h, len({px[i:i + 3] for i in range(0, len(px), 3 * 97)})   # размер и число разных цветов (выборка)


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); break
            except OSError:
                time.sleep(0.1)
        self.s.settimeout(10); self.buf = b""
        self.read(); self.cmd("qmp_capabilities")

    def read(self):
        while b"\n" not in self.buf: self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **a):
        self.s.sendall(json.dumps({"execute": name, "arguments": a} if a else {"execute": name}).encode() + b"\n")
        while True:
            m = self.read()
            if "return" in m or "error" in m: return m


def host_http(port, path):
    """Запрос с машины-хозяина к серверу в гостевой системе (hostfwd): строка итога для журнала."""
    import urllib.request
    try:
        body = urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=30).read()
        first = body.split(b"\n", 1)[0].decode(errors="replace")
        lines = body.count(b" kira tcp window\n")
        return f"[HOST] http {path}: {len(body)} bytes, first line '{first}', window lines {lines}\n"
    except Exception as e:
        return f"[HOST] http {path}: error {e}\n"


QCODE = {" ": "spc", "\r": "ret", "-": "minus", "/": "slash", ".": "dot"}


def run(sc, out):
    d = os.path.join(out, sc["name"]); os.makedirs(d, exist_ok=True)
    args = ["qemu-system-aarch64", "-M", sc["m"], "-cpu", sc["cpu"], "-smp", str(sc["smp"]), "-m", sc["mem"],
            "-display", "none", "-monitor", "none", "-serial", "stdio", "-kernel", "kira-arm64.img",
            "-d", "guest_errors,unimp", "-D", os.path.join(d, "qemu-debug.log")]
    if sc.get("disk"):                              # свежая копия диска с KiraFS на каждую машину
        img = os.path.join(d, "disk.img"); shutil.copyfile("kira-arm64.disk", img)
        if sc.get("files"):                         # тестовые файлы (ci/mkfiles.py) - на диск машины
            sys.path.insert(0, os.path.dirname(__file__)); import mkfiles
            for dst, src in mkfiles.make(os.path.join(d, "files")).items():
                subprocess.run(["tools/kfstool", img, "put", dst, src], check=True, stdout=subprocess.DEVNULL)
        if sc.get("net"):                           # тестовая цепочка TLS: её корневой - единственный доверенный на этом диске
            chain = os.path.abspath("ci-out/tlschain")
            subprocess.run([sys.executable, "-I", "ci/mkchain.py", chain], check=True, stdout=subprocess.DEVNULL)
            subprocess.run(["tools/kfstool", img, "put", "/etc/ssl/roots.der", os.path.join(chain, "root.der")], check=True, stdout=subprocess.DEVNULL)
        args += ["-drive", f"file={img},if=none,id=d0,format=raw"]
        args += ["-device", "nvme,drive=d0,serial=KIRA0001" if sc["disk"] == "nvme" else f"{sc['disk']},drive=d0"]
    if sc.get("net"):
        srv = os.path.abspath(os.path.join(os.path.dirname(__file__), "http_once.py"))
        nic = "virtio-net-pci" if sc["net"] == "virtio" else "e1000"
        tls = os.path.abspath(os.path.join(os.path.dirname(__file__), "https_once.py"))
        args += ["-netdev", f"user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:python3 {srv},guestfwd=tcp:10.0.2.100:443-cmd:python3 {tls} --chain {os.path.abspath("ci-out/tlschain")},guestfwd=tcp:10.0.2.100:8443-cmd:python3 {tls} --self {os.path.abspath("ci-out/tlschain")},hostfwd=tcp:127.0.0.1:18081-:8080,hostfwd=udp:127.0.0.1:18083-:9000",
                 "-device", f"{nic},netdev=n0"]
    else:
        args += ["-nic", "none"]
    qmp_path = os.path.join(d, "qmp.sock")
    if sc.get("flash"):                             # флешка FAT32 (ci/make_flash.sh) - свежая копия
        fl = os.path.join(d, "flash.img"); shutil.copyfile("ci-out/flash.img", fl)
        args += ["-drive", f"if=none,id=stick,format=raw,file={fl}"]
    args += sc.get("extra", [])
    if sc.get("gui") or sc.get("qmp"):
        if os.path.exists(qmp_path): os.unlink(qmp_path)
    if sc.get("qmp") and not sc.get("gui"):
        args += ["-qmp", f"unix:{qmp_path},server=on,wait=off"]
    if sc.get("gui"):
        args += ["-audiodev", "none,id=snd0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0"]
        args += ["-device", "virtio-gpu-pci", "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
                 "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = b""; t0 = time.time(); err = None

    def pump(sec):
        nonlocal log
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([p.stdout], [], [], 0.1)
            if r:
                b = os.read(p.stdout.fileno(), 65536)
                if not b: break
                log += b

    def wait(rx, sec):
        end = time.time() + sec
        while time.time() < end:
            if re.search(rx.encode(), log): return True
            pump(0.2)
        return False

    try:
        if not wait(READY, 60):
            err = "не дождались готовности"
        else:
            text = log.decode(errors="replace")
            for rx in COMMON + sc["must"]:
                if not re.search(rx, text): err = f"нет «{rx}»"; break
            q = Qmp(qmp_path) if (sc.get("gui") or sc.get("qmp")) and not err else None
            if not err:
                for cmd, must in sc["cmds"]:
                    start = len(log)
                    if cmd.startswith("@sleep "):
                        pump(float(cmd[7:])); continue
                    if cmd.startswith("@http "):                 # запрос с хозяина к серверу в гостевой (hostfwd)
                        port, path = cmd[6:].split()
                        log += host_http(int(port), path).encode()
                        part = log[start:].decode(errors="replace")
                        for rx in must:
                            if not re.search(rx, part): err = f"{cmd}: нет «{rx}»"; break
                        if err: break
                        continue
                    if cmd.startswith("@exit "):                 # команда выключает машину: QEMU завершается сам с кодом 0
                        for ch in cmd[6:] + "\r":
                            p.stdin.write(ch.encode()); p.stdin.flush(); time.sleep(0.01)
                        end = time.time() + 20
                        while p.poll() is None and time.time() < end: pump(0.3)
                        pump(0.3)
                        part = log[start:].decode(errors="replace")
                        if p.poll() is None: err = f"{cmd}: машина не выключилась за 20 с"; break
                        if p.returncode != 0: err = f"{cmd}: QEMU завершился с кодом {p.returncode}"; break
                        for rx in must:
                            if not re.search(rx, part): err = f"{cmd}: нет «{rx}»"; break
                        if err: break
                        continue
                    if cmd.startswith("@udp "):                  # датаграмма с хозяина программе в гостевой (hostfwd udp)
                        port, msg = cmd[5:].split(None, 1)
                        s_ = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s_.settimeout(10)
                        try:
                            s_.sendto(msg.encode(), ("127.0.0.1", int(port))); data, _ = s_.recvfrom(2048)
                            log += f"[HOST] udp reply '{data.decode(errors='replace')}'\n".encode()
                        except Exception as e:
                            log += f"[HOST] udp error {e}\n".encode()
                        s_.close()
                        part = log[start:].decode(errors="replace")
                        for rx in must:
                            if not re.search(rx, part): err = f"{cmd}: нет «{rx}»"; break
                        if err: break
                        continue
                    if cmd.startswith("@shot "):
                        ppm = os.path.abspath(os.path.join(d, cmd[6:] + ".ppm"))
                        q.cmd("screendump", filename=ppm); time.sleep(0.5)
                        if os.path.exists(ppm):
                            w, h, colors = ppm_to_png(ppm, ppm[:-4] + ".png"); os.unlink(ppm)
                            if sc.get("shot_size") and ((w, h) != sc["shot_size"] or colors < 8):
                                err = f"снимок {cmd[6:]}: {w}x{h}, цветов {colors} - экран не нарисован"; break
                        else: err = f"снимок {cmd[6:]} не получен"; break
                        continue
                    if cmd.startswith("@tap "):
                        x, y = (int(v) for v in cmd[5:].split())
                        ev = [{"type": "abs", "data": {"axis": "x", "value": x}}, {"type": "abs", "data": {"axis": "y", "value": y}}]
                        q.cmd("input-send-event", events=ev)
                        q.cmd("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": True}}])
                        q.cmd("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": False}}])
                        pump(0.5); continue
                    if cmd.startswith("@key "):
                        q.cmd("send-key", keys=[{"type": "qcode", "data": cmd[5:]}]); pump(0.5); continue
                    if cmd.startswith("@type "):                 # набор без Enter в конце
                        for ch in cmd[6:]:
                            q.cmd("send-key", keys=[{"type": "qcode", "data": QCODE.get(ch, ch)}]); time.sleep(0.05)
                        pump(0.3); continue
                    if cmd.startswith("@keys "):
                        for ch in cmd[6:] + "\r":
                            q.cmd("send-key", keys=[{"type": "qcode", "data": QCODE.get(ch, ch)}]); time.sleep(0.05)
                    else:
                        for ch in cmd + "\r":
                            p.stdin.write(ch.encode()); p.stdin.flush(); time.sleep(0.01)
                    end = time.time() + 60
                    while time.time() < end:                 # ждать, пока сбудется всё нужное (или 60 с)
                        part = log[start:].decode(errors="replace")
                        if all(re.search(rx, part) for rx in must) or re.search(r"RESULT: FAIL|exception on core", part): break
                        pump(0.3)
                    pump(0.5)
                    part = log[start:].decode(errors="replace")
                    for rx in must:
                        if not re.search(rx, part): err = f"{cmd}: нет «{rx}»"; break
                    if err: break
            if not err:
                text = log.decode(errors="replace")
                forbid = [x for x in NO_ERR if not (sc.get("fault") and x == r"exception on core") and x not in sc.get("allow", [])]
                for rx in forbid:
                    if re.search(rx, text): err = f"в журнале «{rx}»"; break
    finally:
        p.kill(); p.wait()
        open(os.path.join(d, "serial.log"), "wb").write(log)
        for im in ("disk.img", "flash.img"):
            if os.path.exists(os.path.join(d, im)): os.unlink(os.path.join(d, im))
        dbg = os.path.join(d, "qemu-debug.log")
        if os.path.exists(dbg):                     # начало журнала исключений QEMU - самое ценное, остальное - тики
            with open(dbg, "rb") as f: head = f.read(200000)
            with open(dbg, "wb") as f: f.write(head)
    res = {"name": sc["name"], "ok": err is None, "error": err, "seconds": round(time.time() - t0, 1)}
    json.dump(res, open(os.path.join(d, "result.json"), "w"), ensure_ascii=False)
    print(("OK  " if res["ok"] else "FAIL"), sc["name"], res["seconds"], "с", err or "")
    return res["ok"]


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "ci-out"
    ok = all([run(s, out) for s in SC])
    sys.exit(0 if ok else 1)
