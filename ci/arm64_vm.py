#!/usr/bin/env python3
"""Стенд ARM64: QEMU virt (эмуляция TCG), последовательный порт на stdio. Каждый сценарий - машина с другим
железом (GICv2/GICv3, число ядер, ОЗУ, процессор, вход на EL1 или EL2); ядро обязано найти всё по дереву устройств.
Итог - ci-out/arm64-<имя>/serial.log и result.json."""
import json, os, re, select, subprocess, sys, time

READY = r"Ready \(platform layer\)"
COMMON = [r"MMU on: identity map", r"generic timer verified: ([5-9]\d|1[0-4]\d) ticks in 200 ms"]
NO_ERR = [r"FATAL", r"exception on core", r"did not come up", r"CPU_ON .* failed"]

SC = [
    dict(name="arm64-gicv3", m="virt,gic-version=3", cpu="cortex-a72", smp=4, mem="1G",
         must=[r"GICv3", r"Processors online: 4 of 4 \(PSCI via hvc\)", r"working timer interrupt: 4 of 4", r"entered at EL1", r"RAM: 1024 MB"],
         cmds=[("smp", [r"core 3: MPIDR 0x0+3, online, ticks [1-9]"]), ("mem", [r"RAM 0x0+40000000 \.\. 0x0+80000000 \(1024 MB\)"])]),
    dict(name="arm64-gicv2", m="virt,gic-version=2", cpu="cortex-a53", smp=2, mem="512M",
         must=[r"GICv2", r"Processors online: 2 of 2", r"working timer interrupt: 2 of 2", r"RAM: 512 MB"],
         cmds=[("smp", [r"core 1: .*online"])]),
    dict(name="arm64-el2-8cpu", m="virt,gic-version=3,virtualization=on", cpu="cortex-a57", smp=8, mem="3G",
         must=[r"entered at EL2", r"Processors online: 8 of 8", r"working timer interrupt: 8 of 8", r"RAM: 3072 MB"],
         cmds=[]),
    dict(name="arm64-fault", m="virt,gic-version=3", cpu="cortex-a72", smp=1, mem="256M",
         must=[r"Processors online: 1 of 1"],
         cmds=[("fault", [r"exception on core 0: data abort .*", r"FAR 0x0000F00000000000"])], fault=True),
]


def run(sc, out):
    d = os.path.join(out, sc["name"]); os.makedirs(d, exist_ok=True)
    args = ["qemu-system-aarch64", "-M", sc["m"], "-cpu", sc["cpu"], "-smp", str(sc["smp"]), "-m", sc["mem"],
            "-nographic", "-monitor", "none", "-serial", "stdio", "-kernel", "kira-arm64.elf",
            "-d", "int,guest_errors,unimp", "-D", os.path.join(d, "qemu-debug.log")]
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
            if not err:
                for cmd, must in sc["cmds"]:
                    start = len(log)
                    for ch in cmd + "\r":
                        p.stdin.write(ch.encode()); p.stdin.flush(); time.sleep(0.01)
                    pump(1.5)
                    part = log[start:].decode(errors="replace")
                    for rx in must:
                        if not re.search(rx, part): err = f"{cmd}: нет «{rx}»"; break
                    if err: break
            if not err:
                text = log.decode(errors="replace")
                forbid = [x for x in NO_ERR if not (sc.get("fault") and x == r"exception on core")]
                for rx in forbid:
                    if re.search(rx, text): err = f"в журнале «{rx}»"; break
    finally:
        p.kill(); p.wait()
        open(os.path.join(d, "serial.log"), "wb").write(log)
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
