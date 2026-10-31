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

PROMPT = "кира> ".encode()
FATAL = [rb"\[KIRA FATAL\]", rb"\[KIRA KERNEL PANIC\]", rb"\[KIRA WATCHDOG\] Halting"]


class Fail(Exception):
    pass


class VM:
    def __init__(self, sc, out, iso, disk, kvm, timeout_scale):
        self.sc, self.out, self.iso, self.kvm, self.k = sc, out, iso, kvm, timeout_scale
        if sc.get("limbo"): self.k = max(self.k, 8.0)       # эмуляция одним потоком в разы медленнее KVM
        self.log = bytearray()
        self.notes = []
        os.makedirs(out, exist_ok=True)
        self.disk = os.path.join(out, "disk.img")
        shutil.copyfile(disk, self.disk)
        if sc.get("disk_mb"):
            with open(self.disk, "r+b") as f:
                f.truncate(sc["disk_mb"] * 1024 * 1024)
        self.ser_path = os.path.join(out, "serial.sock")
        self.qmp_path = os.path.join(out, "qmp.sock")
        for p in (self.ser_path, self.qmp_path):
            if os.path.exists(p):
                os.unlink(p)

    # ---------- запуск ----------
    def args(self):
        sc = self.sc
        a = ["qemu-system-x86_64", "-machine", sc.get("machine", "pc"), "-m", str(sc.get("mem", "1G")),
             "-smp", str(sc.get("smp", 4)), "-vga", "std", "-display", "none", "-no-reboot",
             "-boot", "order=d", "-cdrom", self.iso,
             "-chardev", f"socket,id=ser0,path={self.ser_path},server=on,wait=on", "-serial", "chardev:ser0",
             "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
             "-audiodev", "none,id=a0", "-d", "guest_errors", "-D", os.path.join(self.out, "qemu.log")]
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
        a += sc.get("extra", [])
        if sc.get("flash"):
            fl = os.path.join(self.out, "flash.img")
            shutil.copyfile(sc["flash"], fl)
            bus = "usb-bus.0" if "-usb" in a else "xhci.0"
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

    def stop(self):
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
            buf += self.qmp.recv(65536)
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
                return since + m.end()
            self.pump(0.2)
        raise Fail(f"не дождались «{pattern}» за {timeout * self.k:.0f} с")

    def send(self, text):
        self.ser.setblocking(True)
        for ch in text.encode():
            self.ser.sendall(bytes([ch]))
            time.sleep(0.004)              # консоль читает порт порциями: не переполнять FIFO UART
        self.ser.setblocking(False)

    def command(self, cmd, timeout=30):
        """Выполнить команду консоли; вернуть её вывод (до следующего приглашения)."""
        start = len(self.log)
        self.send(cmd + "\r")
        t = time.time()
        end = self.expect(re.escape(PROMPT.decode()), timeout, start)
        out = bytes(self.log[start:end]).decode(errors="replace")
        self.notes.append({"cmd": cmd, "seconds": round(time.time() - t, 2)})
        return out


def run(sc, args):
    out = os.path.join(args.out, sc["name"])
    vm = VM(sc, out, args.iso, args.disk, args.kvm, args.timeout_scale)
    result = {"name": sc["name"], "ok": False, "kvm": args.kvm, "checks": [], "error": None}
    t0 = time.time()
    try:
        vm.start()
        boot = vm.expect(re.escape(PROMPT.decode()), sc.get("boot_timeout", 90), 0)
        result["boot_seconds"] = round(time.time() - t0, 1)
        time.sleep(1.0)
        vm.screenshot("00-boot")
        for i, st in enumerate(sc["steps"]):
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
                vm.send(st["send"] + "\r")
            elif "keys" in st:                        # набор на клавиатуре машины (окну в фокусе), затем клавиши qcode
                qc = {" ": "spc", "\n": "ret", ".": "dot", "-": "minus", "/": "slash"}
                for ch in st["keys"]:
                    vm.qmp_cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qc.get(ch, ch)}]}})
                    time.sleep(0.05)
                for k in st.get("then", []):
                    vm.qmp_cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
                    time.sleep(0.05)
                vm.pump(0.5)
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
            elif "responsive" in st:                  # система отвечает на команду не дольше заданного
                t = time.time()
                vm.command(st["responsive"], st.get("within", 5))
                result["checks"].append({"responsive": st["responsive"], "seconds": round(time.time() - t, 2), "ok": True})
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
    for sc in chosen:
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
