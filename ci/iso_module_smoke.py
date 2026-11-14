#!/usr/bin/env python3
"""Boot the release ISO with no attached hard disk and require its embedded KiraFS root."""
import pathlib
import subprocess
import tempfile
import time
import sys

iso = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "kira.iso").resolve()
if not iso.is_file():
    raise SystemExit("missing kira.iso")

with tempfile.TemporaryDirectory(prefix="kira-iso-smoke-") as tmp:
    log = pathlib.Path(tmp) / "serial.log"
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-cpu", "qemu64", "-m", "512M",
        "-cdrom", str(iso), "-boot", "d", "-display", "none", "-monitor", "none",
        "-serial", f"file:{log}", "-no-reboot", "-no-shutdown",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 35
        required = ("[KIRA DISK] kira-iso: embedded KiraFS", "[KIRA FS] KiraFS mounted:", "[KIRA DISPLAY] Built-in KIRA wallpaper installed.")
        while time.monotonic() < deadline:
            if log.exists():
                data = log.read_text(errors="replace")
                if all(marker in data for marker in required):
                    print("PASS: booted the ISO without an attached hard disk; embedded KiraFS mounted")
                    break
            if proc.poll() is not None:
                raise SystemExit(f"QEMU exited early with status {proc.returncode}")
            time.sleep(0.25)
        else:
            data = log.read_text(errors="replace") if log.exists() else ""
            raise SystemExit("embedded ISO disk did not mount within 35 seconds; serial log follows:\n" + data[-12000:])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
