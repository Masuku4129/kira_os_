#!/usr/bin/env python3
"""Стенд Киры: запускает kira.iso в QEMU (KVM, если есть), говорит с консолью через последовательный порт,
снимает экран через QMP и проверяет журнал. Один запуск = один сценарий из ci/scenarios.py.

    python3 ci/kira_vm.py --scenario boot-virtio --out ci-out
    python3 ci/kira_vm.py --list

Результат: ci-out/<сценарий>/serial.log, *.png, result.json; код выхода 0 - сценарий пройден.
"""
import argparse, json, os, re, select, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenarios  # noqa: E402

def host_http(port, path):
    """Запрос с машины-хозяина к серверу в гостевой системе (hostfwd): строка итога для журнала."""
    import urllib.request
    t0 = time.time()
    try:
        body = urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=30).read()
        first = body.split(b"\n", 1)[0].decode(errors="replace")
        lines = body.count(b" kira tcp window\n")
        return f"[HOST] http {path}: {len(body)} bytes, first line '{first}', window lines {lines}\n"
    except Exception as e:
        return f"[HOST] http {path}: error {e} in {time.time() - t0:.1f} s\n"


def host_rcon(port, password, lines, wait=6.0):
    """Клиент удалённой консоли с хозяина (hostfwd): AUTH, команды, весь ответ - в журнал стенда."""
    t0 = time.time()
    out = b""
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=10)
        c.settimeout(1.0)
        def rd(sec):
            nonlocal out
            end = time.time() + sec
            while time.time() < end:
                try:
                    d = c.recv(4096)
                except socket.timeout:
                    continue
                if not d:
                    return False
                out += d
            return True
        rd(1.5)
        c.sendall(b"AUTH " + password.encode() + b"\n")
        rd(3.0)
        for ln in lines:
            c.sendall(ln.encode() + b"\n")
            rd(wait)
        c.close()
    except Exception as e:
        return f"[HOST] rcon: error {e} in {time.time() - t0:.1f} s\n"
    return "[HOST] rcon begin\n" + "".join("[RCON] " + l + "\n" for l in out.decode(errors="replace").splitlines()) + "[HOST] rcon end\n"


def host_udp(port, msg):
    """Датаграмма с хозяина в гостевую систему (hostfwd udp) и ответ: строка итога для журнала."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(10)
    try:
        s.sendto(msg.encode(), ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
        return f"[HOST] udp reply '{data.decode(errors='replace')}'\n"
    except Exception as e:
        return f"[HOST] udp error {e}\n"
    finally:
        s.close()


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def host_udp_big(port, size):
    """Большая датаграмма (size байт) хозяин -> гость: QEMU режет её на фрагменты IP, ядро Киры собирает; ответ - FNV-1a всех байт."""
    data = bytes((i * 7 + 3) & 255 for i in range(size))
    want = f"kira-udp: big {size} bytes fnv={fnv1a(data):08x}"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(10)
    try:
        s.sendto(data, ("127.0.0.1", port))
        reply, _ = s.recvfrom(2048)
        got = reply.decode(errors="replace")
        return f"[HOST] udp big {size}: {'reassembled ok' if got == want else 'MISMATCH'} (reply '{got}', want '{want}')\n"
    except Exception as e:
        return f"[HOST] udp big {size}: error {e}\n"
    finally:
        s.close()


def host6_serve(udp_port, tcp_port):
    """Серверы хозяина на [::1] (QEMU user-net отдаёт гостю хозяина как fec0::2): эхо UDP и короткий ответ HTTP по TCP."""
    import threading
    try:
        u = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM); u.bind(("::1", udp_port))
        t = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); t.bind(("::1", tcp_port)); t.listen(4)
    except OSError as e:
        return f"[HOST] ipv6 loopback unavailable: {e}\n"

    def udp_loop():
        while True:
            try:
                data, addr = u.recvfrom(2048)
                u.sendto(b"kira-udp6: " + data, addr)
            except OSError:
                return

    def tcp_loop():
        while True:
            try:
                c, _ = t.accept()
                c.recv(1024)
                c.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nkira-tcp6-ok\n")
                c.close()
            except OSError:
                return
    threading.Thread(target=udp_loop, daemon=True).start()
    threading.Thread(target=tcp_loop, daemon=True).start()
    return f"[HOST] ipv6 servers on [::1]: udp {udp_port}, tcp {tcp_port}\n"


PROMPT = "кира> ".encode()
FATAL = [rb"\[KIRA FATAL\]", rb"\[KIRA KERNEL PANIC\]", rb"\[KIRA WATCHDOG\] Halting"]


def env_missing(needs, kvm):
    """Чего не хватает хозяину для сценария (None - всё есть). Сценарий без нужной среды не падает, а пропускается с причиной."""
    for n in needs or []:
        if n == "kvm" and not kvm:
            return "нужен KVM (на TCG скорость не измеряется честно)"
        if n == "ipv6":
            try:
                t = socket.socket(socket.AF_INET6); t.bind(("::1", 0)); t.close()
            except OSError:
                return "у хозяина нет IPv6 (loopback ::1 недоступен)"
        if n == "dns":
            try:
                socket.getaddrinfo("example.com", 80)
            except OSError:
                return "у хозяина нет DNS/интернета (example.com не находится)"
        if n == "secureboot":                           # доверенная загрузка (А2): OVMF с Secure Boot, ключи и образы создаёт ci/sb_fixture.py
            import sb_fixture
            if not os.path.exists("kira.bin"):
                return "нет kira.bin (соберите ядро)"
            why = sb_fixture.missing()
            if why:
                return "доверенная загрузка: " + why
        if n == "swtpm" and not shutil.which("swtpm"):
            return "нет swtpm (apt install swtpm swtpm-tools): измеряемая загрузка проверяется на эмулируемом TPM"
        if n == "initrd" and not os.environ.get("KIRA_INITRD"):
            return "образ собран без initrd.img (задайте KIRA_INITRD при сборке и запуске)"
    return None


class Fail(Exception):
    pass


class VM:
    def __init__(self, sc, out, iso, disk, kvm, timeout_scale):
        self.sc, self.out, self.iso, self.kvm, self.k = sc, out, iso, kvm, timeout_scale
        if sc.get("limbo"): self.k = max(self.k, 8.0)       # эмуляция одним потоком в разы медленнее KVM
        self.log = bytearray()
        self.notes = []
        self.prompt_ready = False
        self.prompt_search_from = 0
        os.makedirs(out, exist_ok=True)
        self.disk = os.path.join(out, "disk.img")
        shutil.copyfile(disk, self.disk)
        if sc.get("files"):                         # тестовые файлы (ci/mkfiles.py) - на диск машины
            import mkfiles
            for dst, src in mkfiles.make(os.path.join(out, "files")).items():
                subprocess.run(["tools/kfstool", self.disk, "put", dst, src], check=True, stdout=subprocess.DEVNULL)
        for dst, src in sc.get("put", {}).items():  # файлы хозяина на диск машины (например, семя опыта Разума)
            subprocess.run(["tools/kfstool", self.disk, "put", dst, src], check=True, stdout=subprocess.DEVNULL)
        if sc.get("tls_chain"):                     # тестовая цепочка: корневой - единственный доверенный на этом диске
            chain = os.path.abspath("ci-out/tlschain")
            subprocess.run([sys.executable, "-I", "ci/mkchain.py", chain], check=True, stdout=subprocess.DEVNULL)
            subprocess.run(["tools/kfstool", self.disk, "put", "/etc/ssl/roots.der", os.path.join(chain, "root.der")], check=True, stdout=subprocess.DEVNULL)
        self.swtpm = None
        self.fixture_names = {}
        if sc.get("secureboot"):                    # прошивка с Secure Boot, ключи PK/KEK/db и образ ESP (верный или испорченный) - на лету, в ci-out/secureboot
            import sb_fixture
            sbc = sc["secureboot"]
            fx = sb_fixture.build(os.path.join(os.path.dirname(os.path.abspath(out)), "secureboot"), "kira.bin", ".")
            self.fixture_names = fx["names"]
            self.sb_code = fx["code"]
            self.sb_vars = os.path.join(out, "OVMF_VARS.fd")         # прошивка пишет в VARS: у каждого запуска своя копия
            shutil.copyfile(fx["vars_" + sbc.get("vars", "secboot")], self.sb_vars)
            self.sb_esp = os.path.join(out, "esp.img")
            shutil.copyfile(fx["images"][sbc.get("image", "good")], self.sb_esp)
            if sbc.get("tpm"):                      # эмулируемый TPM 2.0: swtpm по сокету, QEMU подключает его как tpm-tis
                self.tpm_dir = os.path.join(out, "tpm")
                os.makedirs(self.tpm_dir, exist_ok=True)
                self.tpm_sock = os.path.join(out, "swtpm.sock")
                self.swtpm = subprocess.Popen(["swtpm", "socket", "--tpm2", "--tpmstate", "dir=" + self.tpm_dir,
                                               "--ctrl", "type=unixio,path=" + self.tpm_sock,
                                               "--log", "file=" + os.path.join(out, "swtpm.log") + ",level=5"],
                                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                end = time.time() + 10
                while not os.path.exists(self.tpm_sock) and time.time() < end:
                    time.sleep(0.05)
        self.daemons = []
        for argv in sc.get("daemons", []):          # процессы хозяина на время сценария (постоянный openssl s_server: билеты сессий живут между соединениями)
            chain = os.path.abspath("ci-out/tlschain")
            self.daemons.append(subprocess.Popen([a.replace("{chain}", chain) for a in argv], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if self.daemons: time.sleep(1.5)
        if sc.get("disk_mb"):
            with open(self.disk, "r+b") as f:
                f.truncate(sc["disk_mb"] * 1024 * 1024)
        self.spare = os.path.join(out, "spare.img")
        if sc.get("spare_mb"):                      # второй пустой диск (для драйверов Ring 3): файл из нулей, в extra он подставляется как {spare}
            with open(self.spare, "wb") as f:
                f.truncate(sc["spare_mb"] * 1024 * 1024)
        self.ser_path = os.path.join(out, "serial.sock")
        self.qmp_path = os.path.join(out, "qmp.sock")
        for p in (self.ser_path, self.qmp_path):
            if os.path.exists(p):
                os.unlink(p)

    # ---------- запуск ----------
    def sb_boot_args(self):
        """Загрузка по UEFI с Secure Boot: прошивка OVMF (CODE только для чтения, VARS - копия с ключами), ESP как диск AHCI, TPM по желанию."""
        a = ["-global", "driver=cfi.pflash01,property=secure,value=on",
             "-drive", f"if=pflash,format=raw,unit=0,file={self.sb_code},readonly=on",
             "-drive", f"if=pflash,format=raw,unit=1,file={self.sb_vars}",
             "-drive", f"if=none,id=esp,format=raw,file={self.sb_esp}",
             "-device", "ide-hd,drive=esp,bus=ide.0,bootindex=0"]
        if self.swtpm:
            a += ["-chardev", f"socket,id=chrtpm,path={self.tpm_sock}", "-tpmdev", "emulator,id=tpm0,chardev=chrtpm",
                  "-device", "tpm-tis,tpmdev=tpm0"]
        return a

    def boot_args(self):
        if self.sc.get("secureboot"):
            return self.sb_boot_args()
        sc_boot_bios = lambda c: c.get("boot") == "bios"
        # По умолчанию - kira.iso (GRUB). KIRA_BOOT=bios: собственный BIOS-загрузчик без GRUB (kira-bios.img на вторичной шине IDE, первая в порядке загрузки).
        if os.environ.get("KIRA_BOOT") == "bios" or sc_boot_bios(self.sc):
            img = self.sc.get("bios_img") or os.environ.get("KIRA_BIOS_IMG", "kira-bios.img")
            return ["-drive", f"file={img},format=raw,if=none,id=bootd,snapshot=on",
                    "-device", "ide-hd,drive=bootd,bus=ide.1,unit=0,bootindex=0"]
        if self.sc.get("boot") == "grub":                       # запасной образ на GRUB
            return ["-boot", "order=d", "-cdrom", os.environ.get("KIRA_GRUB_ISO", "kira-grub.iso")]
        return ["-boot", "order=d", "-cdrom", self.iso]

    def args(self):
        sc = self.sc
        a = ["qemu-system-x86_64", "-machine", sc.get("machine", "pc"), "-m", str(sc.get("mem", "1G")),
             "-smp", str(sc.get("smp", 4)), "-vga", sc.get("vga", "std"), "-display", "none",
             *self.boot_args(),
             "-chardev", f"socket,id=ser0,path={self.ser_path},server=on,wait=on", "-serial", "chardev:ser0",
             "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
             "-audiodev", "none,id=a0", "-d", "guest_errors", "-D", os.path.join(self.out, "qemu.log")]
        if not sc.get("reboot"):                    # сброс = выход QEMU (иначе виснущая перезагрузка не видна)
            a.append("-no-reboot")
        if sc.get("limbo"):                         # как Limbo на телефоне: эмуляция одним потоком, без KVM
            a += ["-accel", "tcg,thread=" + ("multi" if sc.get("mttcg") else "single"), "-cpu", "qemu64"]
        elif self.kvm:
            a += ["-enable-kvm", "-cpu", sc.get("cpu", "host")]
        else:
            a += ["-cpu", sc.get("cpu_tcg", "max")]
        disk = sc.get("disk", "virtio")
        d = self.disk
        if disk == "virtio":
            a += ["-drive", f"file={d},format=raw,if=none,id=kd0", "-device", "virtio-blk-pci,drive=kd0,disable-legacy=on"]
        elif disk == "ata":
            a += ["-drive", f"file={d},format=raw,if=ide,index=0"]
        elif disk == "sata":
            a += ["-drive", f"file={d},format=raw,if=none,id=kd0", "-device", "ahci,id=ahci0", "-device", "ide-hd,drive=kd0,bus=ahci0.0"]
        elif disk == "nvme":
            a += ["-drive", f"file={d},format=raw,if=none,id=kd0", "-device", "nvme,drive=kd0,serial=KIRA0001"]
        a += [x.replace("{spare}", self.spare) for x in sc.get("extra", [])]
        if sc.get("flash"):
            fl = os.path.join(self.out, "flash.img")
            shutil.copyfile(sc["flash"], fl)
            bus = sc.get("flash_bus") or ("usb-bus.0" if "-usb" in a else "xhci.0")
            port = ",port=1.1" if sc.get("hub") else ""
            a += ["-drive", f"if=none,id=stick,format=raw,file={fl}", "-device", f"usb-storage,bus={bus}{port},drive=stick,removable=on"]
        return a

    def start(self):
        self.qemu = subprocess.Popen(self.args(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while not os.path.exists(self.ser_path):
            if time.time() > deadline or self.qemu.poll() is not None:
                raise Fail("QEMU не запустился: " + (self.qemu.stdout.read() or b"").decode(errors="replace")[-2000:])
            time.sleep(0.05)
        while True:
            try:
                self.ser = socket.socket(socket.AF_UNIX)
                self.ser.connect(self.ser_path)
                break
            except OSError:
                if time.time() > deadline:
                    raise Fail("нет последовательного порта QEMU")
                time.sleep(0.05)
        self.ser.setblocking(False)
        self.t0 = time.time()
        deadline = time.time() + 10
        while True:
            try:
                self.qmp = socket.socket(socket.AF_UNIX)
                self.qmp.connect(self.qmp_path)
                break
            except OSError:
                if time.time() > deadline:
                    raise Fail("нет QMP")
                time.sleep(0.05)
        self.qmp.settimeout(10)
        self._qmp_read()
        self.qmp_cmd({"execute": "qmp_capabilities"})

    def restart(self, extra):
        """Вторая фаза сценария: тот же диск (disk.img), другие устройства. Журнал продолжается; phase_mark - где он начался."""
        for sk in (self.ser, self.qmp):
            try: sk.close()
            except OSError: pass
        for pth in (self.ser_path, self.qmp_path):
            if os.path.exists(pth):
                os.unlink(pth)
        self.sc = dict(self.sc, extra=extra)
        self.phase_mark = len(self.log)
        self.start()

    def stop(self):
        for p in getattr(self, "daemons", []) + ([self.swtpm] if getattr(self, "swtpm", None) else []):
            p.terminate()
        try:
            self.qmp_cmd({"execute": "quit"})
        except Exception:
            pass
        try:
            self.qemu.wait(timeout=10)
        except Exception:
            self.qemu.kill()
        with open(os.path.join(self.out, "serial.log"), "wb") as f:
            f.write(bytes(self.log))

    # ---------- QMP ----------
    def _qmp_read(self):
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.qmp.recv(65536)
            if not chunk:
                raise ConnectionError("QMP connection closed before a complete response")
            buf += chunk
        return [json.loads(l) for l in buf.decode().splitlines() if l.strip()]

    def qmp_cmd(self, c):
        self.qmp.sendall(json.dumps(c).encode() + b"\n")
        for _ in range(50):
            for m in self._qmp_read():
                if "return" in m or "error" in m:
                    return m
        return None

    def screenshot(self, name):
        ppm = os.path.abspath(os.path.join(self.out, name + ".ppm"))
        self.qmp_cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        time.sleep(0.3)
        png = ppm[:-4] + ".png"
        try:
            from PIL import Image
            Image.open(ppm).save(png)
            os.unlink(ppm)
        except Exception:
            pass

    # ---------- последовательный порт ----------
    def pump(self, wait):
        r, _, _ = select.select([self.ser], [], [], wait)
        if r:
            try:
                d = self.ser.recv(65536)
            except BlockingIOError:
                d = b""
            if d:
                self.log += d
                for pat in FATAL:
                    m = re.search(pat, bytes(self.log[-len(d) - 200:]))
                    if m:
                        self.drain(4.0)                 # дочитать аварийный дамп (регистры, стек, причина)
                        i = bytes(self.log).rfind(m.group(0))
                        tail = bytes(self.log[i:i + 3000]).decode(errors="replace")
                        raise Fail("авария ядра:\n" + tail)
        if self.qemu.poll() is not None:
            raise Fail(f"QEMU завершился (код {self.qemu.returncode})")

    def drain(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.ser], [], [], 0.2)
            if r:
                try:
                    d = self.ser.recv(65536)
                except (BlockingIOError, OSError):
                    d = b""
                if not d and self.qemu.poll() is not None:
                    break
                self.log += d

    def expect(self, pattern, timeout, since):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        end = time.time() + timeout * self.k
        while time.time() < end:
            m = rx.search(bytes(self.log[since:]))
            if m:
                if rx.pattern == re.escape(PROMPT.decode()).encode():
                    self.prompt_ready = True
                return since + m.end()
            self.pump(0.2)
        raise Fail(f"не дождались «{pattern}» за {timeout * self.k:.0f} с")

    def send(self, text):
        # Сначала забираем хвост предыдущего ответа. Он может содержать prompt,
        # если предыдущий сценарный шаг ждал только строку приложения, а не shell.
        self.drain(0.05)
        if not self.prompt_ready and PROMPT in bytes(self.log[self.prompt_search_from:]):
            self.prompt_ready = True
        if not self.prompt_ready:
            self.expect(re.escape(PROMPT.decode()), 90, self.prompt_search_from)
        self.drain(0.05)
        self.prompt_search_from = len(self.log)
        self.prompt_ready = False
        self.ser.setblocking(True)
        for ch in text.encode():
            self.ser.sendall(bytes([ch]))
            time.sleep(0.004)              # консоль читает порт порциями: не переполнять FIFO UART
        self.ser.setblocking(False)

    def command(self, cmd, timeout=30):
        """Выполнить команду консоли; вернуть её вывод (до следующего приглашения)."""
        # Фоновые команды (_dg) ждут свою строку, а не приглашение. Не отправляем
        # следующую команду, пока предыдущий процесс не завершил вывод и shell не готов.
        if not self.prompt_ready:
            self.expect(re.escape(PROMPT.decode()), timeout, self.prompt_search_from)
        # expect() может остановиться на байтах приглашения, пока хвост UART-пакета
        # ещё лежит в сокете. Дочитываем его до отметки начала нового ответа.
        self.drain(0.05)
        start = len(self.log)
        self.send(cmd + "\r")
        t = time.time()
        end = self.expect(re.escape(PROMPT.decode()), timeout, start)
        out = bytes(self.log[start:end]).decode(errors="replace")
        self.notes.append({"cmd": cmd, "seconds": round(time.time() - t, 2)})
        return out


def disk_ops(vm, ops):
    """Правка диска ВНЕ системы, пока машина выключена (подмена файла журнала между запусками): get - вынуть файл в каталог сценария,
    xor - изменить один байт (смещение от начала файла), truncate - отрезать последние drop байт."""
    for op in ops:
        if "get" in op:
            with open(os.path.join(vm.out, op["as"]), "wb") as f:
                subprocess.run(["tools/kfstool", vm.disk, "cat", op["get"]], check=True, stdout=f)
            vm.log += f"[HOST] disk: {op['get']} -> {op['as']}\n".encode()
        elif "xor" in op or "truncate" in op:
            path = op.get("xor") or op["truncate"]
            tmp = os.path.join(vm.out, "tamper.tmp")
            with open(tmp, "wb") as f:
                subprocess.run(["tools/kfstool", vm.disk, "cat", path], check=True, stdout=f)
            data = bytearray(open(tmp, "rb").read())
            if "xor" in op:
                data[op["offset"]] ^= op.get("mask", 1)
                what = f"byte {op['offset']} xor {op.get('mask', 1)}"
            else:
                del data[len(data) - op["drop"]:]
                what = f"last {op['drop']} bytes removed"
            open(tmp, "wb").write(data)
            subprocess.run(["tools/kfstool", vm.disk, "put", path, tmp], check=True, stdout=subprocess.DEVNULL)
            vm.log += f"[HOST] disk: {path} edited outside the system: {what}\n".encode()


def subst(v, names):
    """<<имя>> и <<имя:N>> (первые N знаков) в строках шага -> значения, снятые шагом capture; неизвестное имя - ошибка сценария."""
    if isinstance(v, str):
        def rep(m):
            nm, _, cut = m.group(1).partition(":")
            if nm not in names:
                raise Fail(f"в сценарии нет значения «{nm}» (его даёт шаг capture)")
            return names[nm][:int(cut)] if cut else names[nm]
        return re.sub(r"<<([A-Za-z0-9_]+(?::\d+)?)>>", rep, v)
    if isinstance(v, list):
        return [subst(x, names) for x in v]
    if isinstance(v, dict):
        return {k: subst(x, names) for k, x in v.items()}
    return v


def run(sc, args):
    out = os.path.join(args.out, sc["name"])
    vm = VM(sc, out, args.iso, args.disk, args.kvm, args.timeout_scale)
    result = {"name": sc["name"], "ok": False, "kvm": args.kvm, "checks": [], "error": None}
    t0 = time.time()
    try:
        vm.start()
        if sc.get("no_prompt"):                  # машина обязана НЕ загрузить систему (отказ доверенной загрузки): приглашения нет, шаги ждут строки журнала
            result["boot_seconds"] = None
            time.sleep(1.0)
        else:
            boot = vm.expect(re.escape(PROMPT.decode()), sc.get("boot_timeout", 90), 0)
            result["boot_seconds"] = round(time.time() - t0, 1)
            time.sleep(1.0)
        vm.screenshot("00-boot")
        mark = 0
        names = dict(vm.fixture_names)
        names["out"] = vm.out
        for i, st in enumerate(sc["steps"]):
            if "capture" not in st:
                st = subst(st, names)
            if "cmd" in st:
                cmd = st["cmd"]
                outp = vm.command(cmd, st.get("timeout", 30))
                for m in st.get("must", []):
                    ok = re.search(m, outp) is not None
                    result["checks"].append({"step": cmd, "must": m, "ok": ok})
                    if not ok:
                        raise Fail(f"«{cmd}»: в выводе нет «{m}»\n{outp[-1500:]}")
                for f in st.get("forbid", []):
                    bad = re.search(f, outp)
                    result["checks"].append({"step": cmd, "forbid": f, "ok": not bad})
                    if bad:
                        raise Fail(f"«{cmd}»: в выводе «{bad.group(0)}»\n{outp[-1500:]}")
            elif "send" in st:                        # без ожидания приглашения (нагрузка, запуск программ)
                mark = len(vm.log)
                vm.send(st["send"] + "\r")
            elif "type" in st:                        # ввод запущенной программе (её читает стандартный ввод): оболочка приглашения не печатает, ждать его нельзя
                vm.drain(0.05)
                vm.ser.setblocking(True)
                for ch in (st["type"] + "\r").encode():
                    vm.ser.sendall(bytes([ch]))
                    time.sleep(0.004)
                mark = len(vm.log)
            elif "capture" in st:                     # значения из журнала после последнего send (первая группа регулярного выражения)
                for nm, rx in st["capture"].items():
                    m = re.search(rx.encode(), bytes(vm.log[mark:]))
                    result["checks"].append({"capture": nm, "ok": m is not None})
                    if not m:
                        raise Fail(f"capture «{nm}»: в журнале нет «{rx}»")
                    names[nm] = m.group(1).decode()
            elif "expect" in st:                      # ждать строку в журнале после последнего send
                vm.expect(st["expect"], st.get("timeout", 60), mark)
                result["checks"].append({"expect": st["expect"], "ok": True})
            elif "exit" in st:                        # команда должна выключить машину: QEMU завершается сам с кодом 0
                vm.send(st["exit"] + "\r")
                end_t = time.time() + st.get("within", 20) * args.timeout_scale
                while vm.qemu.poll() is None and time.time() < end_t:
                    vm.drain(0.5)
                if vm.qemu.poll() is None:
                    raise Fail(f"«{st['exit']}»: машина не выключилась за {st.get('within', 20)} с")
                vm.drain(0.5)
                if vm.qemu.returncode != 0:
                    raise Fail(f"«{st['exit']}»: QEMU завершился с кодом {vm.qemu.returncode}")
                vm.exited = True
                result["checks"].append({"exit": st["exit"], "ok": True})
            elif "qmp_exit" in st:                    # команда QMP (например system_powerdown - нажатие кнопки питания), после неё машина должна выключиться сама
                r = vm.qmp_cmd({"execute": st["qmp_exit"]})
                if r is None or "error" in r:
                    raise Fail(f"QMP «{st['qmp_exit']}»: {r}")
                end_t = time.time() + st.get("within", 30) * args.timeout_scale
                while vm.qemu.poll() is None and time.time() < end_t:
                    vm.drain(0.5)
                if vm.qemu.poll() is None:
                    raise Fail(f"после «{st['qmp_exit']}» машина не выключилась за {st.get('within', 30)} с")
                vm.drain(0.5)
                if vm.qemu.returncode != 0:
                    raise Fail(f"после «{st['qmp_exit']}» QEMU завершился с кодом {vm.qemu.returncode}")
                vm.exited = True
                result["checks"].append({"qmp_exit": st["qmp_exit"], "ok": True})
            elif "qmp" in st:                         # произвольная команда QMP без выключения машины (например system_wakeup после сна S3); wait_status - дождаться состояния машины (query-status)
                if "wait_status" in st:
                    end_t = time.time() + st.get("within", 60) * args.timeout_scale
                    got = None
                    while time.time() < end_t:
                        r = vm.qmp_cmd({"execute": "query-status"})
                        got = (r or {}).get("return", {}).get("status")
                        if got == st["wait_status"]:
                            break
                        vm.drain(0.3)
                    if got != st["wait_status"]:
                        raise Fail(f"QMP: машина не перешла в состояние «{st['wait_status']}» за {st.get('within', 60)} с (сейчас «{got}»)")
                    result["checks"].append({"qmp_status": st["wait_status"], "ok": True})
                msg = {"execute": st["qmp"]}
                if "args" in st:
                    msg["arguments"] = st["args"]
                r = vm.qmp_cmd(msg)
                if r is None or "error" in r:
                    raise Fail(f"QMP «{st['qmp']}»: {r}")
                result["checks"].append({"qmp": st["qmp"], "ok": True})
                if "after_status" in st:              # состояние машины после команды (например running после пробуждения)
                    end_t = time.time() + st.get("within", 60) * args.timeout_scale
                    got = None
                    while time.time() < end_t:
                        r = vm.qmp_cmd({"execute": "query-status"})
                        got = (r or {}).get("return", {}).get("status")
                        if got == st["after_status"]:
                            break
                        vm.drain(0.3)
                    if got != st["after_status"]:
                        raise Fail(f"QMP: после «{st['qmp']}» машина не перешла в «{st['after_status']}» (сейчас «{got}»)")
                    result["checks"].append({"qmp_after_status": st["after_status"], "ok": True})
            elif "num" in st:                         # числа, снятые capture: [больше, меньше, мин. разность, макс. разность]: names[больше] - names[меньше] в пределах
                hi, lo, dmin, dmax = st["num"]
                d = int(names[hi]) - int(names[lo])
                ok = dmin <= d <= dmax
                result["checks"].append({"num": f"{hi}-{lo}={d}", "range": [dmin, dmax], "ok": ok})
                if not ok:
                    raise Fail(f"числа: {hi} - {lo} = {d}, ожидалось от {dmin} до {dmax}")
            elif "relaunch" in st:                  # выключить машину и поднять её снова на ТОМ ЖЕ диске с другим набором устройств (extra):
                vm.send("poweroff\r")                  # драйвер, установленный в первой фазе, нужен устройству, которого в первой фазе не было
                end_t = time.time() + 30 * args.timeout_scale
                while vm.qemu.poll() is None and time.time() < end_t:
                    vm.drain(0.5)
                if vm.qemu.poll() is None or vm.qemu.returncode != 0:
                    raise Fail("relaunch: машина не выключилась сама")
                vm.drain(0.5)
                disk_ops(vm, st.get("disk_ops", []))
                vm.restart(st["relaunch"])
                vm.expect(re.escape(PROMPT.decode()), sc.get("boot_timeout", 90), vm.phase_mark)
                time.sleep(1.0)
                mark = len(vm.log)
                result["checks"].append({"relaunch": True, "ok": True})
            elif "crash" in st:                       # обрыв питания: машину убивают без выключения и поднимают снова на ТОМ ЖЕ диске (дополнительные устройства - в st["crash"])
                vm.qemu.kill()
                vm.qemu.wait(timeout=10)
                disk_ops(vm, st.get("disk_ops", []))
                vm.restart(st["crash"])
                vm.expect(re.escape(PROMPT.decode()), sc.get("boot_timeout", 90), vm.phase_mark)
                time.sleep(1.0)
                mark = len(vm.log)
                result["checks"].append({"crash": True, "ok": True})
            elif "pointer" in st:                     # поставить указатель в точку экрана: PS/2-мышь отдаёт только смещения, поэтому идём шагами
                px, py = st["pointer"][0], st["pointer"][1]
                cur = getattr(vm, "ptr", (640, 360))
                dx, dy = px - cur[0], py - cur[1]
                n = max(abs(dx), abs(dy)) // 40 + 1
                for k in range(n):
                    sx = dx * (k + 1) // n - dx * k // n
                    sy = dy * (k + 1) // n - dy * k // n
                    vm.qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
                        {"type": "rel", "data": {"axis": "x", "value": sx}}, {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
                    time.sleep(0.02)
                vm.ptr = (px, py)
                vm.pump(0.6)
            elif "click" in st:                       # щелчок кнопкой мыши (left/right) там, где стоит указатель (сначала шаг "pointer")
                btn = st["click"] if isinstance(st["click"], str) else "left"
                for down in (True, False):
                    vm.qmp_cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": btn}}]}})
                    time.sleep(0.08)
                vm.pump(0.4)
            elif "mouse" in st:                       # поток движения мыши через QMP: кружит по экрану; hz - событий в секунду, step - пикселей за событие
                m = st["mouse"]
                hz, secs, step = m.get("hz", 125), m.get("seconds", 10), m.get("step", 8)
                kind = m.get("kind", "rel")
                import math
                t_end = time.time() + secs * args.timeout_scale
                t_next = time.time(); k = 0; sent = 0
                while time.time() < t_end:
                    ang = k * 0.05
                    if kind == "abs":
                        ev = [{"type": "abs", "data": {"axis": "x", "value": int(16000 + 12000 * math.cos(ang))}},
                              {"type": "abs", "data": {"axis": "y", "value": int(16000 + 12000 * math.sin(ang))}}]
                    else:
                        ev = [{"type": "rel", "data": {"axis": "x", "value": int(step * math.cos(ang))}},
                              {"type": "rel", "data": {"axis": "y", "value": int(step * math.sin(ang))}}]
                    vm.qmp_cmd({"execute": "input-send-event", "arguments": {"events": ev}})
                    k += 1; sent += 1
                    t_next += 1.0 / hz
                    d = t_next - time.time()
                    if d > 0: time.sleep(d)
                    else: vm.drain(0)
                vm.log += f"[HOST] mouse: sent {sent} move events in {secs * args.timeout_scale} s\n".encode()
                vm.pump(0.5)
            elif "combo" in st:                       # одновременное нажатие (Alt+Shift - раскладка)
                vm.qmp_cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in st["combo"]]}})
                vm.pump(0.3)
            elif "keys" in st:                        # набор на клавиатуре машины (окну в фокусе), затем клавиши qcode
                qc = {" ": "spc", "\n": "ret", ".": "dot", "-": "minus", "/": "slash"}
                for ch in st["keys"]:
                    vm.qmp_cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qc.get(ch, ch)}]}})
                    time.sleep(0.05)
                for k in st.get("then", []):
                    vm.qmp_cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
                    time.sleep(0.05)
                vm.pump(0.5)
            elif "host_http" in st:                   # запрос с хозяина к серверу Киры (hostfwd)
                port, path = st["host_http"]
                vm.log += host_http(port, path).encode()
            elif "host_rcon" in st:                   # удалённая консоль: клиент на хозяине через hostfwd
                port, pw, cmds = st["host_rcon"]
                vm.log += host_rcon(port, pw, cmds).encode()
            elif "host_udp" in st:                    # датаграмма с хозяина программе Киры (hostfwd udp)
                port, msg = st["host_udp"]
                vm.log += host_udp(port, msg).encode()
            elif "host_udp_big" in st:                # большая датаграмма хозяин -> гость (фрагменты IP)
                port, size = st["host_udp_big"]
                vm.log += host_udp_big(port, size).encode()
            elif "host6_serve" in st:                 # серверы хозяина на [::1] для проверки IPv6 гостя
                up, tp = st["host6_serve"]
                vm.log += host6_serve(up, tp).encode()
            elif "host_run" in st:                    # программа хозяина (например tools/auditverify.py на файлах, вынутых из диска): код возврата и вывод проверяются
                hr = st["host_run"]
                argv = [a.replace("{out}", vm.out) for a in hr["argv"]]
                p = subprocess.run(argv, capture_output=True, text=True, timeout=120)
                vm.log += ("[HOST-RUN] " + " ".join(os.path.basename(a) if a.startswith("/") else a for a in argv) + "\n").encode()
                vm.log += "".join("[HOST-RUN] " + l + "\n" for l in (p.stdout + p.stderr).splitlines()).encode()
                okrc = p.returncode == hr.get("rc", 0)
                result["checks"].append({"host_run": argv[-1], "rc": p.returncode, "ok": okrc})
                if not okrc:
                    raise Fail(f"host_run {argv}: код {p.returncode}, ожидался {hr.get('rc', 0)}\n{(p.stdout + p.stderr)[-1500:]}")
                for m in hr.get("must", []):
                    ok = re.search(m, p.stdout + p.stderr) is not None
                    result["checks"].append({"host_run_must": m, "ok": ok})
                    if not ok:
                        raise Fail(f"host_run: в выводе нет «{m}»\n{(p.stdout + p.stderr)[-1500:]}")
            elif "save_log" in st:                    # строки журнала по образцу (первая группа) - в файл каталога сценария (например страницы экспорта audit export)
                sl = st["save_log"]
                lines = [m.group(1) for m in re.finditer(sl["pattern"].encode(), bytes(vm.log), re.M)]
                with open(os.path.join(vm.out, sl["as"]), "w") as f:
                    f.write("\n".join(l.decode(errors="replace") for l in lines) + "\n")
                result["checks"].append({"save_log": sl["as"], "lines": len(lines), "ok": len(lines) >= sl.get("min", 1)})
                if len(lines) < sl.get("min", 1):
                    raise Fail(f"save_log: найдено {len(lines)} строк по «{sl['pattern']}», нужно {sl.get('min', 1)}")
            elif "sleep" in st:
                end_t = time.time() + st["sleep"] * args.timeout_scale
                while time.time() < end_t:
                    vm.pump(0.2)
            elif "shot" in st:
                vm.screenshot(f"{i:02d}-{st['shot']}")
            elif "log" in st:                         # весь журнал с начала
                for m in st["log"]:
                    ok = re.search(m.encode(), bytes(vm.log)) is not None
                    result["checks"].append({"log": m, "ok": ok})
                    if not ok:
                        raise Fail(f"в журнале нет «{m}»")
            elif "log_forbid" in st:
                for m in st["log_forbid"]:
                    bad = re.search(m.encode(), bytes(vm.log))
                    result["checks"].append({"log_forbid": m, "ok": not bad})
                    if bad:
                        raise Fail(f"в журнале «{bad.group(0).decode(errors='replace')}»")
            elif "distinct" in st:                    # в журнале всех фаз не меньше count совпадений первой группы, и все значения разные (KASLR: база ядра при каждой загрузке своя)
                vals = [m.group(1).decode(errors="replace") for m in re.finditer(st["distinct"].encode(), bytes(vm.log))]
                result["checks"].append({"distinct": st["distinct"], "values": vals, "ok": len(vals) >= st.get("count", 2) and len(set(vals)) == len(vals)})
                if len(vals) < st.get("count", 2):
                    raise Fail(f"distinct: найдено {len(vals)} значений «{st['distinct']}», нужно {st.get('count', 2)}: {vals}")
                if len(set(vals)) != len(vals):
                    raise Fail(f"distinct: значения повторяются: {vals}")
            elif "responsive" in st:                  # система отвечает на команду не дольше заданного
                t = time.time()
                vm.command(st["responsive"], st.get("within", 5))
                result["checks"].append({"responsive": st["responsive"], "seconds": round(time.time() - t, 2), "ok": True})
        if not getattr(vm, "exited", False):
            vm.screenshot("99-end")
        result["ok"] = True
    except Fail as e:
        result["error"] = str(e)
        try:
            vm.screenshot("99-fail")
        except Exception:
            pass
    except Exception as e:
        result["error"] = f"{type(e).__name__}: {e}"
    finally:
        vm.stop()
        result["seconds"] = round(time.time() - t0, 1)
        result["commands"] = vm.notes
        with open(os.path.join(out, "result.json"), "w") as f:
            json.dump(result, f, ensure_ascii=False, indent=1)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", action="append", default=[])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--iso", default="kira.iso")
    ap.add_argument("--disk", default="kira.disk")
    ap.add_argument("--out", default="ci-out")
    ap.add_argument("--timeout-scale", type=float, default=None)
    a = ap.parse_args()
    if a.list:
        for s in scenarios.ALL:
            print(s["name"], "-", s.get("about", ""))
        return 0
    a.kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
    if a.timeout_scale is None:
        a.timeout_scale = 1.0 if a.kvm else 6.0
    chosen = scenarios.ALL if a.all else [s for s in scenarios.ALL if s["name"] in a.scenario]
    if not chosen:
        print("нет сценариев (см. --list)")
        return 2
    fails = 0
    summary = []
    skipped = 0
    for sc in chosen:
        why = env_missing(sc.get("needs"), a.kvm)
        if why:
            print(f"SKIP {sc['name']:<22} {why}", flush=True)
            summary.append({"name": sc["name"], "ok": None, "skipped": why})
            skipped += 1
            continue
        r = run(sc, a)
        mark = "OK  " if r["ok"] else "FAIL"
        line = f"{mark} {sc['name']:<22} {r['seconds']:>6} с" + (f"  загрузка {r.get('boot_seconds')} с" if r.get("boot_seconds") else "")
        print(line + ("" if r["ok"] else "\n     " + (r["error"] or "").replace("\n", "\n     ")), flush=True)
        summary.append(r)
        fails += not r["ok"]
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "summary.json"), "w") as f:
        json.dump(summary, f, ensure_ascii=False, indent=1)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
